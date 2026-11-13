// tiramisu/src/trt/bridge_winmm.h
// winmm.dll for the guest (bridge_winmm.cpp).
#ifndef TIRAMISU_BRIDGE_WINMM_H
#define TIRAMISU_BRIDGE_WINMM_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

int trt_bridge_register_winmm(void);

#ifdef __cplusplus
}
#endif

#endif
