// tiramisu/src/trt/fna_platform.cpp
// Native side of the Tiramisu_FNAPlatform backend. See fna_platform.h
// for the contract. All input reads go through the existing trt input
// layer (input.h) — the same state native guests read — so the FNA
// backend and the native guest path can never disagree about input.
#include "fna_platform.h"
#include "trt_trace.h"

#include <atomic>
#include <cstring>
#include <cstdio>

#include "input.h"
#include "thunk.h"

namespace {

// The host window handle. Written once by the host's SetWindow (UI
// thread) before any game launches; read by the Mono game thread.
std::atomic<void*> g_window_handle{nullptr};

// The host storage root (UWP LocalState), UTF-8. Written once at boot;
// read by the managed backend through a static pointer.
char g_storage_root[1024] = "";

// one-time trace of the first managed call to each native entry
void fna_trace(const char* name) {
  static std::atomic<int> ping{0}, win{0}, size{0}, poll{0}, mouse{0}, pad{0};
  std::atomic<int>* c = &ping;
  if (!std::strcmp(name, "GetWindowHandle")) c = &win;
  else if (!std::strcmp(name, "GetDisplaySize")) c = &size;
  else if (!std::strcmp(name, "PollEvent")) c = &poll;
  else if (!std::strcmp(name, "GetMouseState") || !std::strcmp(name, "GetMouseDelta")) c = &mouse;
  else if (!std::strcmp(name, "GetPadState")) c = &pad;
  if (c->fetch_add(1) == 0) {
    char line[128];
    std::snprintf(line, sizeof(line), "TRACE fna-native: first %s call", name);
    trt_trace(line);
  }
}

}  // namespace

extern "C" {

void trt_fna_set_window_handle(void* corewindow_abi) {
  g_window_handle.store(corewindow_abi, std::memory_order_release);
}

void* trt_fna_window_handle(void) {
  return g_window_handle.load(std::memory_order_acquire);
}

void trt_fna_set_storage_root(const char* utf8_path) {
  if (!utf8_path || !utf8_path[0]) {
    g_storage_root[0] = '\0';
    return;
  }
  std::strncpy(g_storage_root, utf8_path, sizeof(g_storage_root) - 1);
  g_storage_root[sizeof(g_storage_root) - 1] = '\0';
}

const char* trt_fna_storage_root(void) {
  return g_storage_root[0] ? g_storage_root : nullptr;
}

int trt_fna_is_headless(void) {
  return g_window_handle.load(std::memory_order_acquire) == nullptr;
}

// ---- Mono internal-call implementations ----

int32_t TiramisuFNA_Ping(void) {
  fna_trace("Ping");
  return TRT_FNA_ABI_VERSION;
}

void* TiramisuFNA_GetWindowHandle(void) {
  fna_trace("GetWindowHandle");
  return trt_fna_window_handle();
}

void TiramisuFNA_GetDisplaySize(int32_t* w, int32_t* h) {
  fna_trace("GetDisplaySize");
  int dw = 0, dh = 0;
  // trt_display_size falls back to 1920x1080 when the host has not fed
  // a size (headless), so this never returns 0x0.
  trt_display_size(&dw, &dh);
  if (w) *w = (int32_t)dw;
  if (h) *h = (int32_t)dh;
}

int32_t TiramisuFNA_PollEvent(int32_t* message, int64_t* wparam,
                              int64_t* lparam) {
  fna_trace("PollEvent");
  if (!message || !wparam || !lparam) return 0;
  trt_msg m;
  if (!trt_msg_peek(&m, true)) return 0;
  {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 20) {
      char line[160];
      std::snprintf(line, sizeof(line),
                    "TRACE input: poll msg=0x%x wp=%lld lp=0x%llx",
                    m.message, (long long)m.wparam,
                    (unsigned long long)m.lparam);
      trt_trace(line);
    }
  }
  *message = (int32_t)m.message;
  *wparam = (int64_t)m.wparam;
  *lparam = (int64_t)m.lparam;
  return 1;
}

void TiramisuFNA_GetMouseState(int32_t* x, int32_t* y, int32_t* buttons) {
  int mx = 0, my = 0;
  trt_mouse_pos(&mx, &my);
  // trt mbtn layout: bit0 L, bit1 R, bit2 M, bit3 X1, bit4 X2.
  int32_t bits = 0;
  if (trt_mouse_button(0)) bits |= 1;
  if (trt_mouse_button(1)) bits |= 2;
  if (trt_mouse_button(2)) bits |= 4;
  if (trt_mouse_button(3)) bits |= 8;
  if (trt_mouse_button(4)) bits |= 16;
  if (x) *x = (int32_t)mx;
  if (y) *y = (int32_t)my;
  if (buttons) *buttons = bits;
}

void TiramisuFNA_GetMouseDelta(int32_t* dx, int32_t* dy) {
  int ddx = 0, ddy = 0;
  // Consumes the accumulation (real-mouse deltas + pointer moves).
  trt_mouse_delta(&ddx, &ddy);
  if (dx) *dx = (int32_t)ddx;
  if (dy) *dy = (int32_t)ddy;
}

void TiramisuFNA_SetMousePosition(int32_t x, int32_t y) {
  trt_input_set_cursor_pos((int)x, (int)y);
}

void TiramisuFNA_SetCursorVisible(int32_t visible) {
  trt_set_cursor_visible(visible != 0);
}

