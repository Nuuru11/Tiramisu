// tiramisu/src/trt/bridge_comctl.cpp
// COMCTL32 bridge. See bridge_comctl.h.
//
// Fail-visible rule: InitCommonControlsEx accepts ONLY known ICC_* class
// bits and names any unknown bits in the log while still returning TRUE
// (Windows accepts the reserved bits too; the named log is our work list).
// OrTMM's import is ordinal-only (#380), which resolves to the same
// function through the "#380" registration.
#include "bridge_comctl.h"
#include "thunk.h"
#include "trt.h"

#include <stdio.h>
#include <string.h>

// unique name, file scope, OUTSIDE extern "C" (the log collision rule)
namespace comctllog {
static void line(const char* s) { trt_core_log(s); }
}
#define log_line(s_) comctllog::line(s_)

namespace {

// ICC_* class bits (wincc.h): the classes a game can ask for
constexpr u32 ICC_LISTVIEW_CLASSES = 0x00000001;
constexpr u32 ICC_TREEVIEW_CLASSES = 0x00000002;
constexpr u32 ICC_BAR_CLASSES = 0x00000004;
constexpr u32 ICC_TAB_CLASSES = 0x00000008;
constexpr u32 ICC_UPDOWN_CLASS = 0x00000010;
constexpr u32 ICC_PROGRESS_CLASS = 0x00000020;
constexpr u32 ICC_HOTCLASS = 0x00000040;
constexpr u32 ICC_DATE_CLASSES = 0x00000100;
constexpr u32 ICC_USEREX_CLASSES = 0x00000200;
constexpr u32 ICC_COOL_CLASSES = 0x00000400;
constexpr u32 ICC_INTERNET_CLASSES = 0x00000800;
constexpr u32 ICC_PAGESCROLLER_CLASS = 0x00001000;
constexpr u32 ICC_NATIVEFNTCTL_CLASS = 0x00002000;
constexpr u32 ICC_STANDARD_CLASSES = 0x00004000;
constexpr u32 ICC_LINK_CLASS = 0x00008000;

// INITCOMMONCONTROLSEX layout (x64): dwSize u32, dwGlobalICC u32
struct iccx {
  u32 dwSize;
  u32 dwICC;
};

void log_flags(u32 dwICC) {
  char line[320];
  line[0] = 0;
  struct bit { u32 v; const char* n; };
  const bit bits[] = {
      {ICC_LISTVIEW_CLASSES, "Listview"},   {ICC_TREEVIEW_CLASSES, "Treeview"},
      {ICC_BAR_CLASSES, "Bar"},             {ICC_TAB_CLASSES, "Tab"},
      {ICC_UPDOWN_CLASS, "UpDown"},         {ICC_PROGRESS_CLASS, "Progress"},
      {ICC_HOTCLASS, "Hot"},                {ICC_DATE_CLASSES, "Date"},
      {ICC_USEREX_CLASSES, "UserEx"},       {ICC_COOL_CLASSES, "Cool"},
      {ICC_INTERNET_CLASSES, "Internet"},   {ICC_PAGESCROLLER_CLASS, "PageScroller"},
      {ICC_NATIVEFNTCTL_CLASS, "NativeFont"}, {ICC_STANDARD_CLASSES, "Standard"},
      {ICC_LINK_CLASS, "Link"},
  };
  u32 known = 0;
  bool first = true;
  for (const bit& b : bits) {
    if (dwICC & b.v) {
      known |= b.v;
      char one[48];
      snprintf(one, sizeof(one), "%s%s", first ? "" : "+", b.n);
      strncat(line, one, sizeof(line) - strlen(line) - 1);
      first = false;
    }
  }
  if (dwICC & ~known) {
    char one[48];
    snprintf(one, sizeof(one), "%sunknown(0x%x)", first ? "" : "+",
             dwICC & ~known);
    strncat(line, one, sizeof(line) - strlen(line) - 1);
    first = false;
  }
  char out[384];
  snprintf(out, sizeof(out), "comctl32: InitCommonControlsEx dwICC=0x%x (%s)",
           dwICC, line[0] ? line : "none");
  log_line(out);
}

} // namespace

