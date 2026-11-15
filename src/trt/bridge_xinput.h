// tiramisu/src/trt/bridge_xinput.h
// XInput bridge (xinput1_4.dll, xinput1_3.dll, xinput9_1_0.dll, xinputuap.dll).
// Wires guest XInputGetState / XInputSetState / XInputGetCapabilities (both by
// name and by ordinals #2, #3, #4, #100) to the Tiramisu input state in input.h.
#ifndef TIRAMISU_BRIDGE_XINPUT_H
#define TIRAMISU_BRIDGE_XINPUT_H

#ifdef __cplusplus
extern "C" {
#endif

int trt_bridge_register_xinput(void);

#ifdef __cplusplus
}
#endif

#endif
