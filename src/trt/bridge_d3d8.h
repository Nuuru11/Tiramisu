// tiramisu/src/trt/bridge_d3d8.h
// The D3D8 bridge (M7): the 2001-2004 commercial generation (Morrowind,
// BF1942, NWN, MAX Payne) talks IDirect3D8/IDirect3DDevice8.
//
// Same shape as bridge_d3d9: SDK-exact vtables, the same plain-C backend
// hooks (d3d9_hooks — the host binds ONE set of GPU hooks), D3D8
// signatures where they differ:
//   - shaders are DWORD handles, not COM objects (D3D9 made them objects)
//   - DrawIndexedPrimitive has no BaseVertexIndex parameter
//   - SetStreamSource has no offset parameter
//   - D3D8-only: CopyRects, CreateImageSurface (D3D9 replaced both with
//     UpdateSurface/StretchRect)
//   - D3DCAPS8 layout (d3d8caps.h) differs from D3DCAPS9
#ifndef TIRAMISU_BRIDGE_D3D8_H
#define TIRAMISU_BRIDGE_D3D8_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register d3d8.dll as BUILTIN.
int trt_bridge_register_d3d8(void);

#ifdef __cplusplus
}
#endif

#endif
