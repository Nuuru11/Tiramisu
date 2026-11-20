// tiramisu/src/trt/fna_platform.h
// Native side of the Tiramisu_FNAPlatform backend (Path B, Celeste).
//
// The managed backend (third_party/fna/Tiramisu_FNAPlatform.cs) reaches
// the host through Mono internal calls, registered by mono_host.cpp with
// trt_fna_register_internal_calls(). This file never links the Mono
// runtime itself: mono_host passes mono_add_internal_call in, so the
// same code compiles into the UWP host (future Mono-on-UWP) and the
// Linux offline harness.
//
// What the native side provides:
//   * the host window handle (CoreWindow ICoreWindow*, set by the host
//     in SetWindow; NULL headless) for FNA's CreateWindow / the future
//     FNA3D D3D11 driver,
//   * the host storage root (UWP LocalState, set at boot) for saves,
//   * input, straight from the existing trt input layer: the message
//     ring (WM_KEYDOWN/UP/CHAR, mouse buttons, wheel) for PollEvents,
//     polled mouse position/buttons/deltas for GetMouseState, and the
//     XInput pad slots (polled by the host every frame) for gamepads.
//     Vibration requests flow back through trt_pad_set_vibration and
//     the host's existing XInputSetState consumer.
//
// CONSOLE-VERIFICATION-PENDING: the CoreWindow handle plumbing and real
// XInput state only exist on the console. Offline the layer runs
// headless (NULL window, 1920x1080 fallback display size from
// trt_display_size, no pads) and must initialize without crashing;
// nothing here claims the console path works until it is tested there.
#ifndef TIRAMISU_FNA_PLATFORM_H
#define TIRAMISU_FNA_PLATFORM_H

#include <cstdint>

#include "pe.h"

#ifdef __cplusplus
extern "C" {
#endif

// ABI version of the managed<->native internal-call surface. The managed
// backend checks it in ProgramInit and refuses to run on a mismatch.
#define TRT_FNA_ABI_VERSION 1

// ---- host side (called by src/host on UWP; inert on Linux) ----
// The CoreWindow the game renders into, as an ICoreWindow* ABI pointer
// (winrt::get_abi). NULL until SetWindow runs (headless before that).
void trt_fna_set_window_handle(void* corewindow_abi);
void* trt_fna_window_handle(void);
// Writable root for game saves (UWP LocalState folder, UTF-8). NULL
// until boot_all provides it.
void trt_fna_set_storage_root(const char* utf8_path);
const char* trt_fna_storage_root(void);
// 1 when no host window is registered (offline harness / pre-SetWindow).
int trt_fna_is_headless(void);

// ---- Mono internal-call implementations ----
// Signatures must match the [InternalCall] declarations in
// Tiramisu_FNAPlatform.cs exactly: C# int <-> int32_t, C# long <->
// int64_t, C# IntPtr <-> void*. All functions are safe to call from the
// Mono game thread; the input layer serializes with its own lock.
int32_t TiramisuFNA_Ping(void);
void* TiramisuFNA_GetWindowHandle(void);
void TiramisuFNA_GetDisplaySize(int32_t* w, int32_t* h);
// Dequeues one message from the host message ring; 1 = event dequeued,
// 0 = ring empty. Unrecognized messages are dropped by the caller.
int32_t TiramisuFNA_PollEvent(int32_t* message, int64_t* wparam,
                              int64_t* lparam);
void TiramisuFNA_GetMouseState(int32_t* x, int32_t* y, int32_t* buttons);
void TiramisuFNA_GetMouseDelta(int32_t* dx, int32_t* dy);
void TiramisuFNA_SetMousePosition(int32_t x, int32_t y);
void TiramisuFNA_SetCursorVisible(int32_t visible);
// 1 = pad connected (state filled), 0 = not connected / bad index.
// Sticks are raw XInput range; triggers are 0..255; buttons is the
// XInput wButtons word (bits identical to XNA Buttons).
int32_t TiramisuFNA_GetPadState(int32_t index, int32_t* lx, int32_t* ly,
                                int32_t* rx, int32_t* ry, int32_t* buttons,
                                int32_t* tl, int32_t* tr);
// 1 = request accepted (pad connected), 0 = not connected / bad index.
// Motors are 0..0xFFFF; the host applies them via XInputSetState.
int32_t TiramisuFNA_SetPadVibration(int32_t index, int32_t left_motor,
                                    int32_t right_motor);
// Pointer to a static NUL-terminated UTF-8 buffer (the host storage
// root), or NULL when the host has not provided one. The buffer is
// written once at boot and never freed; callers must not free it.
void* TiramisuFNA_GetStorageRoot(void);
int32_t TiramisuFNA_IsHeadless(void);

// Registration: mono_host passes mono_add_internal_call (resolved from
// libmono at runtime) so this file never links the Mono runtime itself.
typedef void (*trt_fna_add_internal_call_fn)(const char* name,
                                             const void* method);
void trt_fna_register_internal_calls(trt_fna_add_internal_call_fn add);

#ifdef __cplusplus
}
#endif

#endif  // TIRAMISU_FNA_PLATFORM_H
