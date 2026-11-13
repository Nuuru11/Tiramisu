// tiramisu/src/trt/bridge_psapi.h
// PSAPI for the guest (bridge_psapi.cpp).
//
// K32EnumProcessModules / K32GetModuleFileNameExW / EnumProcesses return the
// REAL guest module list (the loader's image registry) — a game that asks
// "which DLLs am I made of" gets the true answer. Everything else fails
// with last-error + a named log. console presence of psapi.dll is
// UNPROVEN, so the bridge takes over the whole module.
#ifndef TIRAMISU_BRIDGE_PSAPI_H
#define TIRAMISU_BRIDGE_PSAPI_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register psapi.dll as BUILTIN.
int trt_bridge_register_psapi(void);

#ifdef __cplusplus
}
#endif

#endif
