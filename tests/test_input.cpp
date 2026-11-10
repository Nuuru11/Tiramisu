// tiramisu/tests/test_input.cpp
// Offline proof for the input bridge: host feed -> user32 getters + pump.
#include "../src/trt/input.h"

#include <stdio.h>

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

  printf(failures ? "\nRESULT: %d FAILURES\n" : "\nRESULT: ALL PASS\n",
         failures);
  return failures ? 1 : 0;
}
