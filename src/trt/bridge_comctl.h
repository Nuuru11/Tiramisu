// tiramisu/src/trt/bridge_comctl.h
// COMCTL32 for the guest (bridge_comctl.cpp).
//
// The common-controls acceptance surface. On the console there are no
// common controls to draw, but a game's InitCommonControls(Ex) at boot is
// an ACCEPTANCE call, not a rendering call: it must return success so the
// game continues, and the requested class flags are logged (they tell us
// which controls the game would use — roadmap for any real surface).
//
// OrTMM imports COMCTL32 BY ORDINAL: #380 = InitCommonControlsEx on x64.
// The bridge registers the same function under the "#380" ordinal name so
// an ordinal-only import binds to the real bridge function.
#ifndef TIRAMISU_BRIDGE_COMCTL_H
#define TIRAMISU_BRIDGE_COMCTL_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register comctl32.dll as BUILTIN (InitCommonControls/Ex + ordinal #380).
int trt_bridge_register_comctl(void);

#ifdef __cplusplus
}
#endif

#endif
