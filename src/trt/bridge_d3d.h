// tiramisu/src/trt/bridge_d3d.h
// The graphics bridge: a PC game's D3D11/DXGI calls have to end up on the
// host's CoreWindow swapchain.
//
// The console has no HWND, so a game's own CreateSwapChainForHwnd(fakeHWND)
// cannot work, and UWP has no D3D9 at all. The bridge therefore:
//   - hands the guest the HOST's ID3D11Device/ID3D11DeviceContext, so the
//     guest's resources live in the same device as the host's swapchain;
//   - hands the guest a factory whose CreateSwapChain* returns the HOST's
//     swapchain (the guest then renders into the host's backbuffer);
//   - forwards everything else (adapter enumeration, window association)
//     to the console's real DXGI factory.
//
// The real COM objects are passed through unchanged, so the guest's calls
// on them need no shim at all - only the factory needs one.
#ifndef TIRAMISU_BRIDGE_D3D_H
#define TIRAMISU_BRIDGE_D3D_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// What the host provides (real COM pointers).
typedef struct trt_gpu_hooks {
  void* device;      // ID3D11Device*
  void* context;     // ID3D11DeviceContext*
  void* swapchain;   // IDXGISwapChain1* (over the CoreWindow)
  void* factory;     // IDXGIFactory2* (the console's real one)
  unsigned feature_level;
  unsigned width, height;
} trt_gpu_hooks;

void trt_gpu_set_hooks(const trt_gpu_hooks* hooks);
const trt_gpu_hooks* trt_gpu_hooks_get(void);

// Register d3d11.dll / dxgi.dll as BUILTIN with the bridge functions.
int trt_bridge_register_d3d(void);

#ifdef __cplusplus
}
#endif

#endif // TIRAMISU_BRIDGE_D3D_H