int32_t TiramisuFNA_GetPadState(int32_t index, int32_t* lx, int32_t* ly,
                                int32_t* rx, int32_t* ry, int32_t* buttons,
                                int32_t* tl, int32_t* tr) {
  if (lx) *lx = 0;
  if (ly) *ly = 0;
  if (rx) *rx = 0;
  if (ry) *ry = 0;
  if (buttons) *buttons = 0;
  if (tl) *tl = 0;
  if (tr) *tr = 0;
  // Headless: the host is not feeding input, so no pad is present.
  // (Without this, pad 0 reads as connected: trt_pad_connected
  // defaults pad 0 to connected until the first host poll proves
  // otherwise, and headless no poll ever runs.)
  if (trt_fna_is_headless()) return 0;
  if (index < 0 || index >= 4) return 0;
  const u32 idx = (u32)index;
  if (!trt_pad_connected(idx)) return 0;
  if (lx) *lx = (int32_t)trt_pad_thumb_n(idx, 0);
  if (ly) *ly = (int32_t)trt_pad_thumb_n(idx, 1);
  if (rx) *rx = (int32_t)trt_pad_thumb_n(idx, 2);
  if (ry) *ry = (int32_t)trt_pad_thumb_n(idx, 3);
  // Reassemble the XInput button word from the per-bit readers (the
  // input layer exposes no whole-word getter). Bit order is the
  // XINPUT_GAMEPAD_* order; the values are identical to XNA Buttons.
  static const u16 k_bits[14] = {
      0x0001,  // DPAD_UP
      0x0002,  // DPAD_DOWN
      0x0004,  // DPAD_LEFT
      0x0008,  // DPAD_RIGHT
      0x0010,  // START
      0x0020,  // BACK
      0x0040,  // LEFT_THUMB
      0x0080,  // RIGHT_THUMB
      0x0100,  // LEFT_SHOULDER
      0x0200,  // RIGHT_SHOULDER
      0x1000,  // A
      0x2000,  // B
      0x4000,  // X
      0x8000,  // Y
  };
  u16 word = 0;
  for (int i = 0; i < 14; ++i) {
    if (trt_pad_down_n(idx, k_bits[i])) word |= k_bits[i];
  }
  if (buttons) *buttons = (int32_t)word;
  if (tl) *tl = (int32_t)trt_pad_trigger_n(idx, 0);
  if (tr) *tr = (int32_t)trt_pad_trigger_n(idx, 1);
  return 1;
}

int32_t TiramisuFNA_SetPadVibration(int32_t index, int32_t left_motor,
                                    int32_t right_motor) {
  if (trt_fna_is_headless()) return 0;  // no pads headless (see above)
  if (index < 0 || index >= 4) return 0;
  if (!trt_pad_connected((u32)index)) return 0;
  if (left_motor < 0) left_motor = 0;
  if (left_motor > 0xFFFF) left_motor = 0xFFFF;
  if (right_motor < 0) right_motor = 0;
  if (right_motor > 0xFFFF) right_motor = 0xFFFF;
  trt_pad_set_vibration((u32)index, (u16)left_motor, (u16)right_motor);
  return 1;
}

void* TiramisuFNA_GetStorageRoot(void) {
  return g_storage_root[0] ? (void*)g_storage_root : nullptr;
}

int32_t TiramisuFNA_IsHeadless(void) { return trt_fna_is_headless(); }

void trt_fna_register_internal_calls(trt_fna_add_internal_call_fn add) {
  if (!add) return;
  // Names must match the [InternalCall] declarations in
  // Tiramisu_FNAPlatform.cs exactly.
  add("Microsoft.Xna.Framework.Tiramisu_FNAPlatform::NativePing",
      (const void*)TiramisuFNA_Ping);
  add("Microsoft.Xna.Framework.Tiramisu_FNAPlatform::NativeGetWindowHandle",
      (const void*)TiramisuFNA_GetWindowHandle);
  add("Microsoft.Xna.Framework.Tiramisu_FNAPlatform::NativeGetDisplaySize",
      (const void*)TiramisuFNA_GetDisplaySize);
  add("Microsoft.Xna.Framework.Tiramisu_FNAPlatform::NativePollEvent",
      (const void*)TiramisuFNA_PollEvent);
  add("Microsoft.Xna.Framework.Tiramisu_FNAPlatform::NativeGetMouseState",
      (const void*)TiramisuFNA_GetMouseState);
  add("Microsoft.Xna.Framework.Tiramisu_FNAPlatform::NativeGetMouseDelta",
      (const void*)TiramisuFNA_GetMouseDelta);
  add("Microsoft.Xna.Framework.Tiramisu_FNAPlatform::NativeSetMousePosition",
      (const void*)TiramisuFNA_SetMousePosition);
  add("Microsoft.Xna.Framework.Tiramisu_FNAPlatform::NativeSetCursorVisible",
      (const void*)TiramisuFNA_SetCursorVisible);
  add("Microsoft.Xna.Framework.Tiramisu_FNAPlatform::NativeGetPadState",
      (const void*)TiramisuFNA_GetPadState);
  add("Microsoft.Xna.Framework.Tiramisu_FNAPlatform::NativeSetPadVibration",
      (const void*)TiramisuFNA_SetPadVibration);
  add("Microsoft.Xna.Framework.Tiramisu_FNAPlatform::NativeGetStorageRoot",
      (const void*)TiramisuFNA_GetStorageRoot);
  add("Microsoft.Xna.Framework.Tiramisu_FNAPlatform::NativeIsHeadless",
      (const void*)TiramisuFNA_IsHeadless);
}

}  // extern "C"
