// tiramisu/src/trt/module_table.cpp
// Builtin module registry + routing policy.
//
// Routing classes (from xboxnative's console evidence, re-derived):
//   SYSTEM  — console ships the real DLL (d3d11, dxgi, ws2_32, bcrypt,
//             kernelbase, ole32...): host binds via LoadLibrary+GetProcAddress.
//   BUILTIN — Tiramisu implements it to Wine's observable semantics
//             (kernel32 core, user32 window/message layer on CoreWindow).
//   STUB    — exported, logs when called, returns 0: fail-visible, never
//             fail-silent. Every STUB hit in a log is roadmap work.
#include "trt.h"

#include <stdio.h>
#include <string.h>

namespace {

char lower_ascii(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool name_eq(const char* a, const char* b) {
  size_t i = 0;
  for (; a[i] && b[i]; ++i)
    if (lower_ascii(a[i]) != lower_ascii(b[i])) return false;
  return a[i] == b[i];  // both ended (b may be the shorter registered name)
}

struct builtin_fn { const char* name; void* fn; };
struct builtin_mod {
  char name[32];
  trt_route route;
  const builtin_fn* fns;
  int fn_count;
};
builtin_mod g_modules[64];
int g_module_count = 0;

// host-installed binding for SYSTEM-route modules (UWP: LoadLibraryW +
// GetProcAddress of the console's real DLLs). NULL => SYSTEM routes fail.
trt_system_load g_system_load = nullptr;

int find_module(const char* name) {
  for (int i = 0; i < g_module_count; ++i)
    if (name_eq(name, g_modules[i].name)) return i;
  return -1;
}

// resolve() adapter the loader consumes
void* resolve_through_table(void* user, const char* module,
                            const char* symbol, u16 ordinal) {
  (void)user;
  const int mi = find_module(module);
  if (mi < 0) return nullptr;               // unknown module: hard fail
  const builtin_mod& m = g_modules[mi];
  switch (m.route) {
    case TRT_ROUTE_STUB:
      return (void*)&trt_stub_thunk;       // any symbol -> visible stub
    case TRT_ROUTE_BUILTIN:
      if (symbol) {
        for (int i = 0; i < m.fn_count; ++i)
          if (strcmp(m.fns[i].name, symbol) == 0) return m.fns[i].fn;
      }
      return nullptr;                       // missing builtin: hard fail
    case TRT_ROUTE_SYSTEM:
      if (g_system_load) return g_system_load(module, symbol);
      return nullptr;                       // no host binder: hard fail
    default:
      return nullptr;
  }
}

} // namespace

extern "C" {

void trt_module_register(const char* name, trt_route route,
                          const trt_named_fn* fns, int fn_count) {
  if (g_module_count >= (int)(sizeof(g_modules) / sizeof(g_modules[0])) || !name)
    return;
  builtin_mod& m = g_modules[g_module_count++];
  memset(&m, 0, sizeof(m));
  strncpy(m.name, name, sizeof(m.name) - 1);
  m.route = route;
  m.fns = (const builtin_fn*)fns;
  m.fn_count = fn_count;
}

trt_route trt_module_route(const char* name) {
  const int mi = find_module(name);
  return mi < 0 ? TRT_ROUTE_MISSING : g_modules[mi].route;
}

void trt_set_system_loader(trt_system_load fn) { g_system_load = fn; }

void trt_resolver_default(trt_resolver* out) {
  out->user = nullptr;
  out->resolve = &resolve_through_table;
}

// stub entry point: one shared address; host-side logging wraps real call
// observation later (v0: presence in the IAT is itself logged per module)
void trt_stub_thunk(void) {}

} // extern "C"
