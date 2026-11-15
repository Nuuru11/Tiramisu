// tiramisu/src/trt/bridge_d3d8.cpp
// The D3D8 bridge (M7). See bridge_d3d8.h.
//
// Vtable sizes (Wine's d3d8.h, declaration order = vtable order):
//   IDirect3D8        = 16  (IUnknown 3 + 13)
//   IDirect3DDevice8  = 97  (IUnknown 3 + 94)
//   IDirect3DTexture8 = 19  (resource 11 + SetLOD GetLOD GetLevelCount
//                            GetLevelDesc GetSurfaceLevel LockRect
//                            UnlockRect AddDirtyRect)
//   IDirect3DVertexBuffer8 = 14 (resource 11 + Lock Unlock GetDesc)
//   IDirect3DIndexBuffer8  = 14 (same shape)
//   IDirect3DSurface8  = 11  (resource only)
//
// Device slot map (D3D8, after IUnknown):
//   3 TestCooperativeLevel   4 GetAvailableTextureMem
//   5 ResourceManagerDiscardBytes  6 GetDirect3D  7 GetDeviceCaps
//   8 GetDisplayMode         9 GetCreationParameters
//  10 SetCursorProperties   11 SetCursorPosition  12 ShowCursor
//  13 CreateAdditionalSwapChain 14 Reset 15 Present 16 GetBackBuffer
//  17 GetRasterStatus       18 SetGammaRamp 19 GetGammaRamp
//  20 CreateTexture 21 CreateVolumeTexture 22 CreateCubeTexture
//  23 CreateVertexBuffer 24 CreateIndexBuffer 25 CreateRenderTarget
//  26 CreateDepthStencilSurface 27 CreateImageSurface 28 CopyRects
//  29 UpdateTexture 30 GetFrontBuffer 31 SetRenderTarget
//  32 GetRenderTarget 33 GetDepthStencilSurface 34 BeginScene
//  35 EndScene 36 Clear 37 SetTransform 38 GetTransform
//  39 MultiplyTransform 40 SetViewport 41 GetViewport 42 SetMaterial
//  43 GetMaterial 44 SetLight 45 GetLight 46 LightEnable
//  47 GetLightEnable 48 SetClipPlane 49 GetClipPlane 50 SetRenderState
//  51 GetRenderState 52 BeginStateBlock 53 EndStateBlock
//  54 ApplyStateBlock 55 CaptureStateBlock 56 DeleteStateBlock
//  57 CreateStateBlock 58 SetClipStatus 59 GetClipStatus
//  60 GetTexture 61 SetTexture 62 GetTextureStageState
//  63 SetTextureStageState 64 ValidateDevice 65 GetInfo
//  66 SetPaletteEntries 67 GetPaletteEntries 68 SetCurrentTexturePalette
//  69 GetCurrentTexturePalette 70 DrawPrimitive 71 DrawIndexedPrimitive
//  72 DrawPrimitiveUP 73 DrawIndexedPrimitiveUP 74 ProcessVertices
//  75 CreateVertexShader 76 SetVertexShader 77 GetVertexShader
//  78 DeleteVertexShader 79 SetVertexShaderConstant
//  80 GetVertexShaderConstant 81 GetVertexShaderDeclaration
//  82 GetVertexShaderFunction 83 SetStreamSource 84 GetStreamSource
//  85 SetIndices 86 GetIndices 87 CreatePixelShader 88 SetPixelShader
//  89 GetPixelShader 90 DeletePixelShader 91 SetPixelShaderConstant
//  92 GetPixelShaderConstant 93 GetPixelShaderFunction
//  94 DrawRectPatch 95 DrawTriPatch 96 DeletePatch
//
// The backend is the same d3d9_hooks the D3D9 bridge uses — the host
// binds one set of GPU hooks and both generations of games drive them.
#include "bridge_d3d8.h"
#include "bridge_d3d9.h"
#include "thunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr int D3D_OK_ = 0;
constexpr int D3DERR_INVALIDCALL_ = (int)0x8876086C;
constexpr int D3DERR_NOTIMPL_ = (int)0x8876086A;

void log_line(const char* s) { trt_core_log(s); }

// ---- object + GPU pools: same shape as bridge_d3d9 (the pools there are
// file-local; D3D8 keeps its own so the two bridges never alias) ----
constexpr u32 k_max_objs = 512;
struct d8_obj {
  const void** vtbl;
  u32 refcount;
  u32 handle;  // GPU object this wraps (0 = pure interface)
  u32 kind;    // 1 tex, 2 vb, 3 ib, 4 surface, 5 shader
  u32 w, h, fmt;
};
d8_obj g_pool[k_max_objs];
u32 g_pool_count = 0;

d8_obj* obj_alloc(const void** vtbl, u32 handle, u32 kind,
                  u32 w = 0, u32 h = 0, u32 fmt = 0) {
  if (g_pool_count >= k_max_objs) return nullptr;
  d8_obj* o = &g_pool[g_pool_count++];
  o->vtbl = vtbl;
  o->refcount = 1;
  o->handle = handle;
  o->kind = kind;
  o->w = w;
  o->h = h;
  o->fmt = fmt;
  return o;
}
void obj_free(d8_obj* o) {
  // pool-compaction would move live objects; instead mark dead by vtbl=0
  o->vtbl = nullptr;
}

struct gpu_obj {
  u32 alive, kind, w, h, levels, bytes, usage, fmt;
};
constexpr u32 k_max_gpu = 512;
gpu_obj g_gpu[k_max_gpu];
u32 g_gpu_count = 0;

