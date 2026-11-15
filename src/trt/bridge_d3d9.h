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

  // optional UP-draw + stream stride extensions (zero in offline test hooks;
  // populated by the host backend so DrawPrimitiveUP / DDraw upload render)
  void (*set_stream_stride)(u32 stream, u32 stride, void* user);
  void (*draw_up)(u32 prim_type, const void* verts, u32 vstride, u32 num_verts,
                  const void* indices, u32 index_fmt32, u32 index_count,
                  u32 prim_count, void* user);

  // Wave 2B extensions (always zero-init in older test harnesses):
  // Shaders (MojoShader HLSL + parsed MOJOSHADER_parseData*), VertexDecls,
  // MRT/DepthStencil targets, Cube/Volume/RT/DS textures, subresource upload,
  // scissor rect, CopyRect/ColorFill, and 32-bit vs 16-bit index format.
  u32 (*create_shader)(u32 is_ps, u32 maj, u32 min, const char* hlsl,
                       u32 hlsl_len, const void* parsed, void* user);
  void (*set_shader)(u32 is_ps, u32 handle, void* user);
  void (*set_shader_const_f)(u32 is_ps, u32 start_reg, const float* data,
                             u32 vec4_count, void* user);
  void (*set_shader_const_i)(u32 is_ps, u32 start_reg, const i32* data,
                             u32 vec4_count, void* user);
  void (*set_shader_const_b)(u32 is_ps, u32 start_reg, const i32* data,
                             u32 bool_count, void* user);
  u32 (*create_decl)(const void* elems_d3d9, u32 count, void* user);
  void (*set_decl)(u32 handle, void* user);
  u32 (*create_tex_ex)(u32 kind, u32 w, u32 h, u32 depth_or_faces,
                       u32 mip_levels, u32 usage, u32 fmt, void* user);
  void (*unlock_sub)(u32 handle, u32 face_or_slice, u32 level, u32 row_pitch,
                     u32 slice_pitch, const void* bits, u32 bytes, void* user);
  void (*set_render_target)(u32 slot, u32 handle, void* user);
  void (*set_depth_stencil)(u32 handle, void* user);
  void (*set_scissor)(int enabled, int left, int top, int right, int bottom,
                      void* user);
  void (*copy_rect)(u32 dst_handle, u32 dst_sub, int dx, int dy, u32 src_handle,
                    u32 src_sub, int sx, int sy, int w, int h, void* user);
  void (*fill_rect)(u32 handle, u32 sub, int x, int y, int w, int h, u32 color,
                    void* user);
  void (*set_index_fmt)(u32 handle, u32 is_index32, void* user);
} d3d9_hooks;

typedef struct d3d9_vert_elem {
  u16 stream;
  u16 offset;
  u8 type;
  u8 method;
  u8 usage;
  u8 usage_index;
} d3d9_vert_elem;

// Free-function MojoShader bytecode scanner & HLSL translator helper exposed
// by bridge_d3d9 so d3d8/tests can inspect translated shaders directly.
const void* trt_d3d9_mojo_parse(const u32* bytecode, u32 byte_len,
                                u32* out_byte_len);
void trt_d3d9_mojo_free(const void* parsed);

void trt_d3d9_set_hooks(const d3d9_hooks* hooks);
const d3d9_hooks* trt_d3d9_hooks_get(void);

// Register d3d9.dll / d3d9core.dll / d3dx9_43.dll as BUILTIN.
int trt_bridge_register_d3d9(void);

// self-test observation (offline): last primitive type + count seen
u32 trt_d3d9_last_draw(void);

#ifdef __cplusplus
}
#endif

static inline i32 trt_d3d9_row_pitch(u32 w, u32 fmt) {
  if (!w) return 4096;
  switch (fmt) {
    case 28:  // A8
    case 50:  // L8
      return (i32)w;
    case 23:  // R5G6B5
    case 24:  // X1R5G5B5
    case 25:  // A1R5G5B5
    case 26:  // A4R4G4B4
    case 51:  // A8L8
      return (i32)(w * 2);
    case 20:  // R8G8B8
      return (i32)(w * 3);
    case 0x31545844u:  // DXT1
      return (i32)(((w + 3) / 4) * 8);
    case 0x33545844u:  // DXT3
    case 0x35545844u:  // DXT5
      return (i32)(((w + 3) / 4) * 16);
    default:
      return (i32)(w * 4);
  }
}

static inline u32 trt_d3d9_prim_verts(u32 prim, u32 count) {
  switch (prim) {
    case 1: return count;          // POINTLIST
    case 2: return count * 2;      // LINELIST
    case 3: return count + 1;      // LINESTRIP
    case 4: return count * 3;      // TRIANGLELIST
    case 5: return count + 2;      // TRIANGLESTRIP
    case 6: return count + 2;      // TRIANGLEFAN
    default: return count * 3;
  }
}

#endif
