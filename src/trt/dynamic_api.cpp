// tiramisu/src/trt/dynamic_api.cpp
// Universal Dynamic API Synthesizer & Intelligent Blanket Thunking.
#include "dynamic_api.h"
#include "thunk.h"

#include <stdio.h>
#include <string.h>
#include <mutex>
#include <string>
#include <unordered_set>

namespace {

char lower_ascii(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

std::mutex g_synth_mu;
std::unordered_set<std::string> g_synthesized_symbols;

} // namespace

extern "C" {

uint64_t trt_dynamic_deduce_return_val(const char* module, const char* symbol) {
  if (!symbol) return 0;

  // Ordinal fallback
  if (symbol[0] == '#') return 0;

  // Hash symbol name for stable, non-zero pseudo-handles
  uint32_t h = 0x55555555;
  for (const char* p = symbol; *p; ++p) {
    h = (h * 33) ^ (unsigned char)(*p);
  }
  uint64_t pseudo_handle = 0x77000000ULL | (uint64_t)((h & 0x00FFFFFE) + 2);

  // 1. Handle & Object Creators / Openers / Loaders
  if (strncmp(symbol, "Create", 6) == 0 ||
      strncmp(symbol, "Open", 4) == 0 ||
      strncmp(symbol, "FindFirst", 9) == 0 ||
      strncmp(symbol, "LoadLibrary", 11) == 0 ||
      strncmp(symbol, "GetModuleHandle", 15) == 0 ||
      strncmp(symbol, "GetStdHandle", 12) == 0 ||
      strncmp(symbol, "BeginPaint", 10) == 0) {
    return pseudo_handle;
  }

  // 2. Booleans & Success-by-default modifiers
  if (strncmp(symbol, "Is", 2) == 0 ||
      strncmp(symbol, "Set", 3) == 0 ||
      strncmp(symbol, "Close", 5) == 0 ||
      strncmp(symbol, "Delete", 6) == 0 ||
      strncmp(symbol, "Post", 4) == 0 ||
      strncmp(symbol, "Peek", 4) == 0 ||
      strncmp(symbol, "Update", 6) == 0 ||
      strncmp(symbol, "Enable", 6) == 0 ||
      strncmp(symbol, "Initialize", 10) == 0 ||
      strncmp(symbol, "Destroy", 7) == 0 ||
      strncmp(symbol, "Show", 4) == 0 ||
      strncmp(symbol, "Hide", 4) == 0 ||
      strncmp(symbol, "Reset", 5) == 0 ||
      strncmp(symbol, "Play", 4) == 0 ||
      strncmp(symbol, "Stop", 4) == 0 ||
      strncmp(symbol, "Pause", 5) == 0 ||
      strncmp(symbol, "Release", 7) == 0) {
    return 1;
  }

  // 3. COM / HRESULT / Win32 Status
  if (strncmp(symbol, "Co", 2) == 0 ||
      strncmp(symbol, "Reg", 3) == 0 ||
      strncmp(symbol, "D3D", 3) == 0 ||
      strncmp(symbol, "DXGI", 4) == 0 ||
      strncmp(symbol, "XAudio", 6) == 0 ||
      strncmp(symbol, "MF", 2) == 0 ||
      strncmp(symbol, "Direct", 6) == 0 ||
      strncmp(symbol, "Wsa", 3) == 0 ||
      strncmp(symbol, "WSA", 3) == 0) {
    return 0; // S_OK / ERROR_SUCCESS
  }

  // 4. Versioning & Dimensions
  if (strncmp(symbol, "GetVersion", 10) == 0) {
    return 0x0A00; // Windows 10
  }
  if (strstr(symbol, "Count") || strstr(symbol, "Length") || strstr(symbol, "Size")) {
    return 1;
  }

  // 5. Memory queries
  if (strncmp(symbol, "VirtualQuery", 12) == 0) {
    return 48; // sizeof(MEMORY_BASIC_INFORMATION64)
  }

  return 0;
}

int trt_dynamic_deduce_arg_count(const char* module, const char* symbol) {
  if (!symbol) return 0;
  if (symbol[0] == '#') return 4;

  if (strncmp(symbol, "Close", 5) == 0 ||
      strncmp(symbol, "Free", 4) == 0 ||
      strncmp(symbol, "GetModuleHandle", 15) == 0 ||
      strncmp(symbol, "LoadLibraryA", 12) == 0 ||
      strncmp(symbol, "LoadLibraryW", 12) == 0 ||
      strncmp(symbol, "SetEvent", 8) == 0 ||
      strncmp(symbol, "ResetEvent", 10) == 0) {
    return 1;
  }

  if (strncmp(symbol, "GetProcAddress", 14) == 0 ||
      strncmp(symbol, "WaitForSingleObject", 19) == 0 ||
      strncmp(symbol, "SetWindowText", 13) == 0 ||
      strncmp(symbol, "ShowWindow", 10) == 0 ||
      strncmp(symbol, "FindClose", 9) == 0) {
    return 2;
  }

  if (strncmp(symbol, "LoadLibraryEx", 13) == 0) {
    return 3;
  }

  if (strncmp(symbol, "PostMessage", 11) == 0 ||
      strncmp(symbol, "SendMessage", 11) == 0) {
    return 4;
  }

  if (strncmp(symbol, "PeekMessage", 11) == 0 ||
      strncmp(symbol, "ReadFile", 8) == 0 ||
      strncmp(symbol, "WriteFile", 9) == 0) {
    return 5;
  }

  if (strncmp(symbol, "CreateFile", 10) == 0) {
    return 7;
  }

  if (strncmp(symbol, "CreateWindowEx", 14) == 0) {
    return 12;
  }
  if (strncmp(symbol, "CreateWindow", 12) == 0) {
    return 11;
  }

  return 4;
}

int trt_dynamic_is_synthesized(const char* module, const char* symbol) {
  if (!module || !symbol) return 0;
  std::string key = std::string(module) + "!" + symbol;
  std::lock_guard<std::mutex> lk(g_synth_mu);
  return g_synthesized_symbols.count(key) ? 1 : 0;
}

void* trt_dynamic_synthesize_api(const char* module, const char* symbol, uint16_t ordinal) {
  const char* mod_name = module ? module : "unknown.dll";
  char ord_buf[32];
  const char* sym_name = symbol;
  if (!sym_name) {
    snprintf(ord_buf, sizeof(ord_buf), "#%u", (unsigned)ordinal);
    sym_name = ord_buf;
  }

  std::string full_key = std::string(mod_name) + "!" + sym_name;
  uint64_t ret_val = trt_dynamic_deduce_return_val(mod_name, sym_name);

  bool first_time = false;
  {
    std::lock_guard<std::mutex> lk(g_synth_mu);
    if (g_synthesized_symbols.find(full_key) == g_synthesized_symbols.end()) {
      g_synthesized_symbols.insert(full_key);
      first_time = true;
    }
  }

  if (first_time) {
    char log_buf[384];
    snprintf(log_buf, sizeof(log_buf),
             "trt: [AUTONOMOUS SYNTHESIS] synthesized dynamic API %s!%s (inferred ret=0x%llx)",
             mod_name, sym_name, (unsigned long long)ret_val);
    trt_core_log(log_buf);
  }

  return trt_thunk_make_ext(trt_core_backend(), "SYNTH", mod_name, sym_name, ret_val);
}

} // extern "C"