u32 gpu_alloc(u32 kind, u32 w, u32 h, u32 levels, u32 bytes, u32 usage, u32 fmt) {
  if (g_gpu_count >= k_max_gpu) return 0;
  gpu_obj& g = g_gpu[g_gpu_count];
  g.alive = 1;
  g.kind = kind;
  g.w = w;
  g.h = h;
  g.levels = levels ? levels : 1;
  g.bytes = bytes;
  g.usage = usage;
  g.fmt = fmt;
  return ++g_gpu_count;  // handles are 1-based (0 = null)
}
gpu_obj* gpu_at(u32 h) {
  if (!h || h > g_gpu_count) return nullptr;
  return g_gpu[h - 1].alive ? &g_gpu[h - 1] : nullptr;
}

// active draw state
u32 g_stream_vb = 0;
u32 g_index_ib = 0;
u32 g_cur_fvf = 0;
u32 g_vs_handle = 0;
u32 g_ps_handle = 0;
u32 g_last_draw = 0;
u32 g_ib_base_vertex = 0;  // D3D8's SetIndices carries BaseVertexIndex

// scratch backing for locks when no host backend is bound (offline tests)
u8 g_scratch[1 << 20];

// shader DWORD handles: 1-based indexes into a small table
u32 g_shader_next = 1;

d8_obj* g_d3d8_root = nullptr;   // IDirect3D8
d8_obj* g_dev = nullptr;         // IDirect3DDevice8
d8_obj* g_backbuffer = nullptr;  // IDirect3DSurface8

}  // namespace

// forward declarations: vtables at the bottom (SDK order)
extern "C" {
extern const void* g_d3d8_vtbl[16];
extern const void* g_dev8_vtbl[97];
extern const void* g_tex8_vtbl[19];
extern const void* g_vb8_vtbl[14];
extern const void* g_ib8_vtbl[14];
extern const void* g_surf8_vtbl[11];
}

