// tiramisu/src/trt/bridge_shlwapi.h
// Path utilities for the guest (bridge_shlwapi.cpp).
//
// shlwapi is pure string/file-path manipulation — no console surface
// exists because none is needed. Games call PathFileExistsW /
// PathFindFileNameW / PathCombineW constantly at boot to assemble asset
// and config paths. File-touching calls resolve through the VFS.
#ifndef TIRAMISU_BRIDGE_SHLWAPI_H
#define TIRAMISU_BRIDGE_SHLWAPI_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register shlwapi.dll as BUILTIN.
int trt_bridge_register_shlwapi(void);

#ifdef __cplusplus
}
#endif

#endif
