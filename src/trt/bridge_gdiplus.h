// tiramisu/src/trt/bridge_gdiplus.h
// gdiplus.dll bridge — GDI+ 2D flat API for launcher-class exes (BotW-Native
// launcher/setup wizards). Object lifecycle real, draw sites recorded.
#ifndef TIRAMISU_BRIDGE_GDIPLUS_H
#define TIRAMISU_BRIDGE_GDIPLUS_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register gdiplus.dll as BUILTIN (launcher UI surface, recorded draws).
int trt_bridge_register_gdiplus(void);

#ifdef __cplusplus
}
#endif

#endif  // TIRAMISU_BRIDGE_GDIPLUS_H
