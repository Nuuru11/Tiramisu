// tiramisu/tests/test_gdi.cpp
// The gdi32 bridge: a freestanding guest creates a font, selects it into
// a user32 DC, deletes it; the host verifies the record lifecycle through
// the shared log and the guest's return bits.
#include "../src/trt/trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_user32(void);
int trt_bridge_register_gdi(void);
}

namespace {

char g_log[256][256];
int g_log_count = 0;
bool log_contains(const char* needle) {
  for (int i = 0; i < g_log_count; ++i)
    if (strstr(g_log[i], needle)) return true;
  return false;
}
int log_count_of(const char* needle) {
  int n = 0;
  for (int i = 0; i < g_log_count; ++i)
    if (strstr(g_log[i], needle)) ++n;
  return n;
}

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
void linux_log(const char* line) {
  printf("  [trt] %s\n", line);
  if (g_log_count < 256)
    snprintf(g_log[g_log_count++], sizeof(g_log[0]), "%s", line);
}

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
    printf("usage: test_gdi <guest.exe>\n");
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
  trt_bridge_register_gdi();

  printf("tiramisu gdi32 bridge test\n");

  CHECK(trt_module_route("gdi32.dll") == TRT_ROUTE_BUILTIN,
        "gdi32.dll routes BUILTIN (no fake SYSTEM bind)");
  CHECK(trt_module_route("user32.dll") == TRT_ROUTE_BUILTIN,
        "user32.dll routes BUILTIN (shared DC space)");

  trt_resolver rs;
  trt_resolver_default(&rs);
  trt_image* img = nullptr;
  const int rc = trt_map_pe(&be, &rs, file, size, &img);
  CHECK(rc == TRT_OK, "guest PE maps (gdi32 + user32 GetDC bound)");
  if (rc != TRT_OK || !img) {
    printf("  map failed: %s (%s)\n", trt_errstr(rc),
           img ? img->last_detail : "no image");
    free(file);
    return 1;
  }
  CHECK(trt_seal_image(&be, img) == TRT_OK, "image sealed (W^X)");

  const u64 ret = win64_call(img->base + img->entry_rva, 0, 0, 0);
  printf("  guest returned 0x%llx (want 0xff)\n", (unsigned long long)ret);

  CHECK((ret & 0x01u) != 0, "GetDC handed out a real DC record");
  CHECK((ret & 0x02u) != 0, "CreateFontIndirectW returned a handle");
  CHECK((ret & 0x04u) != 0, "SelectObject recorded per-DC state");
  CHECK((ret & 0x08u) != 0, "DeleteObject freed the record (select-after fails)");
  CHECK((ret & 0x10u) != 0, "GetStockObject is stable");
  CHECK((ret & 0x20u) != 0, "stock object selected; ReleaseDC released");
  CHECK((ret & 0x40u) != 0, "garbage DC -> SelectObject NULL (fail-visible)");
  CHECK((ret & 0x80u) != 0, "SetBkMode/SetTextColor/SetBkColor recorded");

  // host-side lifecycle evidence in the log
  CHECK(log_contains("gdi32: CreateFontIndirectW h=") &&
            log_contains("lfHeight=-24"),
        "log names the created font with its lfHeight");
  CHECK(log_count_of("gdi32: DeleteObject") == 1,
        "exactly one DeleteObject (the freed font)");
  CHECK(log_contains("not a user32 DC -> NULL"),
        "the garbage-DC select was named");
  CHECK(log_contains("user32: GetDC") == false,  // GetDC itself is silent-ok
        "no misleading GetDC failure noise");

  trt_unmap(&be, img);
  free(file);
  if (failures) {
    printf("RESULT: %d FAIL\n", failures);
    return 1;
  }
  printf("RESULT: ALL PASS\n");
  return 0;
}
