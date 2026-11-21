// tiramisu/src/trt/fna3d_host.h
// Host-side handoff of Tiramisu's D3D11 device + CoreWindow swapchain to
// FNA3D (Path B step 3, Celeste on Xbox).
//
// The console has no OpenGL, so a managed game (Celeste under embedded
// Mono + FNA) must render through FNA3D's D3D11 driver -- but FNA3D must
// NOT create its own device or swapchain: there is only one CoreWindow
// and one flip-model swapchain on the console, owned by the host
// (src/host/swapchain.cpp). This module hands the host's already-created
// ID3D11Device + ID3D11DeviceContext + IDXGISwapChain to FNA3D before the
// managed game starts, so the game's stock GraphicsDevice creation adopts
// them (see FNA3D_D3D11_SetHostDevice in third_party/fna3d-src).
//
// OWNERSHIP (the contract that matters):
//   - The HOST owns the device, context and swapchain, always. FNA3D
//     AddRefs them at adopt (device creation) and Releases them at
//     destroy (GraphicsDevice.Dispose -> FNA3D_DestroyDevice); the
//     host's own references keep the objects alive, so FNA3D never
//     destroys the host device or swapchain.
//   - The host must keep the objects alive from trt_fna3d_attach until
//     trt_fna3d_detach (or game exit), and must NOT resize, recreate or
//     destroy the swapchain while a managed game runs: FNA3D's render-
//     target view tracks the back buffer it was created from, and a
//     host resize mid-game would orphan it (FNA3D refreshes the RTV if
//     it notices a size change, but that is a safety net, not a path).
//   - While a managed game runs, the host UI loop must NOT Present on
//     the same swapchain: the game's FNA3D_SwapBuffers IS the present.
//     (The host's pointer overlay is drawn by swapchain_present, which
//     must stay out of the way for the same reason.)
//
// PLATFORM: real on Windows/UWP only. On Linux there is no D3D11, so
// trt_fna3d_attach returns TRT_ERR_FNA3D with a clear message -- it does
// NOT fake success. The Linux harness keeps rendering via FNA3D's
// OpenGL driver (llvmpipe), unchanged.
//
// CONSOLE-VERIFICATION-PENDING: runtime D3D11 rendering can only be
// verified on the console. This module + the driver adaptation are
// compile-verified only (tools/build-host.sh rc=0).
#ifndef TIRAMISU_FNA3D_HOST_H
#define TIRAMISU_FNA3D_HOST_H

#include "pe.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct trt_fna3d_attach_config {
  void* device;     // ID3D11Device* (required; host-owned)
  void* context;    // ID3D11DeviceContext* (NULL = derive the device's
                    // immediate context inside FNA3D)
  void* swapchain;  // IDXGISwapChain* (required; host-owned)
  void (*log)(const char* line);  // NULL = stderr with a "fna3d: " prefix.
                                  // When set, FNA3D's own log output is
                                  // also routed here (installed once, for
                                  // the process lifetime; the callback
                                  // must stay valid).
} trt_fna3d_attach_config;

// Pre-register the host device/swapchain with FNA3D's D3D11 driver. The
// next FNA3D_CreateDevice (the managed game's stock GraphicsDevice
// creation) adopts them instead of creating its own; the registration
// is one-shot and consumed by that creation. Returns TRT_OK, or
// TRT_ERR_FNA3D on failure (detail in trt_fna3d_last_error()). Fails if
// already attached: one handoff per game run; detach first.
int trt_fna3d_attach(const trt_fna3d_attach_config* cfg);

// Clear a pending host-device registration (no-op if already consumed
// by a device creation or never attached). Does NOT destroy the host
// device/swapchain, and does NOT release FNA3D's adopted references --
// those are released by FNA3D_DestroyDevice when the game disposes its
// GraphicsDevice. Call at game exit. Safe to call when not attached.
void trt_fna3d_detach(void);

// Attach using the GPU hooks published by the host
// (trt_gpu_hooks_get: device/context/swapchain from swapchain.cpp).
// Returns TRT_OK, or TRT_ERR_FNA3D if no hooks are published or they
// lack a device/swapchain. `log` is the attach log callback (NULL =
// stderr default).
int trt_fna3d_attach_from_hooks(void (*log)(const char* line));

// 1 if a handoff is currently attached (registered, not yet detached),
// 0 otherwise.
int trt_fna3d_is_attached(void);

// Human-readable detail of the last handoff failure ("" when none).
const char* trt_fna3d_last_error(void);

// Current app memory usage in bytes (MemoryManager::AppMemoryUsage on
// UWP; 0 on Linux). Memory instrumentation for the Celeste OOM
// investigation (oom-analysis.md): the FNA3D D3D11 driver logs this
// after each large texture upload (patch 08). The driver is C and
// cannot call WinRT directly, so the host provides it. Returns 0 if
// the query fails.
u64 trt_fna3d_app_memory_usage(void);

#ifdef __cplusplus
}
#endif

#endif  // TIRAMISU_FNA3D_HOST_H
