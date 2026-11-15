// tiramisu/src/trt/bridge_sys_extras.h
// System telemetry, power, WER, user profile, session, and property-store
// bridges (powrprof, pdh, wer, propsys, userenv, wtsapi32) for real engines.
#ifndef TIRAMISU_BRIDGE_SYS_EXTRAS_H
#define TIRAMISU_BRIDGE_SYS_EXTRAS_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

int trt_bridge_register_sys_extras(void);

#ifdef __cplusplus
}
#endif

#endif  // TIRAMISU_BRIDGE_SYS_EXTRAS_H
