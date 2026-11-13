// tiramisu/tests/test_input.cpp
// Offline proof for the input bridge: host feed -> user32 getters + pump.
#include "../src/trt/input.h"
#include "../src/trt/pe.h"  // TRT_STDCALL: these are guest-ABI bridge fns —
                            // the local declaration MUST match the
                            // definition's convention or the arguments
                            // arrive in the wrong registers (STAGE 6)

#include <stdio.h>
#include <string.h>
#include <stdint.h>

extern "C" {
int TRT_STDCALL trt_RegisterRawInputDevices(const void*, u32, u32);
u32 TRT_STDCALL trt_GetRawInputData(void*, u32, void*, u32*, u32);
}

extern "C" {
int trt_bridge_register_user32(void);
void trt_bridge_boot(const void* be);
}

int failures = 0;
#define CHECK(cond, msg)                              \
  do {                                                \
    if (cond) printf("  PASS %s\n", msg);             \
    else { printf("  FAIL %s\n", msg); ++failures; }  \
  } while (0)

int main() {
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("tiramisu offline input bridge test\n");
  trt_bridge_boot(nullptr);

  // keyboard: feed W down/up, read async/sync; drain its queue, then test A
  trt_input_key_event('W', true);
  CHECK(trt_key_down('W'), "W down");
  CHECK(trt_key_async('W') < 0, "GetAsyncKeyState negative while down");
  u8 ks[256];
  (void)ks;
  trt_input_key_event('W', false);
  CHECK(!trt_key_down('W'), "W released");
  CHECK(trt_key_async('W') == 1, "GetAsyncKeyState edge bit after release frame");
  {  // drain W's keydown/keyup from the queue
    trt_msg t{};
    while (trt_msg_peek(&t, true)) {}
  }

  // mouse: absolute moves -> position + consumed deltas
  trt_input_mouse_move(100, 100);
  trt_input_mouse_move(112, 95);
  int x = 0, y = 0;
  trt_mouse_pos(&x, &y);
  CHECK(x == 112 && y == 95, "mouse position");
  int dx = 0, dy = 0;
  trt_mouse_delta(&dx, &dy);
  CHECK(dx == 12 && dy == -5, "mouse deltas accumulate + consume");
  trt_mouse_delta(&dx, &dy);
  CHECK(dx == 0 && dy == 0, "deltas empty after consume");

  // buttons
  trt_input_mouse_button(0, true);
  CHECK(trt_mouse_button(0), "left button down");
  trt_input_mouse_button(0, false);
  CHECK(!trt_mouse_button(0), "left button up");

  // pad
  trt_input_pad_state(32767, 0, 0, -32768, 0x1000 /*RT*/, 0, 255);
  CHECK(trt_pad_down(0x1000), "pad RT bit");
  CHECK(trt_pad_stick(0) > 0.9f, "LX normalized high");
  CHECK(trt_pad_stick(3) < -0.9f, "RY normalized low");
  CHECK(trt_pad_trigger(1) == 255, "right trigger");

  // message queue: drain all (mouse/pad noise), then a clean A down/up pair
  { trt_msg t{}; while (trt_msg_peek(&t, true)) {} }
  trt_input_key_event('A', true);
  trt_input_key_event('A', false);
  CHECK(trt_msg_pending(), "queue pending");
  trt_msg m{};
  CHECK(trt_msg_peek(&m, true) && m.message == 0x0100 && m.wparam == 'A',
        "WM_KEYDOWN A");
  CHECK(trt_msg_peek(&m, true) && m.message == 0x0101 && m.wparam == 'A',
        "WM_KEYUP A");
  CHECK(!trt_msg_pending(), "queue drained");

  // ---- game-grade surface ----
  // scan codes ride WM_KEYDOWN lParam (games read lParam>>16)
  {
    trt_msg t{};
    while (trt_msg_peek(&t, true)) {}
    trt_input_key_event2(0x11 /*VK_W? no, VK 'W'=0x57*/, true, 0x11, 0, 0);
    CHECK(trt_msg_peek(&t, true) && t.message == 0x0100 &&
              (((u32)t.lparam) >> 16) == 0x11,
          "scan code in KEYDOWN lParam");
  }
  // WM_CHAR from char events
  {
    trt_msg t{};
    while (trt_msg_peek(&t, true)) {}
    trt_input_char_event('h');
    CHECK(trt_msg_peek(&t, true) && t.message == 0x0102 && t.wparam == 'h',
          "WM_CHAR posted");
  }
  // Raw Input: register mouse+keyboard, raw deltas synthesize WM_INPUT with a
  // handle that GetRawInputData decodes back to the deltas
  {
    struct rid { u16 page, usage; u32 flags; u64 hwnd; };
    rid devs[2] = {{1, 2, 0, 0}, {1, 6, 0, 0}};  // mouse + keyboard
    CHECK(trt_RegisterRawInputDevices(devs, 2, 16) == 1,
          "RegisterRawInputDevices");
    { trt_msg t{}; while (trt_msg_peek(&t, true)) {} }
    trt_input_mouse_delta_raw(9, -7);
    trt_msg t{};
    CHECK(trt_msg_peek(&t, true) && t.message == 0x00FF, "WM_INPUT posted");
    u8 buf[64];
    u32 sz = sizeof(buf);
    const u32 got =
        trt_GetRawInputData((void*)(uintptr_t)t.lparam, 0x10000003, buf, &sz, 24);
    CHECK(got == 48, "GetRawInputData returns RAWMOUSE size");
    int rdx = 0, rdy = 0;
    memcpy(&rdx, buf + 36, 4);
    memcpy(&rdy, buf + 40, 4);
    CHECK(rdx == 9 && rdy == -7, "RAWMOUSE lLastX/lLastY round-trip");
    // size query form
    u32 q = 0;
    CHECK(trt_GetRawInputData((void*)(uintptr_t)t.lparam, 0x10000003, nullptr,
                              &q, 24) == 48,
          "GetRawInputData size query");
    // stale handle -> -1 (0xFFFFFFFF), no crash
    CHECK(trt_GetRawInputData((void*)(uintptr_t)0xdeadbeef, 0x10000003, buf,
                              &sz, 24) == 0xFFFFFFFFu,
          "stale hRawInput -> -1");
  }

  printf(failures ? "\nRESULT: %d FAILURES\n" : "\nRESULT: ALL PASS\n",
         failures);
  return failures ? 1 : 0;
}
