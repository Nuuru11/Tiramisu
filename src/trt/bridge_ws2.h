// tiramisu/src/trt/bridge_ws2.h
// Real WinSock (ws2_32.dll / wsock32.dll) runtime bridge.
//   - Registers wsock32.dll as TRT_ROUTE_BUILTIN (both offline and on console).
//   - Keeps ws2_32.dll TRT_ROUTE_SYSTEM (Xbox OS DLL on console) while
//     providing trt_ws2_resolve() for offline test_corpus_run bindings.
#ifndef TIRAMISU_BRIDGE_WS2_H
#define TIRAMISU_BRIDGE_WS2_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

int trt_bridge_register_ws2(void);
void* trt_ws2_resolve(const char* symbol);

#ifdef __cplusplus
}
#endif

#endif
