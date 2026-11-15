// tiramisu/src/trt/bridge_nvapi.h
// Honest GPU-vendor stub modules (bridge_nvapi.cpp).
//
// Engines probe the vendor GPU surfaces at boot (nvapi.dll / nvml.dll /
// atiadlxx.dll|atiadlxy.dll) to pick renderer paths, read thermals or cap
// frame rates. The console host has none of those driver surfaces, so every
// export here returns its REAL not-available code — NVAPI_NVIDIA_NOT_FOUND,
// NVML_ERROR_NOT_SUPPORTED, ADL_ERR_NOT_SUPPORTED — exactly once logged,
// so engines bail cleanly instead of guessing, and nothing pretends to be
// hardware that is not there.
#ifndef TIRAMISU_BRIDGE_NVAPI_H
#define TIRAMISU_BRIDGE_NVAPI_H

#include "trt.h"
#include "thunk.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register nvapi.dll + nvml.dll + atiadlxx.dll + atiadlxy.dll as BUILTIN
// honest-stub modules. Wired by the host boot (main.cpp) alongside the
// other trt_bridge_register_* calls.
int trt_bridge_register_nvapi(void);

#ifdef __cplusplus
}
#endif

#endif // TIRAMISU_BRIDGE_NVAPI_H
