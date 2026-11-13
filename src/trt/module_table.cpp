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
#include "apiset.h"
#include "thunk.h"

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

struct builtin_mod {
  char name[32];
  trt_route route;
  // the function table is OWNED by the module: several bridges add to the
  // same module (kernel32 gets memory, thread, system and core functions),
  // so registering must MERGE, not replace
  trt_named_fn fns[128];
  int fn_count;
};
// The table must hold every module a game might import: the defaults
// (70+) plus the builtin bridges. Overflow is a bug, not a shrug - it is
// logged, because a dropped registration means an unresolved import.
builtin_mod g_modules[256];
int g_module_count = 0;

// host-installed binding for SYSTEM-route modules (UWP: LoadLibraryW +
// GetProcAddress of the console's real DLLs). NULL => SYSTEM routes fail.
trt_system_load g_system_load = nullptr;

// alias routing: desktop DLL name -> the name that exists on the console.
// This is the #1 hidden boot-killer: a game importing vcruntime140.dll fails
// at LOAD time unless we route it to the console's vcruntime140_app.dll.
struct dll_alias { const char* guest_name; const char* system_name; };
const dll_alias k_dll_aliases[] = {
    {"vcruntime140.dll", "vcruntime140_app.dll"},
    {"msvcp140.dll", "msvcp140_app.dll"},
    {"msvcp140_1.dll", "msvcp140_1_app.dll"},
    {"msvcp140_2.dll", "msvcp140_2_app.dll"},
    {"msvcp140_atomic_wait.dll", "msvcp140_atomic_wait_app.dll"},
    {"msvcp140_codecvt_ids.dll", "msvcp140_codecvt_ids_app.dll"},
    {"concrt140.dll", "concrt140_app.dll"},
    // XInput on console = XINPUTUAP.dll (the SDK's xinput.lib imports it)
    {"xinput1_4.dll", "xinputuap.dll"},
    {"xinput1_3.dll", "xinputuap.dll"},
    {"xinput9_1_0.dll", "xinputuap.dll"},
    {"xinput1_2.dll", "xinputuap.dll"},
    {"xinput1_1.dll", "xinputuap.dll"},
    {"xapauthenticode.dll", "xapauthenticode.dll"},
    // shader compiler: real UWP component, same name
    {"d3dcompiler_47.dll", "d3dcompiler_47.dll"},
    {"d3dcompiler_46.dll", "d3dcompiler_47.dll"},
    {"d3dcompiler_43.dll", "d3dcompiler_47.dll"},
    // d3d9 has NO console component: keep it failing with a NAME (M5)
    {"d3d9.dll", nullptr},
    // steam api: named failure until M6 (no DRM bypass, ever)
    {"steam_api64.dll", nullptr},
    {"steam_api.dll", nullptr},
};

const char* dll_alias_lookup(const char* name);  // defined below (case-insens)

int find_module(const char* name) {
  for (int i = 0; i < g_module_count; ++i)
    if (name_eq(name, g_modules[i].name)) return i;
  return -1;
}

// alias routing: desktop DLL name -> console system DLL name (case-insensitive)
const char* dll_alias_lookup(const char* name) {
  for (const dll_alias& a : k_dll_aliases) {
    size_t i = 0;
    for (; a.guest_name[i] && name[i]; ++i)
      if (lower_ascii(a.guest_name[i]) != lower_ascii(name[i])) break;
    if (a.guest_name[i] == 0 && name[i] == 0) return a.system_name;
  }
  return nullptr;
}

