// tiramisu/src/trt/pc2xb_translator.cpp — Phase 3 PC->Xbox API translator.
#include "pc2xb_translator.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <string>

namespace {

// PC module -> Xbox module (xb == "" keeps the name; any other string is the
// mapped Xbox module). Modules not listed here are host-serviced.
struct xb_map_entry { const char* pc; const char* xb; };
const xb_map_entry k_xb_map[] = {
  // Direct3D legacy -> the Xbox D3D11 universal path (real games add shim
  // layers; the universal host provides them via the D3D11 bridge).
  {"d3d9.dll",     "d3d11.dll"},
  {"d3d8.dll",     "d3d11.dll"},
  {"ddraw.dll",    "d3d11.dll"},
  {"dxgi.dll",     "d3d11.dll"},
  {"d3d12.dll",    "d3d12.dll"},
  {"d3d11.dll",    "d3d11.dll"},
  // Audio: DSound/WinMM are deprecated off Xbox; Tiramisu XAudio2 path.
  {"dsound.dll",   "xaudio2.dll"},
  {"dsound8.dll",  "xaudio2.dll"},
  {"winmm.dll",    "xaudio2.dll"},
  {"xaudio2.dll",  "xaudio2.dll"},
  {"xaudio2_7.dll","xaudio2.dll"},
  // Input: Xbox exposes the same xinput names games link against.
  {"xinput.dll",    "xinput.dll"},
  {"xinput1_4.dll", "xinput.dll"},
  {"xinput1_3.dll", "xinput.dll"},
  // C runtime: Xbox ucrt is the ABI successor for msvcrt-era games.
  {"msvcrt.dll",    "ucrtbase.dll"},
  {"ucrtbase.dll",  nullptr},  // keep
  {nullptr, nullptr}
};

std::string xb_map_module(const char* pc_mod) {
  for (const auto* e = k_xb_map; e->pc; ++e)
    if (strcasecmp(pc_mod, e->pc) == 0)
      return e->xb ? e->xb : pc_mod;
  return std::string("host:") + pc_mod;  // host-serviced by Tiramisu
}

std::string xb_class_of(const std::string& xb_mod) {
  if (xb_mod.rfind("host:", 0) == 0) return "host";
  if (xb_mod.compare(0, 5, "d3d") == 0 || xb_mod.rfind("dxgi", 0) == 0)
    return "gpu";
  if (xb_mod.compare(0, 8, "xaudio2") == 0) return "audio";
  if (xb_mod.rfind("xinput", 0) == 0) return "input";
  return "keep";
}

}  // namespace

int trt_pc2xb_translate_imports(const trt_universal_profile* pc,
                                trt_universal_profile* xb) {
  if (!pc || !xb) return -1;
  memset(xb, 0, sizeof(*xb));
  xb->machine = pc->machine;
  uint32_t n = 0;
  for (uint32_t i = 0; i < pc->import_count && n < 1024; ++i) {
    const trt_universal_import* in = &pc->imports[i];
    if (!in->symbol[0]) continue;  // ordinal-only: nothing to translate
    trt_universal_import* out = &xb->imports[n];
    memset(out, 0, sizeof(*out));
    const std::string xm = xb_map_module(in->module);
    const size_t ml = xm.size() < sizeof(out->module) - 1
                          ? xm.size()
                          : sizeof(out->module) - 1;
    memcpy(out->module, xm.data(), ml);
    out->module[ml] = '\0';
    const size_t sl = strnlen(in->symbol, sizeof(out->symbol) - 1);
    memcpy(out->symbol, in->symbol, sl);
    out->symbol[sl] = '\0';
    out->symbol_offset = in->symbol_offset;
    out->ordinal = in->ordinal;
    out->name_len = in->name_len;
    ++n;
  }
  xb->import_count = n;
  return (int)n;
}

int trt_pc2xb_translate_file(const char* pc_exe, const char* out_prefix) {
  if (!pc_exe || !out_prefix) return -1;
  static trt_universal_profile pc, xb;  // ~1MB each; static storage
  const int n = trt_pe_enumerate_imports(pc_exe, &pc, 1024);
  if (n < 0) return -1;
  if (trt_pc2xb_translate_imports(&pc, &xb) < 0) return -1;

  char path[512];
  // xb_imports.json
  snprintf(path, sizeof(path), "%s_imports.json", out_prefix);
  FILE* f = fopen(path, "w");
  if (!f) return -1;
  fprintf(f, "[\n");
  for (uint32_t i = 0; i < xb.import_count; ++i) {
    const trt_universal_import* imp = &xb.imports[i];
    fprintf(f, "  {\"module\": \"%s\", \"symbol\": \"%s\", \"class\": \"%s\"}%s\n",
            imp->module, imp->symbol, xb_class_of(imp->module).c_str(),
            i + 1 < xb.import_count ? "," : "");
  }
  fprintf(f, "]\n");
  fclose(f);
  // xb_imports.csv
  snprintf(path, sizeof(path), "%s_imports.csv", out_prefix);
  f = fopen(path, "w");
  if (!f) return -1;
  fprintf(f, "module,symbol\n");
  for (uint32_t i = 0; i < xb.import_count; ++i)
    fprintf(f, "%s,%s\n", xb.imports[i].module, xb.imports[i].symbol);
  fclose(f);
  // xb_profile.json
  snprintf(path, sizeof(path), "%s_profile.json", out_prefix);
  f = fopen(path, "w");
  if (!f) return -1;
  fprintf(f, "{\n");
  fprintf(f, "  \"profile_flags\": \"TRT_UNIV_CLEAR_PC\",\n");
  fprintf(f, "  \"pc_imports\": %d,\n", pc.import_count);
  fprintf(f, "  \"xb_imports\": %d,\n", xb.import_count);
  fprintf(f, "  \"arch\": \"universal\"\n}\n");
  fclose(f);
  return (int)xb.import_count;
}
