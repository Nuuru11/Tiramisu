// tiramisu/src/trt/bridge_gdi.h
// GDI32 for the guest (bridge_gdi.cpp).
//
// A small GDI OBJECT + DC handle table: fonts, brushes, regions, bitmaps
// are host-side records; DCs come from user32 GetDC. The console has no
// GDI display surface, but SDL2-class engines and dialog-based UIs still
// call the object/state surface at boot and expect real handles back.
// Nothing here draws; the records only make the lifecycle observable and
// keep the calls from being fail-visible thunks.
#ifndef TIRAMISU_BRIDGE_GDI_H
#define TIRAMISU_BRIDGE_GDI_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register gdi32.dll as BUILTIN (object/state surface).
int trt_bridge_register_gdi(void);

#ifdef __cplusplus
}
#endif

#endif
