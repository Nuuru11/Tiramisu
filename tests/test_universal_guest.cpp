// tiramisu/tests/test_universal_guest.cpp
// Phase 4: universal guest runner — maps the universal guest PE through the
// standard loader (the console path) and checks the entry point is collected.
#include "../src/trt/universal_guest.h"
#include "../src/trt/trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(cond, msg) \
  do { \
    if (!(cond)) { \
      fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
      exit(1); \
    } else { \
      printf("  PASS: %s\n", msg); \
    } \
  } while (0)

static void dummy_query(void) {}
static void dummy_log_u32(unsigned tag, unsigned value) {
  (void)tag; (void)value;
}
static void* test_resolve(void* user, const char* module, const char* symbol,
                          u16 ordinal) {
  (void)user; (void)ordinal;
  if (strcmp(module, "tiramisu_host.dll") == 0) {
    if (strcmp(symbol, "TiramisuHostQueryBuild") == 0)
      return (void*)&dummy_query;
    if (strcmp(symbol, "TiramisuHostLogU32") == 0)
      return (void*)&dummy_log_u32;
  }
  return nullptr;
}
void* linux_alloc(size_t bytes, u64 preferred_base) {
  (void)preferred_base;
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return p;
}
int linux_protect(void* base, size_t bytes, unsigned prot) {
  int p = 0;
  if (prot & TRT_PROT_R) p |= PROT_READ;
  if (prot & TRT_PROT_W) p |= PROT_WRITE;
  if (prot & TRT_PROT_X) p |= PROT_EXEC;
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  const uintptr_t b = (uintptr_t)base & ~(uintptr_t)(page - 1);
  const size_t len =
      ((uintptr_t)base + bytes - b + page - 1) & ~(uintptr_t)(page - 1);
  return mprotect((void*)b, len, p);
}
void linux_release(void* base, size_t bytes) { munmap(base, bytes); }
void linux_log(const char* line) { printf("  [trt] %s\n", line); }

int main(int argc, char** argv) {
  CHECK(argc >= 2, "usage: test_universal_guest <guest_exe>");
  trt_backend be = {&linux_alloc, &linux_protect, &linux_release, &linux_log,
                    nullptr};
  trt_resolver rs = {nullptr, &test_resolve};
  trt_image* img = trt_universal_guest_load(argv[1], &be, &rs);
  CHECK(img != nullptr, "universal guest maps");
  if (img) {
    CHECK(trt_universal_guest_entry(img) != 0, "entry point collected");
    trt_universal_guest_unload(&be, img);
  }
  return 0;
}
