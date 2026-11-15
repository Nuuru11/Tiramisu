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
  // Optional Wave 2A host hooks (null in offline tests):
  void* factory6;        // IDXGIFactory6* (if host DXGI >= 1.6)
  void* swapchain3;      // IDXGISwapChain3* for active D3D11 CoreWindow swapchain
  void* d3d12_device;    // ID3D12Device*
  void* d3d12_swapchain; // IDXGISwapChain3* bound to active D3D12 command queue
  int (TRT_GUEST_ABI *acquire_d3d12_swapchain)(void* queue, u32 width, u32 height,
                                               u32 fmt, u32 count,
                                               void** out_swapchain);
  void (*restore_d3d11_swapchain)(void);
  int (TRT_GUEST_ABI *d3d11on12_create)(void* device, u32 flags,
                                          const void* levels, u32 level_count,
                                          void* const* queues, u32 num_queues,
                                          u32 node_mask, void** out_device,
                                          void** out_ctx, u32* out_level);
} trt_gpu_hooks;

void trt_gpu_set_hooks(const trt_gpu_hooks* hooks);
const trt_gpu_hooks* trt_gpu_hooks_get(void);

// Register d3d11.dll / dxgi.dll as BUILTIN with the bridge functions.
int trt_bridge_register_d3d(void);

#ifdef __cplusplus
}
#endif

#endif // TIRAMISU_BRIDGE_D3D_H
