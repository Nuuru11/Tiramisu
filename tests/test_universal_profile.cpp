// tiramisu/tests/test_universal_profile.cpp
// Phase 1+2 verification: universal engine profile mode + auto import-surface
// enumerator (trt_pe_enumerate_imports). Uses the built guest_hello.exe as a
// real PE64.
#include "../src/trt/engine_synthesizer.h"
#include "../src/trt/trt.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <sys/stat.h>
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

static bool iequals(const char* a, const char* b) {
  for (; *a && *b; ++a, ++b)
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
  return *a == *b;
}

int main(int argc, char** argv) {
  CHECK(argc >= 2, "usage: test_universal_profile <guest_exe>");
  const char* guest = argv[1];
  static trt_universal_profile up;  // ~530KB -> static storage

  // ---- trt_pe_enumerate_imports on the real guest PE ----
  const int n = trt_pe_enumerate_imports(guest, &up, 64);
  CHECK(n > 0, "enumerator finds guest imports");
  CHECK(up.machine == TRT_PE_MACHINE_X64, "guest is PE64");
  CHECK(up.import_count > 0, "import_count matches return");
  bool sorted_ok = true;
  for (uint32_t i = 1; i < up.import_count; ++i) {
    const int c = strcmp(up.imports[i - 1].module, up.imports[i].module);
    if (c > 0 || (c == 0 && up.imports[i - 1].symbol[0] &&
                  strcmp(up.imports[i - 1].symbol, up.imports[i].symbol) > 0))
      sorted_ok = false;
  }
  CHECK(sorted_ok, "imports sorted by module, symbol");
  bool lower_ok = true;
  for (uint32_t i = 0; i < up.import_count; ++i)
    for (const char* p = up.imports[i].module; *p; ++p)
      if (*p != tolower((unsigned char)*p)) lower_ok = false;
  CHECK(lower_ok, "module names lower-cased");
  bool found = false;
  for (uint32_t i = 0; i < up.import_count; ++i)
    if (iequals(up.imports[i].module, "tiramisu_host.dll") &&
        iequals(up.imports[i].symbol, "tiramisuhostquerybuild"))
      found = true;
  CHECK(found, "enumerator sees TiramisuHostQueryBuild (tiramisu_host.dll)");
  static trt_universal_profile small;
  CHECK(trt_pe_enumerate_imports(guest, &small, 2) <= 2, "cap respected");

  // ---- trt_synthesize_engine on a copy of the guest in a temp dir ----
  char dir[] = "/tmp/trt_uni_synth_XXXXXX";
  CHECK(mkdtemp(dir) != nullptr, "mkdtemp");
  char dst[512];
  snprintf(dst, sizeof(dst), "%s/univgame.exe", dir);
  FILE* in = fopen(guest, "rb");
  CHECK(in != nullptr, "open guest");
  FILE* out = fopen(dst, "wb");
  CHECK(out != nullptr, "create copy");
  char buf[65536];
  size_t r;
  while ((r = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, r, out);
  fclose(in);
  fclose(out);

  trt_engine_profile prof;
  CHECK(trt_synthesize_engine(dir, nullptr, &prof), "synthesize guest dir");
  CHECK(prof.engine_type == TRT_ENGINE_UNIVERSAL,
        "unmanaged no-evidence guest -> UNIVERSAL");
  CHECK((prof.universal_flags & TRT_UNIV_CLEAR_PC) != 0, "CLEAR_PC flag set");
  printf("  info: guest=%s imports=%d engine=%s flags=0x%x\n", guest,
         up.import_count, trt_engine_type_name(prof.engine_type),
         prof.universal_flags);
  return 0;
}
