// tiramisu/src/trt/bridge_d3dx9.h
// D3DX9 for the guest (M6 coverage batch).
//
// d3dx9_43.dll is the D3DX utility library: matrix/vector math, texture
// loading from file/memory, sprite batching, .x mesh loading. Hundreds of
// games import it, and unlike d3d9 they usually do NOT ship the DLL
// (it was a redist). When it's missing, load fails before main().
//
// This bridge implements the functions games actually import (by import-
// count in the wild): the D3DXMatrix* family, D3DXVector math,
// D3DXCreateTexture*/D3DXLoadSurfaceFromMemory (via the d3d9 hooks),
// D3DXSprite (ID3DXSprite, SDK-exact vtable), D3DXAssembleShader/
// D3DXCompileShader (passthrough of source text; the console's real
// d3dcompiler_47 handles HLSL — d3dx9 only assembled it).
//
// Math functions are pure computation and exact. Texture/sprite functions
// are the marshalling layer. Everything else fails visibly (named stub).
#ifndef TIRAMISU_BRIDGE_D3DX9_H
#define TIRAMISU_BRIDGE_D3DX9_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

int trt_bridge_register_d3dx9(void);

#ifdef __cplusplus
}
#endif

#endif
