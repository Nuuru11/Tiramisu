// tiramisu/src/trt/bridge_thread.h
// Threads and synchronisation for the guest (bridge_thread.cpp).
#ifndef TIRAMISU_BRIDGE_THREAD_H
#define TIRAMISU_BRIDGE_THREAD_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

int trt_bridge_register_threads(void);

#ifdef __cplusplus
}
#endif

#endif
