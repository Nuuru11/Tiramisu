// tiramisu/tests/audit_boot.cpp
// The offline boot gate for REAL games. Loads a real game PE64 read-only,
// walks its import table (imports + delay imports) and pushes every
// module!symbol through the runtime's REAL resolver — the exact code path
// trt_map_pe uses to bind imports at load. No execution: the game's code
// is never run, its memory is freed, and its bytes are never modified.
//
// Exit 0 = every import resolves -> boot-loadable.
// Exit 1 = gaps (each named module!symbol, ready to become the next bridge).
// Exit 2 = usage/IO error.
//
// This is the tool that converts "should boot" into a measured statement:
// run it against any DRM-free 64-bit game exe on the dev box.
#include "../src/trt/trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>

extern "C" {
// kernel32/user32 register outside trt_bridge_boot (the console host does
// exactly this pairing — src/host/main.cpp:356). The audit never CALLS the
// resolved pointers, it only checks they exist.
int trt_bridge_register_kernel32(void);
int trt_bridge_register_user32(void);
}

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: audit_boot <game.exe>\n");
    return 2;
  }
  FILE* f = fopen(argv[1], "rb");
  if (!f) { printf("cannot read %s\n", argv[1]); return 2; }
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<u8> file(n);
  const size_t got = fread(file.data(), 1, (size_t)n, f);
  fclose(f);
  if ((long)got != n) { printf("short read\n"); return 2; }

  trt_backend be{nullptr, nullptr, nullptr, nullptr, nullptr};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();

  printf("tiramisu boot audit: %s (%.1f MB)\n", argv[1],
         (double)n / (1024.0 * 1024.0));

  // every import through the real resolver (in trt_audit_imports)
  std::vector<std::string> missing;
  const int rc = trt_audit_imports(file.data(), (size_t)n, &missing);
  if (rc != TRT_OK) {
    printf("  audit failed: %s\n", trt_errstr(rc));
    return 2;
  }
  if (missing.empty()) {
    printf("ALL IMPORTS RESOLVE — boot-loadable\n");
    return 0;
  }
  printf("%zu unresolved module!symbol:\n", missing.size());
  for (const std::string& s : missing) printf("  %s\n", s.c_str());
  return 1;
}
