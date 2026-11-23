// tiramisu/tools/pc2xb.cpp — PC->Xbox translator CLI (Phase 3).
#include "../src/trt/pc2xb_translator.h"

#include <stdio.h>

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: pc2xb <game.exe> <out_prefix>\n");
    fprintf(stderr, "  dumps the PC import surface, translates it, and writes\n");
    fprintf(stderr, "  <out_prefix>_imports.json, <out_prefix>_imports.csv,\n");
    fprintf(stderr, "  <out_prefix>_profile.json.\n");
    return 1;
  }
  const int n = trt_pc2xb_translate_file(argv[1], argv[2]);
  if (n < 0) {
    fprintf(stderr, "pc2xb: translation failed for %s\n", argv[1]);
    return 1;
  }
  printf("pc2xb: translated %d PC imports -> %s_imports.json/.csv + "
         "%s_profile.json\n", n, argv[2], argv[2]);
  return 0;
}