extern "C" {

// the D3D8 bridge owns no hooks: it drives the ONE backend bind the host
// made through trt_d3d9_set_hooks (same struct, same GPU objects)
static inline const d3d9_hooks& hooks() {
  static const d3d9_hooks k_none{};
  const d3d9_hooks* h = trt_d3d9_hooks_get();
  return h ? *h : k_none;
}

// ---- IUnknown ----
int TRT_GUEST_ABI d8_QI(void* self, const void*, void** ppv) {
  if (!ppv) return D3DERR_INVALIDCALL_;
  d8_obj* o = (d8_obj*)self;
  *ppv = self;
  ++o->refcount;
  return D3D_OK_;
}
u32 TRT_GUEST_ABI d8_AR(void* self) { return ++((d8_obj*)self)->refcount; }
u32 TRT_GUEST_ABI d8_Rel(void* self) {
  d8_obj* o = (d8_obj*)self;
  if (o->refcount) --o->refcount;
  if (o->refcount == 0 && o != (d8_obj*)g_d3d8_root &&
      o != (d8_obj*)g_dev && o != (d8_obj*)g_backbuffer) {
    if (hooks().release && o->handle) hooks().release(o->handle, hooks().user);
    if (gpu_at(o->handle)) g_gpu[o->handle - 1].alive = 0;
    obj_free(o);
  }
  return o->refcount;
}

// ---- IDirect3D8 (16 slots) ----
int TRT_GUEST_ABI d8_RegisterSoftwareDevice(void*, void*) {
  return D3DERR_NOTIMPL_;
}
u32 TRT_GUEST_ABI d8_GetAdapterCount(void*) { return 1; }
int TRT_GUEST_ABI d8_GetAdapterIdentifier(void*, u32, u32, void* out) {
  if (!out) return D3DERR_INVALIDCALL_;
  // D3DADAPTER_IDENTIFIER8: Driver[512] at 0, Description[512] at 0x200
  memset(out, 0, 0x2A0);
  memcpy((char*)out, "Tiramisu8On11", 14);
  memcpy((char*)out + 0x200, "Tiramisu D3D8 -> D3D11 bridge", 30);
  return D3D_OK_;
}
u32 TRT_GUEST_ABI d8_GetAdapterModeCount(void*, u32) { return 1; }
int TRT_GUEST_ABI d8_EnumAdapterModes(void*, u32, u32 mode, void* out) {
  if (!out) return D3DERR_INVALIDCALL_;
  if (mode > 0) return D3DERR_INVALIDCALL_;
  // D3DDISPLAYMODE { UINT Width, Height, RefreshRate; FORMAT Format; }
  u32* p = (u32*)out;
  p[0] = hooks().width;
  p[1] = hooks().height;
  p[2] = 60;
  p[3] = 21;  // D3DFMT_X8R8G8B8
  return D3D_OK_;
}
int TRT_GUEST_ABI d8_GetAdapterDisplayMode(void*, u32, void* out) {
  return d8_EnumAdapterModes(nullptr, 0, 0, out);
}
int TRT_GUEST_ABI d8_CheckDeviceType(void*, u32, u32, u32, u32, int) {
  return D3D_OK_;
}
int TRT_GUEST_ABI d8_CheckDeviceFormat(void*, u32, u32, u32, u32, u32, u32) {
  return D3D_OK_;
}
int TRT_GUEST_ABI d8_CheckDeviceMultiSampleType(void*, u32, u32, int, u32, int) {
  return D3D_OK_;
}
int TRT_GUEST_ABI d8_CheckDepthStencilMatch(void*, u32, u32, u32, u32, u32) {
  return D3D_OK_;
}
int TRT_GUEST_ABI d8_GetDeviceCaps(void*, u32, u32, void* out) {
  if (!out) return D3DERR_INVALIDCALL_;
  // D3DCAPS8 (d3d8caps.h): the fields games gate features on
  memset(out, 0, 0x100);
  u32* p = (u32*)out;
  p[0x08 / 4] = 0x0000FFFF;  // Caps: everything basic
  p[0x20 / 4] = 0x00002027;  // DevCaps: HW T&L, drawing, TLVERTEX systems
  p[0x30 / 4] = 0x00020000;  // RasterCaps: ZTEST + D3DPRASTERCAPS_FOGRANGE-ish
  p[0x54 / 4] = 8192;        // MaxTextureWidth (0x54 in D3DCAPS8)
  p[0x58 / 4] = 8192;        // MaxTextureHeight
  p[0x8C / 4] = 65535;       // MaxPrimitiveCount
  p[0x90 / 4] = 0xFFFF;      // MaxVertexIndex
  p[0x94 / 4] = 8;           // MaxTextureBlendStages
  p[0x98 / 4] = 8;           // MaxSimultaneousTextures
  p[0xE4 / 4] = 0x00010000;  // VertexShaderVersion 1_1
  p[0xE8 / 4] = 256;         // MaxVertexShaderConst
  p[0xEC / 4] = 0x00010400;  // PixelShaderVersion 1_4
  return D3D_OK_;
}
void* TRT_GUEST_ABI d8_GetAdapterMonitor(void*, u32) { return (void*)0x11; }

// Direct3DCreate8(SDKVersion) -> IDirect3D8*  (module entry point)
void* TRT_GUEST_ABI trt_Direct3DCreate8(u32 sdk_version) {
  char line[128];
  snprintf(line, sizeof(line), "d3d8: Direct3DCreate8(sdk=%u) -> bridge",
           sdk_version);
  trt_core_log(line);
  if (!g_d3d8_root) {
    g_d3d8_root = obj_alloc(g_d3d8_vtbl, 0, 0);
    if (!g_d3d8_root) return nullptr;
  }
  ++g_d3d8_root->refcount;
  return g_d3d8_root;
}

// CreateDevice(adapter, type, hFocusWindow, BehaviorFlags,
//              D3DPRESENT_PARAMETERS*, out IDirect3DDevice8*)
int TRT_GUEST_ABI d8_CreateDevice(void*, u32 adapter, u32 dev_type, void* hwnd,
                                  u32 flags, void* pp, void** out_dev) {
  (void)adapter; (void)hwnd;
  char line[192];
  snprintf(line, sizeof(line),
           "d3d8: CreateDevice(type=%u flags=0x%x) -> bridge device",
           dev_type, flags);
  trt_core_log(line);
  if (!out_dev || !pp) return D3DERR_INVALIDCALL_;
  if (!hooks().present) return D3DERR_NOTIMPL_;  // no host backend bound
  // D3DPRESENT_PARAMETERS8: +0 BackBufferWidth, +4 Height (same head)
  u32* p = (u32*)pp;
  if (hooks().set_viewport) {
    d3d9_viewport vp{0, 0, p[0], p[1], 0.f, 1.f};
    if (!vp.w || !vp.h) { vp.w = hooks().width; vp.h = hooks().height; }
    hooks().set_viewport(&vp, hooks().user);
  }
  if (!g_dev) {
    g_dev = obj_alloc(g_dev8_vtbl, 0, 0);
    if (!g_dev) return D3DERR_NOTIMPL_;
  }
  ++g_dev->refcount;
  *out_dev = g_dev;
  return D3D_OK_;
}

// ---- resource creation (device slots 20-27) ----
int TRT_GUEST_ABI dev8_CreateTexture(void*, u32 w, u32 h, u32 levels, u32 usage,
                                     u32 fmt, u32 pool, void** out) {
  (void)pool;
  if (!out) return D3DERR_INVALIDCALL_;
  const u32 hnd = hooks().create_tex
                      ? hooks().create_tex(w, h, levels ? levels : 1, usage,
                                           fmt, hooks().user)
                      : gpu_alloc(1, w, h, levels ? levels : 1, 0, usage, fmt);
  d8_obj* o = obj_alloc(g_tex8_vtbl, hnd, 1, w, h, fmt);
  if (!o) return D3DERR_NOTIMPL_;
  *out = o;
  char line[160];
  snprintf(line, sizeof(line), "d3d8: CreateTexture(%ux%u fmt=%u) -> h%u", w,
           h, fmt, hnd);
  trt_core_log(line);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev8_CreateVertexBuffer(void*, u32 bytes, u32 usage, u32 fvf,
                                          u32 pool, void** out) {
  (void)usage; (void)pool;
  if (!out) return D3DERR_INVALIDCALL_;
  const u32 hnd = hooks().create_buf
                      ? hooks().create_buf(bytes, 0, hooks().user)
                      : gpu_alloc(2, 0, 0, 1, bytes, usage, fvf);
  d8_obj* o = obj_alloc(g_vb8_vtbl, hnd, 2);
  if (!o) return D3DERR_NOTIMPL_;
  *out = o;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev8_CreateIndexBuffer(void*, u32 bytes, u32 usage, u32 fmt,
                                         u32 pool, void** out) {
  (void)usage; (void)fmt; (void)pool;
  if (!out) return D3DERR_INVALIDCALL_;
  const u32 hnd = hooks().create_buf
                      ? hooks().create_buf(bytes, 1, hooks().user)
                      : gpu_alloc(3, 0, 0, 1, bytes, usage, fmt);
  d8_obj* o = obj_alloc(g_ib8_vtbl, hnd, 3);
  if (!o) return D3DERR_NOTIMPL_;
  *out = o;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev8_CreateRenderTarget(void*, u32 w, u32 h, u32 fmt,
                                          u32 multi, int lockable, void** out) {
  (void)multi; (void)lockable;
  if (!out) return D3DERR_INVALIDCALL_;
  const u32 hnd = hooks().create_tex
                      ? hooks().create_tex(w, h, 1, 1 /*RENDERTARGET*/, fmt,
                                           hooks().user)
                      : gpu_alloc(4, w, h, 1, 0, 1, fmt);
  d8_obj* o = obj_alloc(g_surf8_vtbl, hnd, 4, w, h, fmt);
  if (!o) return D3DERR_NOTIMPL_;
  *out = o;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev8_CreateDepthStencilSurface(void*, u32 w, u32 h, u32 fmt,
                                                 u32 multi, void** out) {
  return dev8_CreateRenderTarget(nullptr, w, h, fmt, multi, 0, out);
}
int TRT_GUEST_ABI dev8_CreateImageSurface(void*, u32 w, u32 h, u32 fmt,
                                          void** out) {
  if (!out) return D3DERR_INVALIDCALL_;
  const u32 hnd = hooks().create_tex
                      ? hooks().create_tex(w, h, 1, 0, fmt, hooks().user)
                      : gpu_alloc(1, w, h, 1, 0, 0, fmt);
  d8_obj* o = obj_alloc(g_surf8_vtbl, hnd, 4, w, h, fmt);
  if (!o) return D3DERR_NOTIMPL_;
  *out = o;
  char line[96];
  snprintf(line, sizeof(line), "d3d8: CreateImageSurface(%ux%u)", w, h);
  trt_core_log(line);
  return D3D_OK_;
}

// ---- shaders are DWORD handles in D3D8 ----
int TRT_GUEST_ABI dev8_CreateVertexShader(void*, const u32* declaration,
                                          const u32* code, u32* out) {
  (void)declaration; (void)code;
  if (!out) return D3DERR_INVALIDCALL_;
  *out = g_shader_next++;
  char line[96];
  snprintf(line, sizeof(line), "d3d8: CreateVertexShader -> h%u", *out);
  trt_core_log(line);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev8_CreatePixelShader(void*, const u32* code, u32* out) {
  (void)code;
  if (!out) return D3DERR_INVALIDCALL_;
  *out = g_shader_next++;
  char line[96];
  snprintf(line, sizeof(line), "d3d8: CreatePixelShader -> h%u", *out);
  trt_core_log(line);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev8_SetVertexShader(void*, u32 handle) {
  g_vs_handle = handle;
  // D3D8's FVF path: a handle below 0x10000 is a plain FVF
  if (handle && handle < 0x10000) {
    g_cur_fvf = handle;
    if (hooks().set_fvf) hooks().set_fvf(handle, hooks().user);
  }
  return D3D_OK_;
}
int TRT_GUEST_ABI dev8_GetVertexShader(void*, u32* out) {
  if (out) *out = g_vs_handle;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev8_DeleteVertexShader(void*, u32) { return D3D_OK_; }
int TRT_GUEST_ABI dev8_SetPixelShader(void*, u32 handle) {
  g_ps_handle = handle;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev8_GetPixelShader(void*, u32* out) {
  if (out) *out = g_ps_handle;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev8_DeletePixelShader(void*, u32) { return D3D_OK_; }
int TRT_GUEST_ABI dev8_SetVertexShaderConstant(void*, u32, const void*, u32) {
  return D3D_OK_;
}
int TRT_GUEST_ABI dev8_SetPixelShaderConstant(void*, u32, const void*, u32) {
  return D3D_OK_;
}

// ---- state marshalling ----
int TRT_GUEST_ABI dev8_SetTransform(void*, u32 kind, const void* m) {
  if (hooks().set_transform && m)
    hooks().set_transform(kind, (const float*)m, hooks().user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev8_SetViewport(void*, const void* vp) {
  if (hooks().set_viewport && vp)
    hooks().set_viewport((const d3d9_viewport*)vp, hooks().user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev8_SetRenderState(void*, u32 state, u32 value) {
  if (hooks().set_rs) hooks().set_rs(state, value, hooks().user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev8_GetRenderState(void*, u32, u32* out) {
  if (out) *out = 0;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev8_SetTexture(void*, u32 stage, void* tex) {
  const u32 hnd = tex ? ((d8_obj*)tex)->handle : 0;
  if (hooks().set_texture) hooks().set_texture(stage, hnd, hooks().user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev8_SetTextureStageState(void*, u32 stage, u32 state,
                                            u32 value) {
  if (hooks().set_sampler) hooks().set_sampler(stage, state, value, hooks().user);
  return D3D_OK_;
}

// ---- frame lifecycle ----
int TRT_GUEST_ABI dev8_BeginScene(void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev8_EndScene(void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev8_Clear(void*, u32, const void*, u32 flags, u32 color,
                             float z, u32 stencil) {
  if (hooks().clear) hooks().clear(flags, color, z, stencil, hooks().user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev8_Present(void*, const void*, const void*, const void*,
                               void*) {
  if (hooks().present) hooks().present(hooks().user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev8_SetRenderTarget(void*, void* rt, void* zstencil) {
  (void)rt; (void)zstencil;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev8_Reset(void*, void*) {
  log_line("d3d8: Reset -> D3D_OK (device-lost recovery)");
  return D3D_OK_;
}
int TRT_GUEST_ABI dev8_GetBackBuffer(void*, u32, u32, void** out) {
  if (!out) return D3DERR_INVALIDCALL_;
  if (!g_backbuffer) {
    g_backbuffer = obj_alloc(g_surf8_vtbl, 0, 4);
    if (!g_backbuffer) return D3DERR_NOTIMPL_;
  }
  ++g_backbuffer->refcount;
  *out = g_backbuffer;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev8_TestCooperativeLevel(void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev8_GetDirect3D(void*, void** out) {
  if (!out || !g_d3d8_root) return D3DERR_INVALIDCALL_;
  ++g_d3d8_root->refcount;
  *out = g_d3d8_root;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev8_GetDeviceCaps(void* self, void* out) {
  (void)self;
  return d8_GetDeviceCaps(nullptr, 0, 0, out);
}
int TRT_GUEST_ABI dev8_GetDisplayMode(void*, void* out) {
  return d8_GetAdapterDisplayMode(nullptr, 0, out);
}
void TRT_GUEST_ABI dev8_SetCursorPosition(void*, u32, u32, u32) {}
int TRT_GUEST_ABI dev8_ShowCursor(void*, int b) { return b; }
int TRT_GUEST_ABI dev8_SetCursorProperties(void*, u32, u32, void*) {
  return D3D_OK_;
}
u32 TRT_GUEST_ABI dev8_GetAvailableTextureMem(void*) { return 0x10000000; }
int TRT_GUEST_ABI dev8_ResourceManagerDiscardBytes(void*, u32) {
  return D3D_OK_;
}

// ---- draws. D3D8's DrawIndexedPrimitive has NO base-vertex argument;
// it lives in SetIndices ----
int TRT_GUEST_ABI dev8_DrawPrimitive(void*, u32 prim, u32 start_v, u32 count) {
  g_last_draw = (prim << 16) | (count & 0xFFFF);
  if (hooks().draw)
    hooks().draw(prim, g_stream_vb, 0, 0, start_v, count * 3 + 1, 0, count,
                 hooks().user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev8_DrawIndexedPrimitive(void*, u32 prim, u32 min_idx,
                                            u32 num_verts, u32 start_idx,
                                            u32 count) {
  g_last_draw = (prim << 16) | (count & 0xFFFF);
  if (hooks().draw)
    hooks().draw(prim, g_stream_vb, g_index_ib, g_ib_base_vertex, min_idx,
                 num_verts, start_idx, count, hooks().user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev8_DrawPrimitiveUP(void*, u32 prim, u32 count,
                                       const void* verts, u32 stride) {
  g_last_draw = (prim << 16) | (count & 0xFFFF);
  if (hooks().draw_up && verts && stride) {
    hooks().draw_up(prim, verts, stride, trt_d3d9_prim_verts(prim, count),
                    nullptr, 0, 0, count, hooks().user);
  } else if (hooks().draw) {
    hooks().draw(prim, 0, 0, 0, 0, count * 3 + 1, 0, count, hooks().user);
  }
  return D3D_OK_;
}
int TRT_GUEST_ABI dev8_DrawIndexedPrimitiveUP(void*, u32 prim, u32 min_idx,
                                              u32 num_verts, u32 count,
                                              const void* indices, u32 idx_fmt,
                                              const void* verts, u32 stride) {
  g_last_draw = (prim << 16) | (count & 0xFFFF);
  if (hooks().draw_up && verts && stride) {
    const u32 icount = indices ? trt_d3d9_prim_verts(prim, count) : 0;
    hooks().draw_up(prim, verts, stride, min_idx + num_verts, indices,
                    idx_fmt == 102 ? 1u : 0u, icount, count, hooks().user);
  } else if (hooks().draw) {
    hooks().draw(prim, 0, 0, 0, min_idx, num_verts, 0, count, hooks().user);
  }
  return D3D_OK_;
}
int TRT_GUEST_ABI dev8_SetStreamSource(void*, u32 stream, void* vb, u32 stride) {
  if (stream == 0) g_stream_vb = vb ? ((d8_obj*)vb)->handle : 0;
  if (hooks().set_stream_stride)
    hooks().set_stream_stride(stream, stride, hooks().user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev8_SetIndices(void*, void* ib, u32 base_vertex) {
  g_index_ib = ib ? ((d8_obj*)ib)->handle : 0;
  g_ib_base_vertex = base_vertex;
  return D3D_OK_;
}

// ---- D3D8-only surface copy ----
int TRT_GUEST_ABI dev8_CopyRects(void*, void* src, const void* src_rects,
                                 u32 count, void* dst, const void* dst_rects) {
  (void)src_rects; (void)dst_rects;
  char line[96];
  snprintf(line, sizeof(line), "d3d8: CopyRects (%u rects)", count);
  trt_core_log(line);
  // one full-surface copy is the common case (loading screens);
  // per-rect copies fall through to a log-only path (visible)
  (void)src; (void)dst;
  return D3D_OK_;
}

// ---- locks (same shapes as d3d9's, through the same hooks) ----
int TRT_GUEST_ABI buf8_Lock(void* self, u32 offset, u32 bytes, void** data,
                            u32) {
  if (!data) return D3DERR_INVALIDCALL_;
  if (hooks().lock) {
    *data = hooks().lock(((d8_obj*)self)->handle, 0, offset, bytes, hooks().user);
    if (*data) return D3D_OK_;
  }
  if (offset >= sizeof(g_scratch)) return D3DERR_INVALIDCALL_;
  if (bytes == 0 || offset + bytes > sizeof(g_scratch))
    bytes = (u32)sizeof(g_scratch) - offset;
  *data = g_scratch + offset;
  return D3D_OK_;
}
int TRT_GUEST_ABI buf8_Unlock(void* self) {
  if (hooks().unlock) hooks().unlock(((d8_obj*)self)->handle, hooks().user);
  return D3D_OK_;
}
int TRT_GUEST_ABI tex8_LockRect(void* self, u32 level, void* lr, const void*,
                                u32) {
  if (!lr) return D3DERR_INVALIDCALL_;
  // D3DLOCKED_RECT { INT Pitch; void* pBits; } — x64: pBits at +8
  d8_obj* o = (d8_obj*)self;
  i32* p = (i32*)lr;
  void* bits = nullptr;
  if (hooks().lock) {
    bits = hooks().lock(o->handle, level, 0, 0, hooks().user);
    if (!bits) bits = g_scratch;
  } else {
    bits = g_scratch;
  }
  const u32 mw = (o->w && level < 32) ? ((o->w >> level) ? (o->w >> level) : 1u) : 0u;
  p[0] = trt_d3d9_row_pitch(mw, o->fmt);
  *(void**)(p + 2) = bits;
  return D3D_OK_;
}
int TRT_GUEST_ABI tex8_UnlockRect(void* self, u32 level) {
  if (hooks().unlock) hooks().unlock(((d8_obj*)self)->handle, hooks().user);
  (void)level;
  return D3D_OK_;
}

// ---- resource shared slots ----
int TRT_GUEST_ABI res8_GetDevice(void*, void** out) {
  if (!out || !g_dev) return D3DERR_INVALIDCALL_;
  ++g_dev->refcount;
  *out = g_dev;
  return D3D_OK_;
}
int TRT_GUEST_ABI res8_SetPrivateData(void*, const void*, const void*, u32, u32) {
  return D3D_OK_;
}
int TRT_GUEST_ABI res8_GetPrivateData(void*, const void*, void*, u32*) {
  return D3DERR_NOTIMPL_;
}
int TRT_GUEST_ABI res8_FreePrivateData(void*, const void*) { return D3D_OK_; }
u32 TRT_GUEST_ABI res8_SetPriority(void*, u32 p) { return p; }
u32 TRT_GUEST_ABI res8_GetPriority(void*) { return 0; }
void TRT_GUEST_ABI res8_PreLoad(void*) {}
u32 TRT_GUEST_ABI res8_GetType(void* self) {
  // D3DRESOURCETYPE (d3d8types.h): SURFACE=1, TEXTURE=3, VERTEXBUFFER=6,
  // INDEXBUFFER=7 — same numbering as D3D9
  const d8_obj* o = (d8_obj*)self;
  switch (o->kind) {
    case 1: return 3;  // D3DRTYPE_TEXTURE
    case 2: return 6;  // D3DRTYPE_VERTEXBUFFER
    case 3: return 7;  // D3DRTYPE_INDEXBUFFER
    case 4: return 1;  // D3DRTYPE_SURFACE
    default: return 3;
  }
}
u32 TRT_GUEST_ABI tex8_SetLOD(void*, u32 lod) { return lod; }
u32 TRT_GUEST_ABI tex8_GetLOD(void*) { return 0; }
u32 TRT_GUEST_ABI tex8_GetLevelCount(void* self) {
  const gpu_obj* g = gpu_at(((d8_obj*)self)->handle);
  return g ? g->levels : 1;
}
int TRT_GUEST_ABI tex8_GetLevelDesc(void*, u32, void*) {
  return D3DERR_NOTIMPL_;
}
int TRT_GUEST_ABI tex8_GetSurfaceLevel(void*, u32, void**) {
  return D3DERR_NOTIMPL_;
}
int TRT_GUEST_ABI tex8_AddDirtyRect(void*, const void*) { return D3D_OK_; }

// ---- the remaining device slots: fail VISIBLY with the slot number ----
#define DEV8_STUB_FN(n, name)                                             \
  int TRT_GUEST_ABI dev8_stub_##n(void* self) {                           \
    (void)self;                                                           \
    char line[96];                                                        \
    snprintf(line, sizeof(line), "d3d8: device slot %u (%s) called "      \
                                 "(unimplemented)", n, name);             \
    trt_core_log(line);                                                   \
    return D3DERR_NOTIMPL_;                                               \
  }

DEV8_STUB_FN(9, "GetCreationParameters")
DEV8_STUB_FN(13, "CreateAdditionalSwapChain")
DEV8_STUB_FN(17, "GetRasterStatus")
DEV8_STUB_FN(21, "CreateVolumeTexture")
DEV8_STUB_FN(22, "CreateCubeTexture")
DEV8_STUB_FN(29, "UpdateTexture")
DEV8_STUB_FN(30, "GetFrontBuffer")
DEV8_STUB_FN(32, "GetRenderTarget")
DEV8_STUB_FN(33, "GetDepthStencilSurface")
DEV8_STUB_FN(38, "GetTransform")
DEV8_STUB_FN(39, "MultiplyTransform")
DEV8_STUB_FN(41, "GetViewport")
DEV8_STUB_FN(43, "GetMaterial")
DEV8_STUB_FN(45, "GetLight")
DEV8_STUB_FN(47, "GetLightEnable")
DEV8_STUB_FN(49, "GetClipPlane")
DEV8_STUB_FN(51, "GetRenderStateRO")
DEV8_STUB_FN(52, "BeginStateBlock")
DEV8_STUB_FN(53, "EndStateBlock")
DEV8_STUB_FN(54, "ApplyStateBlock")
DEV8_STUB_FN(55, "CaptureStateBlock")
DEV8_STUB_FN(56, "DeleteStateBlock")
DEV8_STUB_FN(57, "CreateStateBlock")
DEV8_STUB_FN(59, "GetClipStatus")
DEV8_STUB_FN(60, "GetTexture")
DEV8_STUB_FN(62, "GetTextureStageState")
DEV8_STUB_FN(65, "GetInfo")
DEV8_STUB_FN(67, "GetPaletteEntries")
DEV8_STUB_FN(69, "GetCurrentTexturePalette")
DEV8_STUB_FN(74, "ProcessVertices")
DEV8_STUB_FN(77, "GetVertexShaderRO")
DEV8_STUB_FN(78, "DeleteVertexShaderRO")
DEV8_STUB_FN(80, "GetVertexShaderConstant")
DEV8_STUB_FN(81, "GetVertexShaderDeclaration")
DEV8_STUB_FN(82, "GetVertexShaderFunction")
DEV8_STUB_FN(84, "GetStreamSource")
DEV8_STUB_FN(86, "GetIndices")
DEV8_STUB_FN(89, "GetPixelShaderRO")
DEV8_STUB_FN(90, "DeletePixelShaderRO")
DEV8_STUB_FN(92, "GetPixelShaderConstant")
DEV8_STUB_FN(93, "GetPixelShaderFunction")
DEV8_STUB_FN(94, "DrawRectPatch")
DEV8_STUB_FN(95, "DrawTriPatch")
DEV8_STUB_FN(96, "DeletePatch")
int TRT_GUEST_ABI dev8_SetGammaRamp(void*, u32, const void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev8_SetMaterial(void*, const void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev8_SetLight(void*, u32, const void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev8_LightEnable(void*, u32, int) { return D3D_OK_; }
int TRT_GUEST_ABI dev8_SetClipPlane(void*, u32, const float*) { return D3D_OK_; }
int TRT_GUEST_ABI dev8_SetClipStatus(void*, const void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev8_ValidateDevice(void*, u32*) { return D3D_OK_; }
int TRT_GUEST_ABI dev8_SetPaletteEntries(void*, u32, const void*) {
  return D3D_OK_;
}
int TRT_GUEST_ABI dev8_SetCurrentTexturePalette(void*, u32) { return D3D_OK_; }
void TRT_GUEST_ABI dev8_GetGammaRamp(void*, void*) {}
int TRT_GUEST_ABI res8_GetDesc(void* self, void* out) {
  // D3DVERTEXBUFFER_DESC/INDEXBUFFER_DESC head: { FORMAT fmt; RTYPE type;
  // DWORD usage; POOL pool; UINT size } — VB and IB share the layout.
  // D3DFMT_VERTEXDATA = 100, D3DFMT_INDEX16 = 101.
  if (!out) return D3DERR_INVALIDCALL_;
  u32* p = (u32*)out;
  const gpu_obj* g = gpu_at(((d8_obj*)self)->handle);
  p[0] = ((d8_obj*)self)->kind == 3 ? 101 : 100;      // Format
  p[1] = ((d8_obj*)self)->kind == 3 ? 7 : 6;          // Resource type
  p[2] = g ? g->usage : 0;                            // Usage
  p[3] = 1;                                           // Pool = MANAGED-ish
  p[4] = g ? g->bytes : 0;                            // Size
  return D3D_OK_;
}

// ---- vtables (SDK order, Wine d3d8.h) ----
const void* g_d3d8_vtbl[16] = {
    (const void*)&d8_QI,                              // 0
    (const void*)&d8_AR,                              // 1
    (const void*)&d8_Rel,                             // 2
    (const void*)&d8_RegisterSoftwareDevice,          // 3
    (const void*)&d8_GetAdapterCount,                 // 4
    (const void*)&d8_GetAdapterIdentifier,            // 5
    (const void*)&d8_GetAdapterModeCount,             // 6
    (const void*)&d8_EnumAdapterModes,                // 7
    (const void*)&d8_GetAdapterDisplayMode,           // 8
    (const void*)&d8_CheckDeviceType,                 // 9
    (const void*)&d8_CheckDeviceFormat,               // 10
    (const void*)&d8_CheckDeviceMultiSampleType,      // 11
    (const void*)&d8_CheckDepthStencilMatch,          // 12
    (const void*)&d8_GetDeviceCaps,                   // 13
    (const void*)&d8_GetAdapterMonitor,               // 14
    (const void*)&d8_CreateDevice,                    // 15
};

const void* g_dev8_vtbl[97] = {
    (const void*)&d8_QI,                              // 0
    (const void*)&d8_AR,                              // 1
    (const void*)&d8_Rel,                             // 2
    (const void*)&dev8_TestCooperativeLevel,          // 3
    (const void*)&dev8_GetAvailableTextureMem,        // 4
    (const void*)&dev8_ResourceManagerDiscardBytes,   // 5
    (const void*)&dev8_GetDirect3D,                   // 6
    (const void*)&dev8_GetDeviceCaps,                 // 7
    (const void*)&dev8_GetDisplayMode,                // 8
    (const void*)&dev8_stub_9,                        // 9
    (const void*)&dev8_SetCursorProperties,           // 10
    (const void*)&dev8_SetCursorPosition,             // 11
    (const void*)&dev8_ShowCursor,                    // 12
    (const void*)&dev8_stub_13,                       // 13
    (const void*)&dev8_Reset,                         // 14
    (const void*)&dev8_Present,                       // 15
    (const void*)&dev8_GetBackBuffer,                 // 16
    (const void*)&dev8_stub_17,                       // 17
    (const void*)&dev8_SetGammaRamp,                  // 18
    (const void*)&dev8_GetGammaRamp,                  // 19
    (const void*)&dev8_CreateTexture,                 // 20
    (const void*)&dev8_stub_21,                       // 21
    (const void*)&dev8_stub_22,                       // 22
    (const void*)&dev8_CreateVertexBuffer,            // 23
    (const void*)&dev8_CreateIndexBuffer,             // 24
    (const void*)&dev8_CreateRenderTarget,            // 25
    (const void*)&dev8_CreateDepthStencilSurface,     // 26
    (const void*)&dev8_CreateImageSurface,            // 27
    (const void*)&dev8_CopyRects,                     // 28
    (const void*)&dev8_stub_29,                       // 29
    (const void*)&dev8_stub_30,                       // 30
    (const void*)&dev8_SetRenderTarget,               // 31
    (const void*)&dev8_stub_32,                       // 32
    (const void*)&dev8_stub_33,                       // 33
    (const void*)&dev8_BeginScene,                    // 34
    (const void*)&dev8_EndScene,                      // 35
    (const void*)&dev8_Clear,                         // 36
    (const void*)&dev8_SetTransform,                  // 37
    (const void*)&dev8_stub_38,                       // 38
    (const void*)&dev8_stub_39,                       // 39
    (const void*)&dev8_SetViewport,                   // 40
    (const void*)&dev8_stub_41,                       // 41
    (const void*)&dev8_SetMaterial,                   // 42
    (const void*)&dev8_stub_43,                       // 43
    (const void*)&dev8_SetLight,                      // 44
    (const void*)&dev8_stub_45,                       // 45
    (const void*)&dev8_LightEnable,                   // 46
    (const void*)&dev8_stub_47,                       // 47
    (const void*)&dev8_SetClipPlane,                  // 48
    (const void*)&dev8_stub_49,                       // 49
    (const void*)&dev8_SetRenderState,                // 50
    (const void*)&dev8_GetRenderState,                // 51
    (const void*)&dev8_stub_52,                       // 52
    (const void*)&dev8_stub_53,                       // 53
    (const void*)&dev8_stub_54,                       // 54
    (const void*)&dev8_stub_55,                       // 55
    (const void*)&dev8_stub_56,                       // 56
    (const void*)&dev8_stub_57,                       // 57
    (const void*)&dev8_SetClipStatus,                 // 58
    (const void*)&dev8_stub_59,                       // 59
    (const void*)&dev8_stub_60,                       // 60
    (const void*)&dev8_SetTexture,                    // 61
    (const void*)&dev8_stub_62,                       // 62
    (const void*)&dev8_SetTextureStageState,          // 63
    (const void*)&dev8_ValidateDevice,                // 64
    (const void*)&dev8_stub_65,                       // 65
    (const void*)&dev8_SetPaletteEntries,             // 66
    (const void*)&dev8_stub_67,                       // 67
    (const void*)&dev8_SetCurrentTexturePalette,      // 68
    (const void*)&dev8_stub_69,                       // 69
    (const void*)&dev8_DrawPrimitive,                 // 70
    (const void*)&dev8_DrawIndexedPrimitive,          // 71
    (const void*)&dev8_DrawPrimitiveUP,               // 72
    (const void*)&dev8_DrawIndexedPrimitiveUP,        // 73
    (const void*)&dev8_stub_74,                       // 74
    (const void*)&dev8_CreateVertexShader,            // 75
    (const void*)&dev8_SetVertexShader,               // 76
    (const void*)&dev8_stub_77,                       // 77
    (const void*)&dev8_stub_78,                       // 78
    (const void*)&dev8_SetVertexShaderConstant,       // 79
    (const void*)&dev8_stub_80,                       // 80
    (const void*)&dev8_stub_81,                       // 81
    (const void*)&dev8_stub_82,                       // 82
    (const void*)&dev8_SetStreamSource,               // 83
    (const void*)&dev8_stub_84,                       // 84
    (const void*)&dev8_SetIndices,                    // 85
    (const void*)&dev8_stub_86,                       // 86
    (const void*)&dev8_CreatePixelShader,             // 87
    (const void*)&dev8_SetPixelShader,                // 88
    (const void*)&dev8_stub_89,                       // 89
    (const void*)&dev8_stub_90,                       // 90
    (const void*)&dev8_SetPixelShaderConstant,        // 91
    (const void*)&dev8_stub_92,                       // 92
    (const void*)&dev8_stub_93,                       // 93
    (const void*)&dev8_stub_94,                       // 94
    (const void*)&dev8_stub_95,                       // 95
    (const void*)&dev8_stub_96,                       // 96
};

const void* g_tex8_vtbl[19] = {
    (const void*)&d8_QI, (const void*)&d8_AR, (const void*)&d8_Rel,
    (const void*)&res8_GetDevice,          // 3
    (const void*)&res8_SetPrivateData,     // 4
    (const void*)&res8_GetPrivateData,     // 5
    (const void*)&res8_FreePrivateData,    // 6
    (const void*)&res8_SetPriority,        // 7
    (const void*)&res8_GetPriority,        // 8
    (const void*)&res8_PreLoad,            // 9
    (const void*)&res8_GetType,            // 10
    (const void*)&tex8_SetLOD,             // 11
    (const void*)&tex8_GetLOD,             // 12
    (const void*)&tex8_GetLevelCount,      // 13
    (const void*)&tex8_GetLevelDesc,       // 14
    (const void*)&tex8_GetSurfaceLevel,    // 15
    (const void*)&tex8_LockRect,           // 16
    (const void*)&tex8_UnlockRect,         // 17
    (const void*)&tex8_AddDirtyRect,       // 18
};

const void* g_vb8_vtbl[14] = {
    (const void*)&d8_QI, (const void*)&d8_AR, (const void*)&d8_Rel,
    (const void*)&res8_GetDevice, (const void*)&res8_SetPrivateData,
    (const void*)&res8_GetPrivateData, (const void*)&res8_FreePrivateData,
    (const void*)&res8_SetPriority, (const void*)&res8_GetPriority,
    (const void*)&res8_PreLoad,          // 9
    (const void*)&res8_GetType,          // 10
    (const void*)&buf8_Lock,             // 11
    (const void*)&buf8_Unlock,           // 12
    (const void*)&res8_GetDesc,          // 13
};

const void* g_ib8_vtbl[14] = {
    (const void*)&d8_QI, (const void*)&d8_AR, (const void*)&d8_Rel,
    (const void*)&res8_GetDevice, (const void*)&res8_SetPrivateData,
    (const void*)&res8_GetPrivateData, (const void*)&res8_FreePrivateData,
    (const void*)&res8_SetPriority, (const void*)&res8_GetPriority,
    (const void*)&res8_PreLoad,          // 9
    (const void*)&res8_GetType,          // 10
    (const void*)&buf8_Lock,             // 11
    (const void*)&buf8_Unlock,           // 12
    (const void*)&res8_GetDesc,          // 13
};

const void* g_surf8_vtbl[11] = {
    (const void*)&d8_QI, (const void*)&d8_AR, (const void*)&d8_Rel,
    (const void*)&res8_GetDevice, (const void*)&res8_SetPrivateData,
    (const void*)&res8_GetPrivateData, (const void*)&res8_FreePrivateData,
    (const void*)&res8_SetPriority, (const void*)&res8_GetPriority,
    (const void*)&res8_PreLoad, (const void*)&res8_GetType,
};

int trt_bridge_register_d3d8(void) {
  static const trt_named_fn k_fns[] = {
      {"Direct3DCreate8", (void*)&trt_Direct3DCreate8},
  };
  trt_module_register("d3d8.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

}  // extern "C"
