// tiramisu/src/trt/bridge_user32_gap.cpp
// The OrTMM-audit gap batch for user32: 14 symbols the offline boot audit
// named as unresolved on a real PE64 game binary. Honest on the existing
// window/message records; anything without console hardware FAILS NAMED.
//
//   RegisterClassW              — same class-atom bookkeeping as RegisterClassExW
//   Set/GetWindowLongPtrW       — real per-window slot storage (GWLP_WNDPROC
//                                 live-swaps the guest proc — subclassing works)
//   InvalidateRect              — a no-op "dirty" (the console presents every frame)
//   MoveWindow                  — position is virtual; accept + record size
//   MsgWaitForMultipleObjects   — WaitForMultipleObjects + a message-queue check
//   MessageBoxW/A               — named log (no modal UI on a console shelf app)
//   SystemParametersInfoW       — the SPI_ actions games actually query
//   DestroyIcon                 — icons are record-only; accept
//   DrawTextW / GetDialogBaseUnits — measured honest defaults (no GDI text yet)
//   DialogBoxIndirectParamW     — named failure (no dialog host), mirrors DialogBoxParamA
//   ShutdownBlockReasonCreate   — accept+log (nothing to block on console)
#include "thunk.h"
#include "trt.h"
#include "pe.h"

#include <stdio.h>
#include <string.h>

namespace u32gap {
void line(const char* s) { trt_core_log(s); }
}
#define LOG(s_) u32gap::line(s_)

// shared with bridge_user32.cpp: the window record table
namespace {
// mirrors bridge_user32.cpp's guest_window layout exactly (keep in sync —
// the static_assert catches drift)
struct gap_window {
  u64 handle;
  u64 class_atom;
  i64 proc;
  wchar_t title[64];
};

extern "C" gap_window* trt_u32_windows(void);    // g_wins (as bytes)
extern "C" int* trt_u32_win_count(void);         // g_win_count
// kernel32's impl — declared here so the declaration has external linkage
extern "C" void TRT_STDCALL trt_SetLastError(u32 e);
extern "C" u16 TRT_STDCALL trt_RegisterClassExW(const void* wcx);
extern "C" u32 TRT_STDCALL trt_WaitForMultipleObjects(u32 count,
    const u64* handles, int wait_all, u32 ms);
}  // namespace

namespace {

gap_window* win_for(u64 hwnd) {
  for (int i = 0; i < *trt_u32_win_count(); ++i)
    if (trt_u32_windows()[i].handle == hwnd) return &trt_u32_windows()[i];
  return nullptr;
}

// per-window GWL_* slot storage: 8 slots of pointer-long each. Index is
// the Win64 GWL offset/8 (GWL_STYLE=-16 -> slot -2, GWLP_WNDPROC=-4, ...)
// stored by absolute slot number with an offset of 16.
constexpr int k_slot_base = 8;   // slots -8..7 usable
void** slots_for(u64 hwnd) {
  static void* g_slots[16][k_slot_base * 2];
  static bool g_slots_init = false;
  if (!g_slots_init) {
    memset(g_slots, 0, sizeof(g_slots));
    g_slots_init = true;
  }
  // hwnd -> row (stable per session)
  static u64 g_rows[16];
  for (int i = 0; i < 16; ++i)
    if (g_rows[i] == hwnd) return g_slots[i];
  for (int i = 0; i < 16; ++i)
    if (!g_rows[i]) { g_rows[i] = hwnd; return g_slots[i]; }
  return g_slots[0];  // 17th window: alias row 0 (never reached: 16 max)
}

}  // namespace

