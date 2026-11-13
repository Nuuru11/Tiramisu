// tiramisu/src/trt/bridge_ddraw.h
// DirectDraw for the guest (M6 coverage batch).
//
// DirectDraw is how every 2D/retro PC game (and countless early-3D ones)
// draws: create surfaces, lock, write pixels, flip/present. UWP has no
// ddraw.dll, so this bridge implements the COM surface (SDK-exact vtables)
// and marshals the presentation through the d3d9 host hooks
// (bridge_d3d9.h): a Lock/Unlock on the primary surface hands the guest
// the real pixel store, and Flip/Unlock maps to the backend's present,
// the DirectDraw analog of a swapchain Present.
#ifndef TIRAMISU_BRIDGE_DDRAW_H
#define TIRAMISU_BRIDGE_DDRAW_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

int trt_bridge_register_ddraw(void);

#ifdef __cplusplus
}
#endif

#endif
