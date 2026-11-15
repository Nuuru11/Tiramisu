// tiramisu/src/trt/bridge_mf.h
// Media Foundation (mfplat, mfreadwrite, mf, mfuuid, evr, dxva2) COM bridge
// for video cutscenes and audio decoding on Xbox UWP / offline harness.
#ifndef TIRAMISU_BRIDGE_MF_H
#define TIRAMISU_BRIDGE_MF_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

int trt_bridge_register_mf(void);

#ifdef __cplusplus
}
#endif

#endif  // TIRAMISU_BRIDGE_MF_H
