// tiramisu/src/trt/bridge_d3dx.h
// D3DX10/D3DX11 bridge (M7): the D3D10/11-era texture + shader helpers.
//
// What D3DX10/11 actually is in commercial games:
//   D3DX11CreateTextureFromMemory      — load a texture (DDS/PNG/JPG/BMP)
//   D3DX11CreateTextureFromFile        — same, from a file path (UTF-8/UTF-16)
//   D3DX11CreateShaderResourceViewFromMemory/File — texture + view in one call
//   D3DX11GetImageInfoFromMemory/File  — pre-scan dimensions before loading
//   D3DX10* — identical names/shapes one generation earlier
//   D3DX11CompileFromFile + D3DCompile/D3DCompileFromFile — the shader path
//     (Unity/Unreal-era games compile HLSL at boot or ship compiled blobs)
//
// The device the guest passes in is the HOST's real ID3D11Device (the
// D3D11 bridge hands it out), so this bridge calls CreateTexture2D /
// CreateShaderResourceView THROUGH the device's own vtable — SDK-exact
// slot numbers from the Windows SDK's d3d11.h (vtable verified below).
//
// Containers: DDS (full support: uncompressed + DXT1/3/5 via bridge_img)
// and PNG via bridge_img. BMP/TGA parsed inline (the d3dx9 parsers are
// d3dx9-local). JPG fails named.
//
// Fail-visible: unknown containers and unsupported formats return a named
// HRESULT and log the reason — never a silent NULL.
#ifndef TIRAMISU_BRIDGE_D3DX_H
#define TIRAMISU_BRIDGE_D3DX_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

int trt_bridge_register_d3dx(void);

// the host hands its real D3DCompile here after boot (the console's
// d3dcompiler_47); guests calling D3DCompile directly go through the
// SYSTEM route and never need this
void trt_d3dx_set_compiler(void* fn);

#ifdef __cplusplus
}
#endif

#endif
