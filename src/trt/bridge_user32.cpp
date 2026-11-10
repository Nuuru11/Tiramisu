// tiramisu/src/trt/bridge_user32.cpp
// Builtin user32 v0.2: input getters (keyboard/mouse real, fed by the host),
// window records, and a WORKING message pump. Semantics follow Wine's
// user32 spec; windows are records, the surface is the host swapchain.
// MSG layout (x64): hwnd(8) message(4+) pad(4) wParam(8) lParam(8) time(4) point(8) = 48
#include "trt.h"

#ifdef _WIN32
#include <windows.h>
#endif
#include "input.h"

#include <string.h>
#include <time.h>

namespace {

void (*g_pump)() = nullptr;

struct guest_window {
  u64 handle;
  u64 class_atom;
  i64 proc;             // guest WNDPROC
  wchar_t title[64];
};
guest_window g_wins[16];
int g_win_count = 0;
u64 g_next_hwnd = 0x00010001;

// Win64 MSG: hwnd u64, message u32, _pad u32, wParam u64, lParam i64,
// time u32, pt {x,y} = 48 bytes
struct guest_msg {
  u64 hwnd;
  u32 message;
  u32 _pad;
  u64 wParam;
  i64 lParam;
  u32 time;
  i32 pt[2];
};
static_assert(sizeof(guest_msg) == 48, "MSG layout drift");

u32 now_ticks() {
#ifdef _WIN32
  return (u32)GetTickCount();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (u32)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

guest_window* find_win(u64 hwnd) {
  for (int i = 0; i < g_win_count; ++i)
    if (g_wins[i].handle == hwnd) return &g_wins[i];
  return nullptr;
}

using wndproc_t = i64 (*)(void* hwnd, u32 msg, u64 wp, i64 lp);

i64 dispatch(u64 hwnd, u32 msg, u64 wp, i64 lp) {
  const guest_window* w = find_win(hwnd);
  if (w && w->proc)
    return ((wndproc_t)(uintptr_t)w->proc)((void*)(uintptr_t)hwnd, msg, wp, lp);
  return 0;  // DefWindowProcW behavior for record windows
}

} // namespace

extern "C" {

// ---- class/window records ----
u16 TRT_STDCALL trt_RegisterClassExW(const void* wcx) {
  if (!wcx) return 0;
  // WNDCLASSEXW: cbSize, style, lpfnWndProc, cbClsExtra, cbWndExtra,
  // hInstance, hIcon, hCursor, hbrBackground, lpszMenuName, lpszClassName, ...
  struct wcxw { u32 cbSize; u32 style; void* proc; i32 cls_x; i32 win_x;
                void* inst; void* icon; void* cursor; void* brush;
                const wchar_t* menu; const wchar_t* name; };
  const wcxw* c = (const wcxw*)wcx;
  if (!c->name) return 0;
  u64 h = 0x84222325;
  for (const wchar_t* p = c->name; *p; ++p) h = (h ^ (*p & 0xFF)) * 0x01000193;
  const u16 atom = (u16)((h & 0x3FFF) | 0xC000);
  // record the class (atom -> proc); re-register updates the proc
  for (int i = 0; i < g_win_count; ++i)
    if (g_wins[i].handle == 0 && g_wins[i].class_atom == atom) {
      g_wins[i].proc = (i64)(uintptr_t)c->proc;
      return atom;
    }
  if (g_win_count >= 16) return atom;
  guest_window& w = g_wins[g_win_count++];
  memset(&w, 0, sizeof(w));
  w.class_atom = atom;
  w.proc = (i64)(uintptr_t)c->proc;
  return atom;
}

void* TRT_STDCALL trt_CreateWindowExW(u32 exstyle, const wchar_t* cls,
                                      const wchar_t* title, u32 style,
                                      int x, int y, int w, int h,
                                      void* parent, void* menu, void* inst,
                                      void* param) {
  (void)exstyle; (void)cls; (void)style; (void)x; (void)y; (void)w; (void)h;
  (void)parent; (void)menu; (void)inst; (void)param;
  // v0.2: one virtual window (the swapchain surface); class bookkeeping
  // stays so RegisterClass/CreateWindow round-trips for the guest
  u64 hwnd;
  if (g_win_count > 0 && g_wins[0].handle != 0) {
    hwnd = g_wins[0].handle;  // games calling CreateWindow twice reuse it
  } else {
    if (g_win_count >= 16) return nullptr;
    guest_window& rec = g_wins[g_win_count++];
    memset(&rec, 0, sizeof(rec));
    rec.handle = g_next_hwnd += 0x100;
    hwnd = rec.handle;
  }
  guest_window* rec = find_win(hwnd);
  if (rec && title) {
    size_t i = 0;
    for (; title[i] && i < 63; ++i) rec->title[i] = title[i];
    rec->title[i] = 0;
  }
  trt_msg_post(0x0001, 0, (i64)hwnd);   // WM_CREATE
  trt_msg_post(0x0018, 1, 0);           // WM_SHOWWINDOW
  return (void*)(uintptr_t)hwnd;
}

int TRT_STDCALL trt_DestroyWindow(void* hwnd) {
  trt_msg_post(0x0002, 0, (i64)(uintptr_t)hwnd);  // WM_DESTROY
  return 1;
}

int TRT_STDCALL trt_ShowWindow(void*, int) { return 1; }
int TRT_STDCALL trt_UpdateWindow(void*) { return 1; }

i64 TRT_STDCALL trt_DefWindowProcW(void* hwnd, u32 msg, u64 wp, i64 lp) {
  (void)wp; (void)lp;
  return msg == 0x0010 ? 1 : 0;  // WM_CLOSE -> TRUE
}

void* TRT_STDCALL trt_GetActiveWindow(void) {
  for (int i = 0; i < g_win_count; ++i)
    if (g_wins[i].handle) return (void*)(uintptr_t)g_wins[i].handle;
  return nullptr;
}

// ---- pump: fill a guest MSG from the ring ----
static bool fill_msg(guest_msg* m, bool remove) {
  trt_msg t;
  if (!trt_msg_peek(&t, remove)) return false;
  if (m) {
    m->hwnd = (u64)(uintptr_t)trt_GetActiveWindow();
    m->message = t.message;
    m->_pad = 0;
    m->wParam = t.wparam;
    m->lParam = t.lparam;
    m->time = now_ticks();
    m->pt[0] = 0;
    m->pt[1] = 0;
  }
  return true;
}

int TRT_STDCALL trt_GetMessageW(guest_msg* out, void* hwnd, u32 min, u32 max) {
  (void)hwnd; (void)min; (void)max;
  for (;;) {
    if (g_pump) g_pump();  // keep CoreWindow/XInput alive inside the guest
    if (fill_msg(out, true)) return (int)out->message != 0x0012;  // WM_QUIT->0
#ifdef _WIN32
    ::Sleep(4);
#else
    struct timespec ts{0, 4 * 1000000};
    nanosleep(&ts, nullptr);
#endif
  }
}

void trt_set_pump(void (*fn)(void)) { g_pump = fn; }

int TRT_STDCALL trt_PeekMessageW(guest_msg* out, void* hwnd, u32 min, u32 max,
                                 u32 remove) {
  (void)hwnd; (void)min; (void)max;
  return fill_msg(out, (remove & 1) != 0) ? 1 : 0;
}

int TRT_STDCALL trt_PostMessageW(void* hwnd, u32 msg, u64 wp, i64 lp) {
  (void)hwnd;
  return trt_msg_post(msg, wp, lp) ? 1 : 0;
}

int TRT_STDCALL trt_TranslateMessage(const void*) { return 1; }

i64 TRT_STDCALL trt_DispatchMessageW(const guest_msg* m) {
  if (!m || !m->hwnd) return 0;
  return dispatch(m->hwnd, m->message, m->wParam, m->lParam);
}

// ---- input getters ----
i16 TRT_STDCALL trt_GetAsyncKeyState(int vk) { return trt_key_async((u16)vk); }
i16 TRT_STDCALL trt_GetKeyState(int vk) { return trt_key_sync((u16)vk); }

int TRT_STDCALL trt_GetKeyboardState(u8* state) {
  if (!state) return 0;
  memset(state, 0, TRT_MAX_VK);
  for (int i = 0; i < TRT_MAX_VK; ++i)
    if (trt_key_down((u16)i)) state[i] = 0x80;
  return 1;
}

int TRT_STDCALL trt_GetCursorPos(void* pt) {
  struct ptw { i32 x, y; };
  int x = 0, y = 0;
  trt_mouse_pos(&x, &y);
  if (pt) { ((ptw*)pt)->x = x; ((ptw*)pt)->y = y; }
  return 1;
}

int TRT_STDCALL trt_SetCursorPos(int x, int y) { (void)x; (void)y; return 1; }
int TRT_STDCALL trt_ClipCursor(const void*) { return 1; }

int TRT_STDCALL trt_GetSystemMetrics(int idx) {
  int w = 0, h = 0;
  trt_display_size(&w, &h);
  switch (idx) {
    case 0: return w;   // SM_CXSCREEN
    case 1: return h;   // SM_CYSCREEN
    case 2: return 1;   // SM_CXVSCROLL
    case 15: return 0;  // SM_SECURE
    default: return 0;
  }
}

void* TRT_STDCALL trt_LoadCursorW(void* inst, const wchar_t* name) {
  (void)inst; (void)name; return (void*)(uintptr_t)0x00020001;
}
void* TRT_STDCALL trt_LoadIconW(void* inst, const wchar_t* name) {
  (void)inst; (void)name; return (void*)(uintptr_t)0x00030001;
}

// ---- Raw Input (the layer most PC engines use for mouse) ----
int TRT_STDCALL trt_RegisterRawInputDevices(const void* devices, u32 count,
                                            u32 size) {
  if (size != 16) return 0;  // sizeof(RAWINPUTDEVICE) on x64
  return trt_raw_register(devices, count) ? 1 : 0;
}

u32 TRT_STDCALL trt_GetRawInputData(void* hraw, u32 cmd, void* out,
                                    u32* out_size, u32 header_size) {
  if (!out_size) return (u32)-1;
  const u8* blob = nullptr;
  u32 blob_size = 0;
  if (!trt_raw_blob((u64)(uintptr_t)hraw, &blob, &blob_size))
    return (u32)-1;  // stale/unknown handle: real Windows also -1s this
  if (cmd != 0x10000003 /*RID_INPUT*/) return (u32)-1;
  if (!out) return blob_size;  // size query
  if (*out_size < blob_size || header_size != 24) return (u32)-1;
  memcpy(out, blob, blob_size);
  return blob_size;
}

u32 TRT_STDCALL trt_MapVirtualKeyW(u32 code, u32 type) {
  // type 0: VK->scan, 1: scan->VK, 2: VK->char, 3: scan->VK extended
  if (type == 0) return trt_key_scan((u16)code);
  if (type == 2) return code >= 0x41 && code <= 0x5A ? code - 0x41 + 'a' : 0;
  return 0;  // scan->VK needs the layout table (M2)
}

int TRT_STDCALL trt_ShowCursor(int show) {
  static int counter = 0;
  counter += show >= 0 ? 1 : -1;
  if (counter < 0) counter = 0;
  trt_set_cursor_visible(counter > 0);
  return counter;
}

void* TRT_STDCALL trt_SetCapture(void* hwnd) { return hwnd; }
int TRT_STDCALL trt_ReleaseCapture(void) { return 1; }
void* TRT_STDCALL trt_GetCapture(void) { return nullptr; }
void* TRT_STDCALL trt_GetForegroundWindow(void) { return trt_GetActiveWindow(); }
u64 TRT_STDCALL trt_GetKeyboardLayout(u32 tid) {
  (void)tid;
  return 0x04090409;  // en-US
}
i64 TRT_STDCALL trt_SendMessageW(void* hwnd, u32 msg, u64 wp, i64 lp) {
  guest_msg m{};
  m.hwnd = (u64)(uintptr_t)hwnd;
  m.message = msg;
  m.wParam = wp;
  m.lParam = lp;
  return trt_DispatchMessageW(&m);
}

static const trt_named_fn k_user32_fns[] = {
  {"RegisterClassExW", (void*)&trt_RegisterClassExW},
  {"CreateWindowExW", (void*)&trt_CreateWindowExW},
  {"DestroyWindow", (void*)&trt_DestroyWindow},
  {"ShowWindow", (void*)&trt_ShowWindow},
  {"UpdateWindow", (void*)&trt_UpdateWindow},
  {"DefWindowProcW", (void*)&trt_DefWindowProcW},
  {"GetActiveWindow", (void*)&trt_GetActiveWindow},
  {"GetMessageW", (void*)&trt_GetMessageW},
  {"PeekMessageW", (void*)&trt_PeekMessageW},
  {"PostMessageW", (void*)&trt_PostMessageW},
  {"TranslateMessage", (void*)&trt_TranslateMessage},
  {"DispatchMessageW", (void*)&trt_DispatchMessageW},
  {"GetAsyncKeyState", (void*)&trt_GetAsyncKeyState},
  {"GetKeyState", (void*)&trt_GetKeyState},
  {"GetKeyboardState", (void*)&trt_GetKeyboardState},
  {"GetCursorPos", (void*)&trt_GetCursorPos},
  {"SetCursorPos", (void*)&trt_SetCursorPos},
  {"ClipCursor", (void*)&trt_ClipCursor},
  {"GetSystemMetrics", (void*)&trt_GetSystemMetrics},
  {"LoadCursorW", (void*)&trt_LoadCursorW},
  {"LoadIconW", (void*)&trt_LoadIconW},
  {"RegisterRawInputDevices", (void*)&trt_RegisterRawInputDevices},
  {"GetRawInputData", (void*)&trt_GetRawInputData},
  {"MapVirtualKeyW", (void*)&trt_MapVirtualKeyW},
  {"ShowCursor", (void*)&trt_ShowCursor},
  {"SetCapture", (void*)&trt_SetCapture},
  {"ReleaseCapture", (void*)&trt_ReleaseCapture},
  {"GetCapture", (void*)&trt_GetCapture},
  {"GetForegroundWindow", (void*)&trt_GetForegroundWindow},
  {"GetKeyboardLayout", (void*)&trt_GetKeyboardLayout},
  {"SendMessageW", (void*)&trt_SendMessageW},
};

int trt_bridge_register_user32(void) {
  trt_module_register("user32.dll", TRT_ROUTE_BUILTIN, k_user32_fns,
                      (int)(sizeof(k_user32_fns) / sizeof(k_user32_fns[0])));
  return 0;
}

} // extern "C"
