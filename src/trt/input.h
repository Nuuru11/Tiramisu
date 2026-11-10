// tiramisu/src/trt/input.h
// Input provider: the host feeds it (CoreWindow keyboard/pointer + XInput),
// guests read it through the user32 bridge. Same process, one consumer
// thread (the guest runs on the host loop), so a lock only guards handoff.
#ifndef TIRAMISU_INPUT_H
#define TIRAMISU_INPUT_H

#include "pe.h"

#define TRT_MAX_VK 256

// message ring for the guest pump (user32 GetMessage/PeekMessage)
typedef struct trt_msg {
  u32 message;   // WM_*
  u64 wparam;
  i64 lparam;
} trt_msg;

#define TRT_MSG_RING 512

#ifdef __cplusplus
extern "C" {
#endif

// ---- host side ----
void trt_input_key_event(u16 vk, bool down);          // CoreWindow KeyDown/Up
void trt_input_char_event(u32 ch);                     // for WM_CHAR
void trt_input_mouse_move(int x, int y);               // absolute; deltas accumulate
void trt_input_mouse_button(int btn /*0=L 1=R 2=M*/, bool down);
void trt_input_wheel(i32 delta);
void trt_input_pad_state(i16 lx, i16 ly, i16 rx, i16 ry,
                         u16 buttons /*XINPUT_GAMEPAD_* bits*/,
                         u8 trigger_l, u8 trigger_r);
void trt_input_display_size(int w, int h);             // GetSystemMetrics
void trt_input_frame_begin();                          // clears per-frame bits

// ---- guest side (user32 bridge) ----
bool  trt_key_down(u16 vk);
i16   trt_key_async(u16 vk);                           // GetAsyncKeyState value
i16   trt_key_sync(u16 vk);                            // GetKeyState value
int   trt_mouse_pos(int* x, int* y);
void  trt_mouse_delta(int* dx, int* dy);               // consumes accumulation
bool  trt_mouse_button(int btn);
i32   trt_mouse_wheel();                               // consumes
bool  trt_pad_down(u16 xinput_bit);
float trt_pad_stick(int stick /*0=Lx 1=Ly 2=Rx 3=Ry*/);
u8    trt_pad_trigger(int t /*0=L 1=R*/);
int   trt_display_size(int* w, int* h);

// ---- message queue ----
bool trt_msg_post(u32 message, u64 wparam, i64 lparam);
bool trt_msg_peek(trt_msg* out, bool remove);
bool trt_msg_pending();

#ifdef __cplusplus
}
#endif

#endif
