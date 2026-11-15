// tiramisu/src/trt/bridge_gameinput.h
// Microsoft GameInput (gameinput.dll) COM bridge backed by Tiramisu's
// multi-slot XInput + keyboard/mouse input state (input.h).
#ifndef TIRAMISU_BRIDGE_GAMEINPUT_H
#define TIRAMISU_BRIDGE_GAMEINPUT_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

int trt_bridge_register_gameinput(void);

#ifdef __cplusplus
}
#endif

#endif  // TIRAMISU_BRIDGE_GAMEINPUT_H