// resolve() adapter the loader consumes
void* resolve_through_table(void* user, const char* module,
                            const char* symbol, u16 ordinal) {
  (void)user;

  // ---- API-set contracts --------------------------------------------
  // "api-ms-win-core-synch-l1-2-0.dll" is not a file on the console; it
  // is a contract name. Redirect it to the DLL that implements it, then
  // resolve normally. The contract name is also tried literally against
  // the console, because some OS images DO carry the schema.
  const char* target = module;
  const char* contract_target = trt_apiset_target(module);
  if (contract_target) {
    target = contract_target;
    char line[224];
    snprintf(line, sizeof(line), "trt: apiset %s -> %s", module, target);
    trt_core_log(line);
  }

  const int mi = find_module(target);
  if (mi < 0) {
    // unknown module: not a hard fail — try the console's real DLL by the
    // same name (dwrite, d2d1, xmcore, secs... exist as system components),
    // then known aliases. Failure stays NAMED in the log (not silent).
    if (g_system_load) {
      if (void* fn = g_system_load(target, symbol)) return fn;
      if (target != module)
        if (void* fn2 = g_system_load(module, symbol)) return fn2;
      if (const char* alias = dll_alias_lookup(target))
        if (void* fn3 = g_system_load(alias, symbol)) return fn3;
      if (const char* alias2 = dll_alias_lookup(module))
        if (void* fn4 = g_system_load(alias2, symbol)) return fn4;
    }
    return nullptr;  // loader names module!symbol in its error
  }
  const builtin_mod& m = g_modules[mi];
  switch (m.route) {
    case TRT_ROUTE_STUB: {
      // fail-visible: the call site names itself when it is reached
      char sym[128];
      if (symbol) snprintf(sym, sizeof(sym), "%s", symbol);
      else snprintf(sym, sizeof(sym), "#%u", (unsigned)ordinal);
      return trt_thunk_make(trt_core_backend(), "STUB", target, sym);
    }
    case TRT_ROUTE_BUILTIN:
      if (symbol) {
        for (int i = 0; i < m.fn_count; ++i)
          if (strcmp(m.fns[i].name, symbol) == 0) return (void*)m.fns[i].fn;
        // Some SDK headers declare their exports inside C++ without
        // extern "C", so the import comes in MSVC-mangled:
        //   __imp_?CreateDXGIFactory2@@YAJAEBU_GUID@@PEAPEAX@Z
        // Fall back to the base name between '?' and the first '@@'.
        if (symbol[0] == '?') {
          const char* at = strstr(symbol, "@@");
          if (at && (size_t)(at - symbol) < 128) {
            char base[128];
            const size_t len = (size_t)(at - symbol - 1);
            memcpy(base, symbol + 1, len);
            base[len] = 0;
            for (int i = 0; i < m.fn_count; ++i)
              if (strcmp(m.fns[i].name, base) == 0) return (void*)m.fns[i].fn;
          }
        }
        // builtin module, unknown symbol: fall through to system by alias
        if (g_system_load) {
          if (void* fn = g_system_load(target, symbol)) return fn;
          if (target != module)
            if (void* fn2 = g_system_load(module, symbol)) return fn2;
          if (const char* alias = dll_alias_lookup(target))
            if (void* fn3 = g_system_load(alias, symbol)) return fn3;
        }
      }
      return nullptr;                       // named failure in loader log
    case TRT_ROUTE_SYSTEM:
      if (g_system_load) {
        if (void* fn = g_system_load(target, symbol)) return fn;
        if (target != module)
          if (void* fn2 = g_system_load(module, symbol)) return fn2;
        if (const char* alias = dll_alias_lookup(target))
          if (void* fn3 = g_system_load(alias, symbol)) return fn3;
      }
      return nullptr;
    default:
      return nullptr;
  }
}

} // namespace

