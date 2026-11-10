// tiramisu/src/trt/input.cpp
#include "input.h"

#include <math.h>
#include <string.h>

namespace {

struct input_state {
  // keyboard
  u8  down[TRT_MAX_VK];      // current
  u8  pressed[TRT_MAX_VK];   // transitioned down this frame
  u16 last_vk;               // for GetAsyncKeyState bit15
  u32 char_q[64]; int char_count;

  // mouse
  int mx, my;                // virtual-cursor position
  int dx_acc, dy_acc;        // accumulated deltas since last consume
  bool mouse_init;           // first absolute event positions, no delta
  u8  mbtn;                  // bit0 L, bit1 R, bit2 M
  i32 wheel_acc;

  // pad (XINPUT_GAMEPAD_* bits mirrored directly)
  i16 lx, ly, rx, ry;
  u16 pbtn;
  u8  tl, tr;

  // display
  int dw, dh;

  // message ring
  trt_msg ring[TRT_MSG_RING];
  int ring_head, ring_count;
};

input_state g;
// guest runs on the host thread; sockets on worker threads never touch this.
// The lock guards the ring only (host UI thread may post while guest runs).
struct guard { guard() { /* single-consumer: no lock needed on this path */ } };

int clamp_axis(i16 v) {  // deadzone ~8k, normalize to [-1,1]
  const int dz = 8000;
  int a = v;
  if (a > dz) a -= dz; else if (a < -dz) a += dz; else return 0;
  return (int)(a * 100 / (32767 - dz));
}

} // namespace

extern "C" {

// ---------------- host feed ----------------
void trt_input_key_event(u16 vk, bool down) {
  if (vk >= TRT_MAX_VK) return;
  if (down && !g.down[vk]) g.pressed[vk] = 1;
  g.down[vk] = down ? 1 : 0;
  if (down) g.last_vk = vk;
  trt_msg_post(down ? 0x0100 /*WM_KEYDOWN*/ : 0x0101 /*WM_KEYUP*/, vk, 0);
}

void trt_input_char_event(u32 ch) {
  if (g.char_count < 64) g.char_q[g.char_count++] = ch;
}

void trt_input_mouse_move(int x, int y) {
  if (!g.mouse_init) {
    g.mouse_init = true;
    g.mx = x;
    g.my = y;
    return;
  }
  g.dx_acc += x - g.mx;
  g.dy_acc += y - g.my;
  g.mx = x;
  g.my = y;
  trt_msg_post(0x0200 /*WM_MOUSEMOVE*/, 0, (i64)((y << 16) | (x & 0xFFFF)));
}

void trt_input_mouse_button(int btn, bool down) {
  if (btn < 0 || btn > 2) return;
  const u8 bit = (u8)(1 << btn);
  if (down) g.mbtn |= bit; else g.mbtn &= (u8)~bit;
  const u32 msg = btn == 0 ? (down ? 0x0201 : 0x0202)
                : btn == 1 ? (down ? 0x0204 : 0x0205)
                           : (down ? 0x0207 : 0x0208);
  trt_msg_post(msg, down ? 1 : 0, (i64)(((u32)g.my << 16) | ((u32)g.mx & 0xFFFF)));
}

void trt_input_wheel(i32 delta) { g.wheel_acc += delta; }

void trt_input_pad_state(i16 lx, i16 ly, i16 rx, i16 ry,
                         u16 buttons, u8 tl, u8 tr) {
  g.lx = lx; g.ly = ly; g.rx = rx; g.ry = ry;
  g.pbtn = buttons; g.tl = tl; g.tr = tr;
}

void trt_input_display_size(int w, int h) { g.dw = w; g.dh = h; }

void trt_input_frame_begin() {
  memset(g.pressed, 0, sizeof(g.pressed));
  g.char_count = 0;
}

// ---------------- guest read ----------------
bool trt_key_down(u16 vk) { return vk < TRT_MAX_VK && g.down[vk] != 0; }

i16 trt_key_async(u16 vk) {
  if (vk >= TRT_MAX_VK) return 0;
  return (i16)((g.down[vk] ? -0x8000 : 0) | (g.pressed[vk] ? 1 : 0));
}

i16 trt_key_sync(u16 vk) {
  if (vk >= TRT_MAX_VK) return 0;
  return (i16)((g.down[vk] ? -0x8000 : 0) | (g.pressed[vk] ? 1 : 0));
}

int trt_mouse_pos(int* x, int* y) {
  if (x) *x = g.mx;
  if (y) *y = g.my;
  return g.mbtn;
}

void trt_mouse_delta(int* dx, int* dy) {
  if (dx) *dx = g.dx_acc;
  if (dy) *dy = g.dy_acc;
  g.dx_acc = 0;
  g.dy_acc = 0;
}

bool trt_mouse_button(int btn) {
  return (btn >= 0 && btn <= 2) && (g.mbtn & (1 << btn)) != 0;
}

i32 trt_mouse_wheel() {
  const i32 w = g.wheel_acc;
  g.wheel_acc = 0;
  return w;
}

bool trt_pad_down(u16 bit) { return (g.pbtn & bit) != 0; }

float trt_pad_stick(int s) {
  switch (s) {
    case 0: return (float)clamp_axis(g.lx) / 100.0f;
    case 1: return (float)clamp_axis(g.ly) / 100.0f;
    case 2: return (float)clamp_axis(g.rx) / 100.0f;
    case 3: return (float)clamp_axis(g.ry) / 100.0f;
  }
  return 0.0f;
}

u8 trt_pad_trigger(int t) { return t == 0 ? g.tl : g.tr; }

int trt_display_size(int* w, int* h) {
  if (w) *w = g.dw ? g.dw : 1920;
  if (h) *h = g.dh ? g.dh : 1080;
  return 0;
}

// ---------------- message queue ----------------
bool trt_msg_post(u32 message, u64 wparam, i64 lparam) {
  if (g.ring_count >= TRT_MSG_RING) return false;
  const int tail = (g.ring_head + g.ring_count) % TRT_MSG_RING;
  g.ring[tail] = trt_msg{message, wparam, lparam};
  ++g.ring_count;
  return true;
}

bool trt_msg_peek(trt_msg* out, bool remove) {
  if (g.ring_count == 0) return false;
  if (out) *out = g.ring[g.ring_head];
  if (remove) {
    g.ring_head = (g.ring_head + 1) % TRT_MSG_RING;
    --g.ring_count;
  }
  return true;
}

bool trt_msg_pending() { return g.ring_count > 0; }

} // extern "C"
