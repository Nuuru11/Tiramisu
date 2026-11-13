// tiramisu/src/trt/bridge_mem.h
// Memory-mapped files for the guest (bridge_mem.cpp).
#ifndef TIRAMISU_BRIDGE_MEM_H
#define TIRAMISU_BRIDGE_MEM_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

int trt_bridge_register_memory(void);

#ifdef __cplusplus
}
#endif

#endif
