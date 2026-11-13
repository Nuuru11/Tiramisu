// tiramisu/tests/test_pe_loader.cpp
// Offline proof for the trt PE64 loader on a Linux host:
//   1. map the real guest PE (clang-built) at its preferred base (no relocs)
//   2. map it again anywhere (delta != 0 -> DIR64/HIGHLOW relocation pass)
//   3. bind its imports through the module/resolver path
//   4. seal (W^X) and EXECUTE the guest entry under the Win64 ABI
//   5. verify the guest's import call landed in the host and the checksum
//      matches the same arithmetic computed in C
//   6. negative: corrupt DOS header -> TRT_ERR_BAD_DOS, no crash
// Console runs the identical loader through the UWP backend.
#include "../src/trt/trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

// ---------------- mmap backend ----------------
namespace {

struct extent { void* base; size_t len; };
extent g_extents[16];
int g_extent_count = 0;
bool g_ignore_preferred = false;  // reloc-path test flips this

void* linux_alloc(size_t bytes, u64 preferred_base) {
  if (g_ignore_preferred) preferred_base = 0;
  void* p = mmap(preferred_base ? (void*)(uintptr_t)preferred_base : nullptr,
                 bytes, PROT_READ | PROT_WRITE,
                 preferred_base ? (MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS)
                                : (MAP_PRIVATE | MAP_ANONYMOUS),
                 -1, 0);
  if (p == MAP_FAILED) return nullptr;
  if (g_extent_count < 16) {
    g_extents[g_extent_count].base = p;
    g_extents[g_extent_count].len = bytes;
    ++g_extent_count;
  }
  return p;
}

int linux_protect(void* base, size_t bytes, unsigned prot) {
  int p = 0;
  if (prot & TRT_PROT_R) p |= PROT_READ;
  if (prot & TRT_PROT_W) p |= PROT_WRITE;
  if (prot & TRT_PROT_X) p |= PROT_EXEC;
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  const uintptr_t b = (uintptr_t)base & ~(uintptr_t)(page - 1);
  const size_t len = ((uintptr_t)base + bytes - b + page - 1) & ~(page - 1);
  return mprotect((void*)b, len, p);
}

void linux_release(void* base, size_t bytes) {
  for (int i = 0; i < g_extent_count; ++i) {
    if (g_extents[i].base == base) {
      munmap(base, g_extents[i].len);
      g_extents[i].base = nullptr;
      return;
    }
  }
  (void)bytes;
}

void linux_log(const char* line) { printf("  [trt] %s\n", line); }

// ---------------- import plumbing ----------------
// SysV implementations the guest's imports forward into
u32 g_build_value = 0x0042u;
u32 g_log_tag = 0, g_log_value = 0;
bool g_log_called = false;

extern "C" u32 TRT_STDCALL sysv_query_build() { return g_build_value; }
extern "C" void TRT_STDCALL sysv_log_u32(u32 tag, u32 value) {
  g_log_tag = tag;
  g_log_value = value;
  g_log_called = true;
}

// The test fakes carry TRT_STDCALL = ms_abi (Win64) so the guest binds their
// addresses directly — the console-exact path (host and guest share Win64,
// no trampoline needed).

void* test_resolve(void* user, const char* module, const char* symbol,
                   u16 ordinal) {
  (void)user; (void)ordinal;
  if (strcmp(module, "tiramisu_host.dll") == 0) {
    if (strcmp(symbol, "TiramisuHostQueryBuild") == 0) return (void*)&sysv_query_build;
    if (strcmp(symbol, "TiramisuHostLogU32") == 0) return (void*)&sysv_log_u32;
  }
  return nullptr;
}

// call a Win64 function: shadow space + rcx/rdx/r8 args, result in rax
u64 win64_call(void* fn, u64 a, u64 b, u64 c) {
  u64 ret;
  asm volatile(
    "subq $48, %%rsp\n"       // keeps rsp 16-aligned; 40 bytes shadow+pad
    "movq %1, %%rcx\n"
    "movq %2, %%rdx\n"
    "movq %3, %%r8\n"
    "call *%4\n"
    "addq $48, %%rsp\n"
    : "=a"(ret)
    : "r"(a), "r"(b), "r"(c), "r"(fn)
    : "rcx", "rdx", "r8", "r9", "r10", "r11",
      "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "memory", "cc");
  return ret;
}

int failures = 0;
#define CHECK(cond, msg)                                              \
  do {                                                                \
    if (cond) { printf("  PASS %s\n", msg); }                          \
    else { printf("  FAIL %s\n", msg); ++failures; }                   \
  } while (0)

u8* read_file(const char* path, size_t* size) {
  FILE* f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  u8* buf = (u8*)malloc((size_t)n);
  if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return nullptr; }
  fclose(f);
  *size = (size_t)n;
  return buf;
}

// same arithmetic as the guest, computed host-side; the +5 is the pointer
// table read (3+2) — only correct when the DIR64 relocation pass ran
u32 expected_checksum(u32 build, u64 a, u64 b) {
  u32 x = build ^ 0x1DEAu;
  x += (u32)(a & 0xF) + (u32)(b & 0xF);
  x += 5u;
  for (volatile int i = 0; i < 16; ++i) x = x * 16777619u + 1u;
  return x;
}

int run_case(const char* label, const u8* file, size_t size, bool ignore_preferred) {
  printf("case: %s\n", label);
  g_ignore_preferred = ignore_preferred;
  trt_backend be = {&linux_alloc, &linux_protect, &linux_release, &linux_log, nullptr};
  trt_resolver rs = {nullptr, &test_resolve};
  trt_image* img = nullptr;
  int rc = trt_map_pe(&be, &rs, file, size, &img);
  CHECK(rc == TRT_OK, "map returns OK");
  if (rc != TRT_OK) return failures;
  CHECK(img->base != nullptr, "image base non-null");
  CHECK(img->entry_rva != 0, "entry rva non-zero");

  g_log_called = false; g_log_tag = 0; g_log_value = 0;
  CHECK(trt_seal_image(&be, img) == TRT_OK, "seal OK (W^X flip)");

  const u64 a = 0, b = 0;
  printf("  info exec at %llx (text+%llx)\n",
         (unsigned long long)(uintptr_t)(img->base + img->entry_rva),
         (unsigned long long)img->entry_rva);
  const u64 ret = win64_call(img->base + img->entry_rva, a, b, 0);
  CHECK(g_log_called, "guest import call reached the host");
  CHECK(g_log_tag == 0x1DEAu, "log tag == 0x1DEA");
  CHECK(g_log_value == g_build_value, "log value == build constant");
  CHECK((u32)ret == expected_checksum(g_build_value, a, b),
        "guest checksum matches host-side computation");
  char det[128];
  snprintf(det, sizeof(det), "entry returned 0x%llx", (unsigned long long)ret);
  printf("  info %s (base=%llx)\n", det, (unsigned long long)(uintptr_t)img->base);
  trt_unmap(&be, img);
  return failures;
}

} // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);  // survive SIGSEGV with logs intact
  if (argc < 3) {
    fprintf(stderr, "usage: %s <guest_hello.exe> <guest_hello_reloc.exe>\n", argv[0]);
    return 2;
  }
  size_t size = 0;
  u8* file = read_file(argv[1], &size);
  if (!file) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }
  printf("tiramisu offline loader test — guest %s (%zu bytes)\n", argv[1], size);

  size_t size2 = 0;
  u8* file2 = read_file(argv[2], &size2);
  if (!file2) {
    fprintf(stderr, "cannot read %s\n", argv[2]);
    return 2;
  }
  run_case("preferred base (delta == 0, no reloc pass)", file, size, false);
  // reloc guest: linked /BASE:0x1A00000 (internally consistent fixups);
  // backend ignores preferred -> delta != 0 -> DIR64 pass must run
  run_case("relocated base (delta != 0, full reloc pass)", file2, size2, true);

  // negative: corrupt MZ
  {
    trt_backend be = {&linux_alloc, &linux_protect, &linux_release, &linux_log, nullptr};
    trt_resolver rs = {nullptr, &test_resolve};
    trt_image* img = nullptr;
    file2[0] = 'X';
    const int rc = trt_map_pe(&be, &rs, file2, size2, &img);
    CHECK(rc == TRT_ERR_BAD_DOS, "corrupt MZ -> TRT_ERR_BAD_DOS");
    CHECK(img == nullptr, "no image leaked on error");
  }

  free(file2);
  free(file);
  printf(failures ? "\nRESULT: %d FAILURES\n" : "\nRESULT: ALL PASS\n",
         failures);
  return failures ? 1 : 0;
}