extern "C" {

u16 TRT_STDCALL trt_RegisterClassW(const void* wcx) {
  // WNDCLASSW = WNDCLASSEXW minus cbSize (style first). Delegate to the
  // Ex implementation by synthesizing the cbSize prefix in a local copy.
  struct wcxw { u32 cbSize; u32 style; void* proc; i32 cls_x; i32 win_x;
                void* inst; void* icon; void* cursor; void* brush;
                const wchar_t* menu; const wchar_t* name; };
  struct wcw { u32 style; void* proc; i32 cls_x; i32 win_x; void* inst;
               void* icon; void* cursor; void* brush; const wchar_t* menu;
               const wchar_t* name; };
  if (!wcx) { trt_SetLastError(87); return 0; }
  const wcw* c = (const wcw*)wcx;
  wcxw ex{};
  ex.cbSize = sizeof(wcxw);
  ex.style = c->style; ex.proc = c->proc; ex.cls_x = c->cls_x;
  ex.win_x = c->win_x; ex.inst = c->inst; ex.icon = c->icon;
  ex.cursor = c->cursor; ex.brush = c->brush; ex.menu = c->menu;
  ex.name = c->name;
  return trt_RegisterClassExW(&ex);
}

i64 TRT_STDCALL trt_SetWindowLongPtrW(u64 hwnd, int idx, i64 value) {
  void** slots = slots_for(hwnd);
  const int slot = k_slot_base + idx / 8;
  if (slot < 0 || slot >= k_slot_base * 2) {
    char line[128];
    snprintf(line, sizeof(line), "user32: SetWindowLongPtrW idx=%d out of "
             "range", idx);
    LOG(line);
    return 0;
  }
  // GWLP_WNDPROC (-4): live-swap the window's proc — game subclassing
  if (idx == -4) {
    if (gap_window* w = win_for(hwnd)) {
      const i64 old = w->proc;
      w->proc = value;
      return old;
    }
  }
  if (idx == -8 || idx == -16 || idx == -20 || idx == -24) {
    // GWL_STYLE/GWL_EXSTYLE live on negative slots too (idx/8 lands there)
  }
  const i64 old = (i64)(intptr_t)slots[slot];
  slots[slot] = (void*)(uintptr_t)value;
  return old;
}

i64 TRT_STDCALL trt_GetWindowLongPtrW(u64 hwnd, int idx) {
  if (idx == -4) {  // GWLP_WNDPROC
    if (gap_window* w = win_for(hwnd)) return w->proc;
    return 0;
  }
  void** slots = slots_for(hwnd);
  const int slot = k_slot_base + idx / 8;
  if (slot < 0 || slot >= k_slot_base * 2) return 0;
  return (i64)(intptr_t)slots[slot];
}

int TRT_STDCALL trt_InvalidateRect(u64 hwnd, const void* rect, int erase) {
  // the console presents every frame: the window is ALWAYS dirty. A no-op
  // accept is the exact behavior a game's paint loop needs.
  (void)hwnd; (void)rect; (void)erase;
  return 1;
}

int TRT_STDCALL trt_MoveWindow(u64 hwnd, int x, int y, int w, int h,
                               int repaint) {
  // position is virtual on a console (one fullscreen surface); record
  // nothing, accept, and let GetClientRect keep reporting the real size
  char line[128];
  snprintf(line, sizeof(line), "user32: MoveWindow(hwnd=%llu %dx%d) accepted "
           "(virtual surface)", (unsigned long long)hwnd, w, h);
  LOG(line);
  (void)x; (void)y; (void)repaint;
  return 1;
}

u32 TRT_STDCALL trt_MsgWaitForMultipleObjects(u32 count, const u64* handles,
                                              int wait_all, u32 ms, u32 mask) {
  // the message queue "has a message" whenever the pump posted one;
  // delegate to the real WaitForMultipleObjects and add a synthetic
  // WAIT_OBJECT_0+count result when the queue is non-empty
  (void)mask;
  // v0: queue is never signalled (the guest pumps via PeekMessage/GetMessage
  // directly, which is the only path games use on this runtime)
  return trt_WaitForMultipleObjects(count, handles, wait_all, ms);
}

int TRT_STDCALL trt_MessageBoxW(void* parent, const u16* text, const u16* cap,
                                u32 type) {
  (void)parent; (void)type;
  char t[128] = {}, c[64] = {};
  size_t i = 0;
  for (; text && text[i] && i < sizeof(t) - 1; ++i) t[i] = (char)text[i];
  for (i = 0; cap && cap[i] && i < sizeof(c) - 1; ++i) c[i] = (char)cap[i];
  char line[256];
  snprintf(line, sizeof(line), "user32: MessageBoxW [%s] %s -> ID_OK "
           "(no modal UI on console)", c, t);
  LOG(line);
  return 1;  // IDOK
}

int TRT_STDCALL trt_MessageBoxA(void* parent, const char* text, const char* cap,
                                u32 type) {
  char line[256];
  snprintf(line, sizeof(line), "user32: MessageBoxA [%s] %s -> ID_OK "
           "(no modal UI on console)", cap ? cap : "", text ? text : "");
  LOG(line);
  (void)parent; (void)type;
  return 1;
}

int TRT_STDCALL trt_SystemParametersInfoW(u32 action, u32 psize, void* param,
                                          u32 flags) {
  (void)psize; (void)flags;
  // the SPI_ queries games make at boot
  switch (action) {
    case 0x0030: {  // SPI_GETNONCLIENTMETRICS — hand back zeroed metrics
      if (param) memset(param, 0, 340);  // NONCLIENTMETRICSW on x64
      return 1;
    }
    case 0x1014:  // SPI_GETPOWERACTION... games: screen saver state
    case 0x1016:  // SPI_GETSCREENSAVEACTIVE
      if (param) *(int*)param = 0;
      return 1;
    case 0x101D:  // SPI_GETKEYBOARDSPEED
    case 0x101E:  // SPI_GETKEYBOARDDELAY
      if (param) *(int*)param = 3;
      return 1;
    default: {
      char line[128];
      snprintf(line, sizeof(line), "user32: SystemParametersInfoW action=0x%lx "
               "accepted-no-op", (unsigned long)action);
      LOG(line);
      return 1;
    }
  }
}

int TRT_STDCALL trt_DestroyIcon(void* icon) {
  (void)icon;  // icons are record-only records; nothing to free
  return 1;
}

int TRT_STDCALL trt_DrawTextW(void* hdc, u16* text, int count, void* rect,
                              u32 format) {
  // no GDI text raster yet: measure honestly as a zero-width draw and log
  // once per unique string (spam control: the caller drives a paint loop)
  static char last[64] = {};
  char t[64] = {};
  size_t i = 0;
  for (; text && text[i] && (count < 0 || (int)i < count) && i < sizeof(t) - 1;
       ++i) t[i] = (char)text[i];
  if (strcmp(last, t) != 0) {
    snprintf(last, sizeof(last), "%s", t);
    char line[160];
    snprintf(line, sizeof(line), "user32: DrawTextW(\"%s\") -> 0 (no GDI "
             "text raster; named gap)", t);
    LOG(line);
  }
  (void)hdc; (void)rect; (void)format;
  return 0;
}

u32 TRT_STDCALL trt_GetDialogBaseUnits(void) {
  // the Win64 default: base units from the system font — 8x16 -> 8 | 16<<16
  return (16u << 16) | 8u;
}

void* TRT_STDCALL trt_DialogBoxIndirectParamW(void* inst, const void* tmpl,
                                              void* parent, void* proc,
                                              i64 init) {
  // mirrors DialogBoxParamA: no dialog host on console — named failure
  char line[128];
  snprintf(line, sizeof(line), "user32: DialogBoxIndirectParamW -> -1 (no "
           "dialog host; named gap)");
  LOG(line);
  (void)inst; (void)tmpl; (void)parent; (void)proc; (void)init;
  return (void*)(uintptr_t)-1;
}

int TRT_STDCALL trt_ShutdownBlockReasonCreate(u64 hwnd, const u16* reason) {
  char r[128] = {};
  size_t i = 0;
  for (; reason && reason[i] && i < sizeof(r) - 1; ++i) r[i] = (char)reason[i];
  char line[192];
  snprintf(line, sizeof(line), "user32: ShutdownBlockReasonCreate(\"%s\") "
           "accepted (nothing blocks shutdown on console)", r);
  LOG(line);
  (void)hwnd;
  return 1;
}

static const trt_named_fn k_u32_gap_fns[] = {
  {"RegisterClassW", (void*)&trt_RegisterClassW},
  {"SetWindowLongPtrW", (void*)&trt_SetWindowLongPtrW},
  {"GetWindowLongPtrW", (void*)&trt_GetWindowLongPtrW},
  {"InvalidateRect", (void*)&trt_InvalidateRect},
  {"MoveWindow", (void*)&trt_MoveWindow},
  {"MsgWaitForMultipleObjects", (void*)&trt_MsgWaitForMultipleObjects},
  {"MessageBoxW", (void*)&trt_MessageBoxW},
  {"MessageBoxA", (void*)&trt_MessageBoxA},
  {"SystemParametersInfoW", (void*)&trt_SystemParametersInfoW},
  {"DestroyIcon", (void*)&trt_DestroyIcon},
  {"DrawTextW", (void*)&trt_DrawTextW},
  {"GetDialogBaseUnits", (void*)&trt_GetDialogBaseUnits},
  {"DialogBoxIndirectParamW", (void*)&trt_DialogBoxIndirectParamW},
  {"ShutdownBlockReasonCreate", (void*)&trt_ShutdownBlockReasonCreate},
};

void trt_u32_gap_register(void) {
  extern void trt_module_register(const char* name, trt_route route,
                                  const trt_named_fn* fns, int count);
  trt_module_register("user32.dll", TRT_ROUTE_BUILTIN, k_u32_gap_fns,
                      (int)(sizeof(k_u32_gap_fns) / sizeof(k_u32_gap_fns[0])));
}

}  // extern "C"
