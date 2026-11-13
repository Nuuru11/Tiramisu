// tiramisu/src/trt/bridge_crypt32.h
// crypt32 for the guest (bridge_crypt32.cpp).
//
// The console has no certificate store. CertOpenSystemStore/CertOpenStore
// fail (ERROR_INVALID_PARAMETER-class, named log: no cert store on console),
// chain verbs fail honestly, the *Free* verbs succeed (freeing nothing is
// not a lie). UnityPlayer probes the cert surface at boot; an honest FALSE
// sends it down its no-cert path instead of dying at load.
#ifndef TIRAMISU_BRIDGE_CRYPT32_H
#define TIRAMISU_BRIDGE_CRYPT32_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register crypt32.dll as BUILTIN.
int trt_bridge_register_crypt32(void);

#ifdef __cplusplus
}
#endif

#endif
