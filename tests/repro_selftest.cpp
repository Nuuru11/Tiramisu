// repro of the console selftest flow: register tiramisu_host.dll like
// main.cpp boot_all does, then map guest_hello.exe through the RUNTIME
// resolver exactly like run_game does. Pass = the console bug is elsewhere;
// Fail = local repro.
#include "../src/trt/trt.h"
#include "../src/trt/images.h"
#include <sys/mman.h>
#include <cstdio>
#include <fstream>
#include <vector>
#include <string>

static void TRT_GUEST_ABI host_builtin_log_u32(unsigned tag, unsigned value) {
  printf("guest marker: tag=%u value=%u\n", tag, value);
}
static unsigned TRT_GUEST_ABI host_builtin_query_build(void) {
  return 0x00020000u;
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  const char* dir = argc > 1 ? argv[1] : ".";
  static const trt_named_fn k_fns[] = {
      {"TiramisuHostLogU32", (void*)&host_builtin_log_u32},
      {"TiramisuHostQueryBuild", (void*)&host_builtin_query_build},
  };
  trt_module_register("tiramisu_host.dll", TRT_ROUTE_BUILTIN, k_fns, 2);
  printf("query says: %d\n",
         trt_module_query("tiramisu_host.dll", "TiramisuHostLogU32"));

  std::string exe = std::string(dir) + "/guest_hello.exe";
  std::ifstream f(exe, std::ios::binary);
  if (!f) { printf("cannot open %s\n", exe.c_str()); return 2; }
  std::vector<u8> buf((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
  // backend with REAL memory ops (the empty backend {} was my repro bug:
  // map_pe indirect-calls be->alloc — a null there = the RIP=0 crash)
  static auto* k_alloc = +[](size_t bytes, u64 preferred) -> void* {
    (void)preferred;
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? nullptr : p;
  };
  static auto* k_protect = +[](void* base, size_t bytes, unsigned prot) -> int {
    int p = 0;
    if (prot & TRT_PROT_R) p |= PROT_READ;
    if (prot & TRT_PROT_W) p |= PROT_WRITE;
    if (prot & TRT_PROT_X) p |= PROT_EXEC;
    const uintptr_t addr = (uintptr_t)base;
    const uintptr_t page = addr & ~(uintptr_t)4095ULL;
    const size_t span = (size_t)((addr + bytes) - page);
    const size_t rounded = (span + 4095ULL) & ~(size_t)4095ULL;
    return mprotect((void*)page, rounded, p) == 0 ? 0 : -1;
  };
  static auto* k_free = +[](void* base, size_t bytes) { if (base) munmap(base, bytes); };
  static auto* k_log = +[](const char* s) { fprintf(stderr, "  [trt] %s\n", s); };
  const trt_backend be{k_alloc, k_protect, k_free, k_log};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_runtime_init(&be, dir);
  trt_resolver* rs = trt_runtime_resolver();
  trt_image* img = nullptr;
  auto rc = trt_map_pe(&be, rs, buf.data(), buf.size(), &img);
  printf("map_rc=%s\n", trt_errstr(rc));
  if (rc == TRT_OK) {
    printf("SELFTEST FLOW WOULD PROCEED (base=%p)\n", (void*)img->base);
  }
  return rc == TRT_OK ? 0 : 1;
}
