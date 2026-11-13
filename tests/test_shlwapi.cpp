// tiramisu/tests/test_shlwapi.cpp
// The shlwapi bridge: a guest runs the path-assembly work every engine
// does at boot (Combine/FindFileName/Append/StripPath/extension helpers/
// IsRelative) through the IAT, plus VFS-backed existence checks.
#include "../src/trt/trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_user32(void);
int trt_bridge_register_shlwapi(void);
void trt_vfs_mount(const char* drive_prefix, const char* host_path);
}

// ---- Win64 -> SysV trampolines (TEST HARNESS ONLY) ----
// kernel32 bridge fns are host-ABI; see test_engine.cpp. The guest's
// file calls (CreateFileW/CloseHandle for the existence check) need
// marshalling here.
extern "C" void shlw_generic_w64_to_sysv(void);
asm(
  ".text\n"
  ".globl shlw_generic_w64_to_sysv\n"
  "shlw_generic_w64_to_sysv:\n"
  "  subq $104, %rsp\n"
  "  movq %rdi, 72(%rsp)\n"
  "  movq %rsi, 80(%rsp)\n"
  "  movq %rcx, %rdi\n"
  "  movq %rdx, %rsi\n"
  "  movq %r8, %rdx\n"
  "  movq %r9, %rcx\n"
  "  movq 144(%rsp), %r8\n"
  "  movq 152(%rsp), %r9\n"
  "  movq 160(%rsp), %rax\n"
  "  movq %rax, (%rsp)\n"
  "  movq 168(%rsp), %rax\n"
  "  movq %rax, 8(%rsp)\n"
  "  call *%r11\n"
  "  movq 72(%rsp), %rdi\n"
  "  movq 80(%rsp), %rsi\n"
  "  addq $104, %rsp\n"
  "  ret\n"
);
#define SHLW_TRAMP(sym, target)                      \
  extern "C" void sym();                             \
  asm(".globl " #sym "\n" #sym ":\n"                \
      "  movq $" #target ", %r11\n"                 \
      "  jmp shlw_generic_w64_to_sysv\n");
extern "C" {
void* trt_CreateFileW(const u16*, u32, u32, void*, u32, u32, void*);
i32 trt_CloseHandle(void*);
}
SHLW_TRAMP(w_lCreateFileW, trt_CreateFileW)
SHLW_TRAMP(w_lCloseHandle, trt_CloseHandle)

namespace {

struct extent { void* base; size_t len; };
extent g_extents[16];
int g_extent_count = 0;

void* linux_alloc(size_t bytes, u64 preferred_base) {
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
  const size_t len = ((uintptr_t)base + bytes - b + page - 1) & ~(uintptr_t)(page - 1);
  return mprotect((void*)b, len, p);
}
void linux_release(void* base, size_t) {
  for (int i = 0; i < g_extent_count; ++i)
    if (g_extents[i].base == base) {
      munmap(base, g_extents[i].len);
      g_extents[i].base = nullptr;
      return;
    }
}
void linux_log(const char* line) { printf("  [trt] %s\n", line); }

u8* read_file(const char* path, size_t* size) {
  FILE* f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  u8* buf = (u8*)malloc((size_t)n);
  const size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  *size = got;
  return buf;
}

u64 win64_call(void* fn, u64 a, u64 b, u64 c) {
  u64 args[3] = {a, b, c};
  u64 ret;
  asm volatile(
    "movq %[a0], %%rcx\n"
    "movq %[a1], %%rdx\n"
    "movq %[a2], %%r8\n"
    "subq $48, %%rsp\n"
    "call *%[fn]\n"
    "addq $48, %%rsp\n"
    : "=a"(ret)
    : [a0] "m"(args[0]), [a1] "m"(args[1]), [a2] "m"(args[2]),
      [fn] "r"(fn)
    : "rcx", "rdx", "r8", "r9", "r10", "r11",
      "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "memory", "cc");
  return ret;
}

int failures = 0;
#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (cond) printf("  PASS %s\n", msg);                            \
    else { printf("  FAIL %s\n", msg); ++failures; }                 \
  } while (0)

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: test_shlwapi <guest.exe>\n");
    return 2;
  }
  size_t size = 0;
  u8* file = read_file(argv[1], &size);
  if (!file) { printf("cannot read %s\n", argv[1]); return 2; }

  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &linux_log,
                 nullptr};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();
  trt_bridge_register_shlwapi();

  char drive[64];
  snprintf(drive, sizeof(drive), "/tmp/opencode/shlwapi-test-%d", getpid());
  mkdir(drive, 0755);  // the mount root must exist for CreateFileW
  trt_vfs_mount("c:", drive);
  {
    static const trt_named_fn k_k32_extra[] = {
        {"CreateFileW", (void*)&w_lCreateFileW},
        {"CloseHandle", (void*)&w_lCloseHandle},
    };
    trt_module_register("kernel32.dll", TRT_ROUTE_BUILTIN, k_k32_extra,
                        (int)(sizeof(k_k32_extra) / sizeof(k_k32_extra[0])));
  }

  printf("tiramisu shlwapi bridge test\n");

  trt_resolver rs;
  trt_resolver_default(&rs);
  trt_image* img = nullptr;
  const int rc = trt_map_pe(&be, &rs, file, size, &img);
  CHECK(rc == TRT_OK, "guest PE maps (shlwapi bound)");
  if (rc != TRT_OK || !img) {
    printf("  map failed: %s\n", trt_errstr(rc));
    free(file);
    return 1;
  }
  CHECK(trt_seal_image(&be, img) == TRT_OK, "image sealed (W^X)");

  const u64 ret = win64_call(img->base + img->entry_rva, 0, 0, 0);
  printf("  guest returned 0x%llx (want 0x3ff)\n", (unsigned long long)ret);

  CHECK(ret & 0x001u, "PathCombineW -> C:\\dir\\file.txt");
  CHECK(ret & 0x002u, "PathFindFileNameW -> engine.dll");
  CHECK(ret & 0x004u, "PathAppendW -> C:\\sv\\config");
  CHECK(ret & 0x008u, "PathStripPathW -> data.bin");
  CHECK(ret & 0x010u, "PathFindExtensionW -> .cfg");
  CHECK(ret & 0x020u, "PathRemoveExtensionW -> settings");
  CHECK(ret & 0x040u, "PathIsRelativeW: absolute no, relative yes");
  CHECK(ret & 0x080u, "PathFileExistsW TRUE for a file the guest created");
  CHECK(ret & 0x100u, "PathFileExistsW FALSE for a missing file");
  CHECK(ret & 0x200u, "PathIsDirectoryW: file is not a directory");

  free(file);
  if (failures) {
    printf("RESULT: %d FAIL\n", failures);
    return 1;
  }
  printf("RESULT: ALL PASS\n");
  return 0;
}
