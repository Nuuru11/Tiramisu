// tiramisu/src/trt/bridge_d3d9.h
// The D3D9 bridge (M5): a PC game's Direct3D 9 calls end up on the host's
// D3D11 device + CoreWindow swapchain.
//
// Why a bridge (not a passthrough): UWP has no d3d9.dll at all, and a D3D9
// game's fake HWND cannot host a swapchain on console. This bridge
// implements the D3D9 COM surface (SDK-exact vtables) and marshals the
// GPU work through plain-C hooks (d3d9_hooks) that the console host binds
// to its real D3D11 device. The offline test binds counting hooks.
//
// Semantics reference: Microsoft's D3D9On12 (MIT) maps D3D9 state onto a
// modern device; we follow the same mapping (FVF -> decl, D3DRS blend/z,
// texture stage 0 = the only sampler most 2D engines use).
#ifndef TIRAMISU_BRIDGE_D3D9_H
#define TIRAMISU_BRIDGE_D3D9_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// opaque GPU object handles (the host backend owns the real objects)
typedef u32 d3d9_handle;
#define D3D9_HANDLE_NULL 0

typedef struct d3d9_viewport {
  u32 x, y, w, h;
  float z_min, z_max;
} d3d9_viewport;

typedef struct d3d9_hooks {
  u32 width, height;  // the host's current swapchain size (display modes)
  // resources: texture/surface (tex), vertex/index buffer (buf)
  d3d9_handle (*create_tex)(u32 w, u32 h, u32 levels, u32 usage, u32 fmt,
                            void* user);
  d3d9_handle (*create_buf)(u32 bytes, u32 is_index, void* user);
  void* (*lock)(d3d9_handle h, u32 level, u32 offset, u32 bytes, void* user);
  void (*unlock)(d3d9_handle h, void* user);
  void (*release)(d3d9_handle h, void* user);

  // frame
  void (*clear)(u32 flags, u32 color, float z, u32 stencil, void* user);
  void (*present)(void* user);

  // render state
  void (*set_viewport)(const d3d9_viewport* vp, void* user);
  void (*set_texture)(u32 stage, d3d9_handle h, void* user);
  void (*set_transform)(u32 kind, const float* m4x4, void* user);
  void (*set_fvf)(u32 fvf, void* user);
  void (*set_rs)(u32 state, u32 value, void* user);  // D3DRS_*
  void (*set_sampler)(u32 stage, u32 state, u32 value, void* user);

  // draws (vb/ib handles; UP variants pass data through lock)
  void (*draw)(u32 prim_type, d3d9_handle vb, d3d9_handle ib, u32 base_index,
               u32 min_index, u32 num_vertices, u32 start_index,
               u32 prim_count, void* user);
  void* user;
} d3d9_hooks;

void trt_d3d9_set_hooks(const d3d9_hooks* hooks);
const d3d9_hooks* trt_d3d9_hooks_get(void);

// Register d3d9.dll / d3d9core.dll / d3dx9_43.dll as BUILTIN.
int trt_bridge_register_d3d9(void);

// self-test observation (offline): last primitive type + count seen
u32 trt_d3d9_last_draw(void);

#ifdef __cplusplus
}
#endif

#endif
