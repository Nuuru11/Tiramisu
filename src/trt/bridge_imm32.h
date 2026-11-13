// tiramisu/src/trt/bridge_imm32.h
// IMM32 for the guest (bridge_imm32.cpp).
//
// The console has no IME. ImmGetContext -> NULL (no input-context record),
// the associate/release/set calls are defensive no-ops that log their name.
// A game that guards on ImmGetContext(NULL) at boot simply skips its IME
// path — that is the honest answer, not a fake context handle.
#ifndef TIRAMISU_BRIDGE_IMM32_H
#define TIRAMISU_BRIDGE_IMM32_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register imm32.dll as BUILTIN.
int trt_bridge_register_imm32(void);

#ifdef __cplusplus
}
#endif

#endif