extern "C" {

void TRT_GUEST_ABI trt_InitCommonControls(void) {
  log_line("comctl32: InitCommonControls (all classes accepted)");
}

int TRT_GUEST_ABI trt_InitCommonControlsEx(const iccx* classes) {
  if (!classes) {
    // Windows returns FALSE on a NULL pointer; the log keeps it honest
    log_line("comctl32: InitCommonControlsEx(NULL) -> FALSE");
    return 0;
  }
  log_flags(classes->dwICC);
  return 1;  // TRUE: accepted (console has no common controls to draw)
}

// ---- Subclass family (SetWindowSubclass/RemoveWindowSubclass/DefSubclassProc)
// Real Win32 keeps a per-window subclass chain and a "current proc" cursor
// during dispatch. The console window layer has no comctl32 message loop
// integration, so the chain is RECORDED (named, per window) and
// DefSubclassProc forwards to the window's base wndproc (DefWindowProcW).
// Subclass hooks are theme/editor tweaks; boot depth does not depend on
// them firing. Every install is logged so a game that truly needs the hook
// shows up as a named work item, not a silent no-op.
// bridge_user32.cpp owns the base window procedure
i64 TRT_GUEST_ABI trt_DefWindowProcW(void* hwnd, u32 msg, u64 wparam,
                                     u64 lparam);

struct sub_class {
  void* hwnd;
  void* proc;
  u64 id;
  u64 refdata;
  int used;
};
static sub_class g_subclasses[128];
static int g_subclass_count = 0;

int TRT_GUEST_ABI trt_SetWindowSubclass(void* hwnd, void* proc, u64 id,
                                        u64 refdata) {
  if (!hwnd || !proc) return 0;
  if (g_subclass_count < (int)(sizeof(g_subclasses) / sizeof(g_subclasses[0]))) {
    sub_class& s = g_subclasses[g_subclass_count++];
    s.hwnd = hwnd;
    s.proc = proc;
    s.id = id;
    s.refdata = refdata;
    s.used = 1;
  }
  char line[128];
  snprintf(line, sizeof(line),
           "comctl32: SetWindowSubclass hwnd=%p proc=%p id=%llu -> recorded "
           "(chain not dispatched; DefSubclassProc -> base wndproc)",
           hwnd, proc, (unsigned long long)id);
  log_line(line);
  return 1;
}

int TRT_GUEST_ABI trt_RemoveWindowSubclass(void* hwnd, void* proc, u64 id) {
  for (int i = 0; i < g_subclass_count; ++i) {
    if (g_subclasses[i].used && g_subclasses[i].hwnd == hwnd &&
        g_subclasses[i].proc == proc && g_subclasses[i].id == id) {
      g_subclasses[i].used = 0;
      return 1;
    }
  }
  return 0;
}

i64 TRT_GUEST_ABI trt_DefSubclassProc(void* hwnd, u32 msg, u64 wparam,
                                      u64 lparam) {
  // Forward to the base window procedure (see the chain note above).
  return trt_DefWindowProcW(hwnd, msg, wparam, lparam);
}

int trt_bridge_register_comctl(void) {
  static const trt_named_fn k_comctl32_fns[] = {
      {"InitCommonControls", (void*)&trt_InitCommonControls},
      {"InitCommonControlsEx", (void*)&trt_InitCommonControlsEx},
      // OrTMM imports COMCTL32 ordinal 380 = InitCommonControlsEx on x64
      // (checked: llvm-objdump shows a bare hint/ord 380 in its COMCTL32
      // import table). The loader passes ordinal-only imports here with
      // symbol == NULL, so the ordinal is registered as "#380".
      {"#380", (void*)&trt_InitCommonControlsEx},
      {"SetWindowSubclass", (void*)&trt_SetWindowSubclass},
      {"RemoveWindowSubclass", (void*)&trt_RemoveWindowSubclass},
      {"DefSubclassProc", (void*)&trt_DefSubclassProc},
  };
  trt_module_register("comctl32.dll", TRT_ROUTE_BUILTIN, k_comctl32_fns,
                      (int)(sizeof(k_comctl32_fns) / sizeof(k_comctl32_fns[0])));
  return 0;
}

} // extern "C"