extern "C" {

void trt_module_register(const char* name, trt_route route,
                          const trt_named_fn* fns, int fn_count) {
  if (!name) return;
  // last registration wins: a bridge registering "kernel32.dll" after the
  // defaults must REPLACE the default entry, not shadow it (find_module
  // returns the first match).
  const int existing = find_module(name);
  builtin_mod* m = nullptr;
  if (existing >= 0) {
    m = &g_modules[existing];
    // merging into an existing BUILTIN module: keep what is there and add
    // the new functions; a function that already exists is REPLACED by the
    // newer registration (a test harness or a later bridge can override)
    if (m->route == TRT_ROUTE_BUILTIN && route == TRT_ROUTE_BUILTIN && fns) {
      for (int i = 0; i < fn_count; ++i) {
        bool replaced = false;
        for (int j = 0; j < m->fn_count; ++j) {
          if (strcmp(m->fns[j].name, fns[i].name) == 0) {
            m->fns[j] = fns[i];
            replaced = true;
            break;
          }
        }
        if (!replaced &&
            m->fn_count < (int)(sizeof(m->fns) / sizeof(m->fns[0])))
          m->fns[m->fn_count++] = fns[i];
      }
      return;
    }
  } else {
    if (g_module_count >= (int)(sizeof(g_modules) / sizeof(g_modules[0]))) {
      char line[160];
      snprintf(line, sizeof(line),
               "trt: MODULE TABLE FULL - %s was NOT registered", name);
      trt_core_log(line);
      return;
    }
    m = &g_modules[g_module_count++];
  }
  memset(m, 0, sizeof(*m));
  strncpy(m->name, name, sizeof(m->name) - 1);
  m->route = route;
  for (int i = 0; i < fn_count && i < (int)(sizeof(m->fns) / sizeof(m->fns[0])); ++i)
    m->fns[i] = fns[i];
  m->fn_count = fn_count;
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

// exported so the guest-image resolver (images.cpp) can fall back to the
// module table for system modules
void* trt_resolve_module(void* user, const char* module, const char* symbol,
                         u16 ordinal) {
  return resolve_through_table(user, module, symbol, ordinal);
}

// stub entry point: one shared address; host-side logging wraps real call
// observation later (v0: presence in the IAT is itself logged per module)
void trt_stub_thunk(void) {}

// ---------------------------------------------------------------------------
// Module coverage for real PC games.
//
// A shipped game imports far more than kernel32/user32. Every name it
// imports must resolve to SOMETHING at load time or the game never starts:
//   - SYSTEM: the console ships the real DLL (host binds it by name)
//   - BUILTIN: we implement it (kernel32 core, user32 window layer)
//   - STUB: not implemented here; binds to a fail-visible thunk so the
//           game still boots and the log names the missing piece
// Registration never overwrites a module the bridges already registered.
namespace {

struct mod_default { const char* name; trt_route route; };

const mod_default k_defaults[] = {
    // ---- graphics / presentation (console ships these) ----
    {"d3d11.dll", TRT_ROUTE_SYSTEM},
    {"dxgi.dll", TRT_ROUTE_SYSTEM},
    {"d3dcompiler_47.dll", TRT_ROUTE_SYSTEM},
    {"dcomp.dll", TRT_ROUTE_SYSTEM},
    {"d2d1.dll", TRT_ROUTE_SYSTEM},
    {"dwrite.dll", TRT_ROUTE_SYSTEM},
    {"gdi32.dll", TRT_ROUTE_SYSTEM},
    {"dwmapi.dll", TRT_ROUTE_SYSTEM},
    {"uxtheme.dll", TRT_ROUTE_SYSTEM},
    {"msimg32.dll", TRT_ROUTE_SYSTEM},

    // ---- COM / OLE / WinRT ----
    {"ole32.dll", TRT_ROUTE_SYSTEM},
    {"oleaut32.dll", TRT_ROUTE_SYSTEM},
    {"combase.dll", TRT_ROUTE_SYSTEM},

    // ---- core runtime / CRT ----
    {"ucrtbase.dll", TRT_ROUTE_SYSTEM},
    {"api-ms-win-crt-runtime-l1-1-0.dll", TRT_ROUTE_SYSTEM},
    {"ntdll.dll", TRT_ROUTE_SYSTEM},

    // ---- networking ----
    {"ws2_32.dll", TRT_ROUTE_SYSTEM},
    {"mswsock.dll", TRT_ROUTE_SYSTEM},
    {"winhttp.dll", TRT_ROUTE_SYSTEM},
    {"wininet.dll", TRT_ROUTE_SYSTEM},
    {"dnsapi.dll", TRT_ROUTE_SYSTEM},
    {"iphlpapi.dll", TRT_ROUTE_SYSTEM},
    {"netapi32.dll", TRT_ROUTE_SYSTEM},
    {"mpr.dll", TRT_ROUTE_SYSTEM},
    {"normaliz.dll", TRT_ROUTE_SYSTEM},

    // ---- crypto / security / services ----
    {"bcrypt.dll", TRT_ROUTE_SYSTEM},
    {"ncrypt.dll", TRT_ROUTE_SYSTEM},
    {"crypt32.dll", TRT_ROUTE_SYSTEM},
    {"cryptui.dll", TRT_ROUTE_SYSTEM},
    {"secur32.dll", TRT_ROUTE_SYSTEM},
    {"sechost.dll", TRT_ROUTE_SYSTEM},
    {"sspicli.dll", TRT_ROUTE_SYSTEM},
    {"wintrust.dll", TRT_ROUTE_SYSTEM},
    {"credui.dll", TRT_ROUTE_SYSTEM},

    // ---- shell / env / misc ----
    {"shlwapi.dll", TRT_ROUTE_SYSTEM},
    {"shfolder.dll", TRT_ROUTE_SYSTEM},
    {"shcore.dll", TRT_ROUTE_SYSTEM},
    {"version.dll", TRT_ROUTE_SYSTEM},
    {"propsys.dll", TRT_ROUTE_SYSTEM},
    {"userenv.dll", TRT_ROUTE_SYSTEM},
    {"psapi.dll", TRT_ROUTE_SYSTEM},
    {"hid.dll", TRT_ROUTE_SYSTEM},
    {"setupapi.dll", TRT_ROUTE_SYSTEM},
    {"avrt.dll", TRT_ROUTE_SYSTEM},
    {"imm32.dll", TRT_ROUTE_SYSTEM},
    {"usp10.dll", TRT_ROUTE_SYSTEM},
    {"wtsapi32.dll", TRT_ROUTE_SYSTEM},
    {"dbghelp.dll", TRT_ROUTE_SYSTEM},
    {"dbgcore.dll", TRT_ROUTE_SYSTEM},
    {"dxcore.dll", TRT_ROUTE_SYSTEM},
    {"directxmath.dll", TRT_ROUTE_SYSTEM},

    // ---- input: XInput (console = XINPUTUAP) ----
    {"xinput1_4.dll", TRT_ROUTE_SYSTEM},

    // ---- no console equivalent: fail-visible work items ----
    {"d3d9.dll", TRT_ROUTE_STUB},        // M5: needs a D3D9->D3D11 shim
    {"d3d9core.dll", TRT_ROUTE_STUB},
    {"ddraw.dll", TRT_ROUTE_STUB},
    {"dsound.dll", TRT_ROUTE_STUB},
    {"opengl32.dll", TRT_ROUTE_STUB},    // console has no GL
    {"glu32.dll", TRT_ROUTE_STUB},
    {"dinput8.dll", TRT_ROUTE_STUB},     // console input = XInput
    {"dinput.dll", TRT_ROUTE_STUB},
    {"gameinput.dll", TRT_ROUTE_STUB},   // GDK input
    {"advapi32.dll", TRT_ROUTE_STUB},    // M4: registry (HKLM/HKCU hive)
    {"msacm32.dll", TRT_ROUTE_STUB},
    {"winspool.drv", TRT_ROUTE_STUB},
    {"d3dx9_43.dll", TRT_ROUTE_STUB},    // usually shipped with the game
    {"d3dx10_43.dll", TRT_ROUTE_STUB},
    {"d3dx11_43.dll", TRT_ROUTE_STUB},
    {"d3dx11_47.dll", TRT_ROUTE_STUB},
    {"steam_api64.dll", TRT_ROUTE_STUB}, // M6: never a DRM bypass
    {"steam_api.dll", TRT_ROUTE_STUB},
};

} // namespace

void trt_modules_register_defaults(void) {
  for (size_t i = 0; i < sizeof(k_defaults) / sizeof(k_defaults[0]); ++i) {
    if (find_module(k_defaults[i].name) >= 0) continue;  // bridge wins
    trt_module_register(k_defaults[i].name, k_defaults[i].route, nullptr, 0);
  }
}

} // extern "C"
