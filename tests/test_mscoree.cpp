// tiramisu/tests/test_mscoree.cpp
// Standalone gate for the real mscoree shim: links only module_table.cpp with
// minimal host stubs, so it runs even while other bridge work is in flight.
#include "../src/trt/trt.h"
#include "../src/trt/thunk.h"

#include <cstdio>
#include <cstring>
#include <string>

static bool g_quit = false;
static int g_quit_code = -1;

extern "C" {
void trt_bridge_post_quit(int code) { g_quit = true; g_quit_code = code; }
const char* trt_runtime_search_dir(void) { return ""; }
const trt_backend* trt_core_backend(void) { return nullptr; }
void trt_core_log(const char* line) { (void)line; }
void* trt_thunk_make(const trt_backend*, const char*, const char*,
                     const char*) { return nullptr; }
const trt_backend* trt_runtime_backend(void) { return nullptr; }
trt_resolver* trt_runtime_resolver(void) { return nullptr; }
trt_image* trt_images_find(const char*) { return nullptr; }
trt_image* trt_load_pe(const char*, const char*, int) { return nullptr; }
void* trt_pe_find_export(const trt_backend*, const trt_resolver*,
                         const trt_image*, const char*, u16) { return nullptr; }
}

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { ++fails; printf("  FAIL: %s (line %d)\n", m, __LINE__); } } while (0)

int main() {
  trt_modules_register_defaults();

  int cls = -1;
  CHECK(trt_module_query("mscoree.dll", "CorExitProcess") != 0,
        "mscoree.dll is a resolvable BUILTIN module");
  CHECK(trt_symbol_class("mscoree.dll", "CorExitProcess", &cls) &&
            cls == TRT_IMPL_REAL,
        "CorExitProcess is classified REAL (not a STUB)");
  CHECK(trt_symbol_class("mscoree.dll", "_CorExeMain", &cls) &&
            cls == TRT_IMPL_NAMED_SILENT,
        "managed CLR entry stays named-silent (no CLR on console)");

  typedef void(TRT_STDCALL *exit_fn)(int);
  exit_fn fn = (exit_fn)trt_resolve_module(nullptr, "mscoree.dll",
                                           "CorExitProcess", 0);
  CHECK(fn != nullptr, "CorExitProcess resolves to a real function");
  if (fn) {
    fn(7);
    CHECK(g_quit && g_quit_code == 7,
          "CorExitProcess posts a clean guest quit with the exit code");
  }

  printf("  result: %s\n", fails ? "FAIL" : "ALL PASS");
  return fails ? 1 : 0;
}
