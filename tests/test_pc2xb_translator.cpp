// tiramisu/tests/test_pc2xb_translator.cpp
// Phase 3: PC->Xbox API translator. Translates the built guest_hello.exe's
// import surface and verifies the translated profile + the JSON/CSV dumps.
#include "../src/trt/pc2xb_translator.h"
#include "../src/trt/trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CHECK(cond, msg) \
  do { \
    if (!(cond)) { \
      fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
      exit(1); \
    } else { \
      printf("  PASS: %s\n", msg); \
    } \
  } while (0)

int main(int argc, char** argv) {
  CHECK(argc >= 2, "usage: test_pc2xb_translator <guest_exe>");
  const char* guest = argv[1];
  static trt_universal_profile pc, xb;

  const int n = trt_pc2xb_translate_file(guest, "/tmp/trt_pc2xb_test");
  CHECK(n >= 1, "translate_file ran");
  CHECK(trt_pe_enumerate_imports(guest, &pc, 1024) >= 1, "pc surface dumped");
  CHECK(trt_pc2xb_translate_imports(&pc, &xb) > 0, "translate_imports");
  CHECK(xb.import_count == (uint32_t)n, "translate count matches");
  bool modules_ok = true, symbols_ok = true;
  for (uint32_t i = 0; i < xb.import_count; ++i) {
    if (!xb.imports[i].module[0]) modules_ok = false;
    if (!xb.imports[i].symbol[0]) symbols_ok = false;
  }
  CHECK(modules_ok, "xb module names non-empty");
  CHECK(symbols_ok, "xb symbol names non-empty");
  // guest_hello imports TiramisuHostQueryBuild (tiramisu_host.dll) ->
  // host-serviced on Xbox.
  bool found_host = false;
  for (uint32_t i = 0; i < xb.import_count; ++i)
    if (strcmp(xb.imports[i].module, "host:tiramisu_host.dll") == 0)
      found_host = true;
  CHECK(found_host, "tiramisu_host.dll mapped to host: serviced module");
  struct stat st;
  CHECK(stat("/tmp/trt_pc2xb_test_imports.json", &st) == 0,
        "xb_imports.json written");
  CHECK(stat("/tmp/trt_pc2xb_test_imports.csv", &st) == 0,
        "xb_imports.csv written");
  CHECK(stat("/tmp/trt_pc2xb_test_profile.json", &st) == 0,
        "xb_profile.json written");
  printf("  info: pc_imports=%d xb_imports=%d\n", pc.import_count,
         xb.import_count);
  return 0;
}
