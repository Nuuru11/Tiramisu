// tiramisu/src/trt/input.cpp
#include "input.h"

#include <math.h>
#include <string.h>

namespace {

struct input_state {
  // keyboard
  u8  down[TRT_MAX_VK];      // current
  u8  pressed[TRT_MAX_VK];   // transitioned down this frame
  u16 scan[TRT_MAX_VK];      // last scan code per VK (games read lParam)
  u16 last_vk;               // for GetAsyncKeyState bit15
  u32 char_q[64]; int char_count;

  // mouse
  int mx, my;                // virtual-cursor position
  int dx_acc, dy_acc;        // accumulated deltas since last consume
  bool mouse_init;           // first absolute event positions, no delta
  int rdx, rdy;              // last REAL mouse deltas (MouseDevice)
  u8  mbtn;                  // bit0 L, bit1 R, bit2 M
  i32 wheel_acc;

  // pad (XINPUT_GAMEPAD_* bits mirrored directly)
  i16 lx, ly, rx, ry;
  u16 pbtn;
  u8  tl, tr;

  // display
  int dw, dh;

  // cursor visibility hook (ShowCursor)
  bool cursor_visible;

  // raw input: registered devices + a small ring of RAWINPUT blobs
  u8  raw_mouse_reg, raw_kbd_reg;
  u64 raw_next_handle;
  struct raw_rec { u64 handle; u8 blob[48]; u32 size; };
  raw_rec raw_ring[16];
  int raw_ring_len;

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

// raw-blob feeders (defined below; used by the host-feed section)
void trt_raw_post_mouse(i32 dx, i32 dy, u8 mbtn, i32 wheel);
void trt_raw_post_key(u16 vk, u16 scan, bool down);

// ---------------- host feed ----------------
void trt_input_key_event(u16 vk, bool down) {
  trt_input_key_event2(vk, down, 0, 0, 0);
}

void trt_input_key_event2(u16 vk, bool down, u16 scan, u8 extended, u8 repeat) {
  if (vk >= TRT_MAX_VK) return;
  if (down && !g.down[vk]) g.pressed[vk] = 1;
  g.down[vk] = down ? 1 : 0;
  if (scan) g.scan[vk] = scan;
  if (down) g.last_vk = vk;
  // WM_KEYDOWN/UP lParam: repeat(16) scan(9)<<16 ext(1)<<24 prev(1)<<30 up(1)<<31
  const u32 lp = (u32)repeat | ((u32)scan << 16) | ((u32)(extended != 0) << 24) |
                 (down ? 0 : 0xC0000000u);
  trt_msg_post(down ? 0x0100 /*WM_KEYDOWN*/ : 0x0101 /*WM_KEYUP*/, vk,
               (i64)(u64)lp);
  if (g.raw_kbd_reg) trt_raw_post_key(vk, scan, down);
}

void trt_input_char_event(u32 ch) {
  if (g.char_count < 64) g.char_q[g.char_count++] = ch;
  trt_msg_post(0x0102 /*WM_CHAR*/, ch, 0);
}

void trt_input_mouse_delta_raw(i32 dx, i32 dy) {
  // real mouse (MouseDevice.MouseMoved): feed BOTH consumers
  g.rdx = dx;
  g.rdy = dy;
  g.dx_acc += dx;  // GetCursorPos-based games + WM_MOUSEMOVE keep working
  g.dy_acc += dy;
  // synthesize WM_INPUT for engines on the Raw Input path
  if (g.raw_mouse_reg) trt_raw_post_mouse(dx, dy, g.mbtn, g.wheel_acc);
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

void trt_mouse_delta_raw(int* dx, int* dy) {
  if (dx) *dx = g.rdx;
  if (dy) *dy = g.rdy;
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

u16 trt_key_scan(u16 vk) { return vk < TRT_MAX_VK ? g.scan[vk] : 0; }

void trt_set_cursor_visible(bool v) { g.cursor_visible = v; }

// ---------------- raw input (WM_INPUT path) ----------------
// RAWINPUT blob: RAWINPUTHEADER(24) + RAWMOUSE(24) or RAWKEYBOARD(16)
void trt_raw_post_mouse(i32 dx, i32 dy, u8 mbtn, i32 wheel) {
  (void)mbtn; (void)wheel;  // buttons ride the WM_ path
  input_state::raw_rec& r =
      g.raw_ring[g.raw_ring_len < 16 ? g.raw_ring_len++ : 15];
  r.handle = ++g.raw_next_handle;
  r.size = 48;
  memset(r.blob, 0, sizeof(r.blob));
  const u32 type = 0;         // RIM_TYPEMOUSE
  const u32 size = 48;
  const u64 dev = 0, wp = 0;  // hDevice, wParam RIM_INPUT
  const u16 flags = 0;        // MOUSE_MOVE_RELATIVE
  const u32 none = 0;         // ulRawButtons
  memcpy(r.blob + 0, &type, 4);
  memcpy(r.blob + 4, &size, 4);
  memcpy(r.blob + 8, &dev, 8);
  memcpy(r.blob + 16, &wp, 8);
  memcpy(r.blob + 24, &flags, 2);
  memcpy(r.blob + 32, &none, 4);
  memcpy(r.blob + 36, &dx, 4);   // lLastX
  memcpy(r.blob + 40, &dy, 4);   // lLastY
  trt_msg_post(0x00FF /*WM_INPUT*/, 0, (i64)r.handle);
}

void trt_raw_post_key(u16 vk, u16 scan, bool down) {
  input_state::raw_rec& r =
      g.raw_ring[g.raw_ring_len < 16 ? g.raw_ring_len++ : 15];
  r.handle = ++g.raw_next_handle;
  r.size = 40;
  memset(r.blob, 0, sizeof(r.blob));
  const u32 type = 1;          // RIM_TYPEKEYBOARD
  const u32 size = 40;
  const u32 msg = down ? 0x0100 : 0x0101;
  memcpy(r.blob + 0, &type, 4);
  memcpy(r.blob + 4, &size, 4);
  memcpy(r.blob + 24, &scan, 2);  // MakeCode
  memcpy(r.blob + 28, &vk, 2);    // VKey
  memcpy(r.blob + 32, &msg, 4);   // Message
  trt_msg_post(0x00FF /*WM_INPUT*/, 0, (i64)r.handle);
}

bool trt_raw_register(const void* devices, u32 count) {
  if (!devices || count == 0 || count > 8) return false;
  // RAWINPUTDEVICE: usUsagePage(2) usUsage(2) dwFlags(4) hwndTarget(8)
  struct rid { u16 page; u16 usage; u32 flags; u64 hwnd; };
  const rid* d = (const rid*)devices;
  for (u32 i = 0; i < count; ++i) {
    if (d[i].page == 1 && d[i].usage == 2) g.raw_mouse_reg = 1;  // mouse
    if (d[i].page == 1 && d[i].usage == 6) g.raw_kbd_reg = 1;    // keyboard
  }
  return true;
}

bool trt_raw_blob(u64 hraw, const u8** blob, u32* size) {
  for (int i = 0; i < g.raw_ring_len; ++i)
    if (g.raw_ring[i].handle == hraw) {
      if (blob) *blob = g.raw_ring[i].blob;
      if (size) *size = g.raw_ring[i].size;
      return true;
    }
  return false;
}

// ---------------- message queue ----------------
unsigned long g_msg_dispatched_count = 0;  // STAGE 7 pump-dispatch counter
unsigned trt_msg_dispatched(void);         // (definition below, in trt.h ABI)

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
    ++g_msg_dispatched_count;
  }
  return true;
}

bool trt_msg_pending() { return g.ring_count > 0; }

unsigned trt_msg_dispatched(void) { return (unsigned)g_msg_dispatched_count; }

} // extern "C"
