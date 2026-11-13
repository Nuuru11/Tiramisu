// tiramisu/src/trt/bridge_steam.h
// Steamworks auth bridge for the guest (bridge_steam.cpp).
// AUTH ONLY: no DRM bypass, ever. A DRM'd build refuses visibly.
#ifndef TIRAMISU_BRIDGE_STEAM_H
#define TIRAMISU_BRIDGE_STEAM_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

int trt_bridge_register_steam(void);

#ifdef __cplusplus
}
#endif

#endif
