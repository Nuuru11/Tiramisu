// tiramisu/src/trt/bridge_ntdll.h
// Native NT / Rtl / ETW surface for ntdll.dll (bridge_ntdll.cpp).
#ifndef TIRAMISU_BRIDGE_NTDLL_H
#define TIRAMISU_BRIDGE_NTDLL_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

int trt_bridge_register_ntdll(void);

#ifdef __cplusplus
}
#endif

#endif // TIRAMISU_BRIDGE_NTDLL_H
