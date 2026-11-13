// tiramisu/src/trt/bridge_d3d9.cpp
// The D3D9 bridge (M5). See bridge_d3d9.h for the design: SDK-exact COM
// vtables, plain-C host hooks, fail-visible stubs.
//
// Vtable slot orders verified against the Windows SDK's d3d9.h:
//   IDirect3D9            = 17 slots (CreateDevice = 16)
//   IDirect3DDevice9      = 119 slots (Present = 17, CreateTexture = 23,
//                             SetRenderState = 57, DrawIndexedPrimitive = 82,
//                             DrawPrimitiveUP = 83, SetStreamSource = 100)
//   IDirect3DTexture9     = 22 slots (LockRect = 19, UnlockRect = 20)
//   IDirect3DVertexBuffer9 / IndexBuffer9 = 13 (Lock = 11, Unlock = 12)
//   IDirect3DSurface9 (Resource9) = 11
//   Vertex/PixelShader9, VertexDeclaration9 = 5
//
// Semantics follow Microsoft's D3D9On12 (MIT): D3DRS_* render-state values
// pass through verbatim; the host backend maps them to D3D11, just as
// 9on12 maps them to D3D12.
#include "bridge_d3d9.h"
#include "trt.h"
#include "thunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr int D3D_OK_ = 0;
constexpr int D3DERR_NOTIMPL_ = (int)0x8876086A;
constexpr int D3DERR_INVALIDCALL_ = (int)0x8876086C;

// ---- hooks ----
d3d9_hooks g_hooks;

// self-test observation: (prim_type<<16)|prim_count of the last draw
u32 g_last_draw = 0;

// ---- shared COM header: every object starts with this ----
struct d9_obj;
struct d9_obj {
  const void** vtbl;
  u32 refcount;
  u32 handle;  // GPU object this wraps (0 = pure interface)
  u32 kind;    // 1 tex, 2 vb, 3 ib, 4 surface, 5 vs, 6 ps, 7 decl
};

// object pool (games churn thousands; recycle on Release)
constexpr u32 k_max_objs = 512;
u8 g_pool[k_max_objs * sizeof(d9_obj)];
bool g_pool_used[k_max_objs];
u32 g_pool_scan = 0;

d9_obj* obj_alloc(const void** vtbl, u32 handle, u32 kind) {
  for (u32 i = 0; i < k_max_objs; ++i) {
    const u32 idx = (g_pool_scan + i) % k_max_objs;
    if (!g_pool_used[idx]) {
      g_pool_used[idx] = true;
      ++g_pool_scan;
      d9_obj* o = (d9_obj*)(g_pool + idx * sizeof(d9_obj));
      o->vtbl = vtbl;
      o->refcount = 1;
      o->handle = handle;
      o->kind = kind;
      return o;
    }
  }
  return nullptr;
}

void obj_free(d9_obj* o) {
  const u32 idx = (u32)(((u8*)o - g_pool) / sizeof(d9_obj));
  if (idx < k_max_objs) g_pool_used[idx] = false;
}

// GPU-object metadata (the host backend allocates the real things; we
// track sizes for LockRect pitch and for the offline object table)
struct gpu_obj {
  u32 alive, kind, w, h, levels, bytes, usage, fmt;
};
constexpr u32 k_max_gpu = 512;
gpu_obj g_gpu[k_max_gpu];
u32 g_gpu_next = 1;

u32 gpu_alloc(u32 kind, u32 w, u32 hh, u32 levels, u32 bytes, u32 usage, u32 fmt) {
  if (g_gpu_next >= k_max_gpu) return 0;
  const u32 h = g_gpu_next++;
  g_gpu[h] = {1, kind, w, hh, levels, bytes, usage, fmt};
  return h;
}
gpu_obj* gpu_at(u32 h) {
  return (h && h < g_gpu_next && g_gpu[h].alive) ? &g_gpu[h] : nullptr;
}

// singleton interfaces the guest holds for the process lifetime
d9_obj* g_d3d9_root = nullptr;   // IDirect3D9
d9_obj* g_dev = nullptr;         // IDirect3DDevice9
d9_obj* g_backbuffer = nullptr;  // IDirect3DSurface9

// active draw state marshalled to the host at draw time
u32 g_stream_vb[2] = {0, 0};
u32 g_index_ib = 0;
u32 g_cur_fvf = 0;

// scratch backing for locks when no host backend is bound (offline tests)
u8 g_scratch[1 << 20];

}  // namespace

// forward declarations: the vtables live at the bottom (SDK order), the
// functions they point at are defined above them
extern "C" {
extern const void* g_d3d9_vtbl[17];
extern const void* g_dev9_vtbl[119];
extern const void* g_tex9_vtbl[22];
extern const void* g_vb9_vtbl[13];
extern const void* g_ib9_vtbl[13];
extern const void* g_vs9_vtbl[5];
extern const void* g_ps9_vtbl[5];
extern const void* g_decl9_vtbl[5];
extern const void* g_surf9_vtbl[11];
}

extern "C" {

// ---- IUnknown ----
int TRT_GUEST_ABI d9_QI(void* self, const void*, void** ppv) {
  if (!ppv) return D3DERR_INVALIDCALL_;
  d9_obj* o = (d9_obj*)self;
  *ppv = self;
  ++o->refcount;
  return D3D_OK_;
}
u32 TRT_GUEST_ABI d9_AR(void* self) { return ++((d9_obj*)self)->refcount; }
u32 TRT_GUEST_ABI d9_Rel(void* self) {
  d9_obj* o = (d9_obj*)self;
  if (o->refcount) --o->refcount;
  if (o->refcount == 0 && o != (d9_obj*)g_d3d9_root &&
      o != (d9_obj*)g_dev && o != (d9_obj*)g_backbuffer) {
    if (g_hooks.release && o->handle) g_hooks.release(o->handle, g_hooks.user);
    if (gpu_at(o->handle)) g_gpu[o->handle].alive = 0;
    obj_free(o);
  }
  return o->refcount;
}

// ---- IDirect3D9 (17 slots) ----
int TRT_GUEST_ABI d9_RegisterSoftwareDevice(void*, void*) {
  return D3DERR_NOTIMPL_;
}
u32 TRT_GUEST_ABI d9_GetAdapterCount(void*) { return 1; }
int TRT_GUEST_ABI d9_GetAdapterIdentifier(void*, u32, u32, void* out) {
  if (!out) return D3DERR_INVALIDCALL_;
  // D3DADAPTER_IDENTIFIER9: Driver[512] at 0, Description[512] at 0x200
  memset(out, 0, 0x2A0);
  memcpy((char*)out, "Tiramisu9On11", 14);
  memcpy((char*)out + 0x200, "Tiramisu D3D9 -> D3D11 bridge", 30);
  return D3D_OK_;
}
u32 TRT_GUEST_ABI d9_GetAdapterModeCount(void*, u32, u32) { return 1; }
int TRT_GUEST_ABI d9_EnumAdapterModes(void*, u32, u32 fmt, u32 mode, void* out) {
  if (!out) return D3DERR_INVALIDCALL_;
  if (mode > 0) return D3DERR_INVALIDCALL_;  // exactly one mode
  // D3DDISPLAYMODE { UINT Width, Height, RefreshRate; FORMAT Format; }
  u32* p = (u32*)out;
  p[0] = g_hooks.width;
  p[1] = g_hooks.height;
  p[2] = 60;
  p[3] = fmt;
  return D3D_OK_;
}
int TRT_GUEST_ABI d9_GetAdapterDisplayMode(void*, u32 adapter, void* out) {
  return d9_EnumAdapterModes(nullptr, adapter, 21 /*D3DFMT_X8R8G8B8*/, 0, out);
}
int TRT_GUEST_ABI d9_CheckDeviceType(void*, u32, u32, u32, u32, int) {
  return D3D_OK_;
}
int TRT_GUEST_ABI d9_CheckDeviceFormat(void*, u32, u32, u32, u32, u32, u32) {
  return D3D_OK_;
}
int TRT_GUEST_ABI d9_CheckDeviceMultiSampleType(void*, u32, u32, int, u32, int) {
  return D3D_OK_;
}
int TRT_GUEST_ABI d9_CheckDepthStencilMatch(void*, u32, u32, u32, u32, u32) {
  return D3D_OK_;
}
int TRT_GUEST_ABI d9_CheckDeviceFormatConversion(void*, u32, u32, u32) {
  return D3D_OK_;
}
int TRT_GUEST_ABI d9_GetDeviceCaps(void*, u32, u32, void* out) {
  if (!out) return D3DERR_INVALIDCALL_;
  memset(out, 0, 0x200);
  u32* p = (u32*)out;
  p[0x10 / 4] = 0x00000020;  // DevCaps: D3DDEVCAPS_HWTRANSFORMANDLIGHT-ish
  p[0x54 / 4] = 8192;        // MaxTextureWidth
  p[0x58 / 4] = 8192;        // MaxTextureHeight
  p[0x8C / 4] = 65535;       // MaxPrimitiveCount
  p[0x94 / 4] = 8;           // MaxTextureBlendStages
  p[0x98 / 4] = 8;           // MaxSimultaneousTextures
  p[0xE4 / 4] = 256;         // MaxVertexShaderConst
  p[0xE8 / 4] = 0x00030000;  // VertexShaderVersion 3_0
  p[0xEC / 4] = 0x00030000;  // PixelShaderVersion 3_0
  return D3D_OK_;
}
void* TRT_GUEST_ABI d9_GetAdapterMonitor(void*, u32) { return (void*)0x11; }

// Direct3DCreate9(SDKVersion) -> IDirect3D9*  (module entry point)
void* TRT_GUEST_ABI trt_Direct3DCreate9(u32 sdk_version) {
  char line[128];
  snprintf(line, sizeof(line), "d3d9: Direct3DCreate9(sdk=%u) -> bridge",
           sdk_version);
  trt_core_log(line);
  if (!g_d3d9_root) {
    g_d3d9_root = obj_alloc(g_d3d9_vtbl, 0, 0);
    if (!g_d3d9_root) return nullptr;
  }
  ++g_d3d9_root->refcount;
  return g_d3d9_root;
}
int TRT_GUEST_ABI trt_Direct3DCreate9Ex(u32 sdk_version, void** out) {
  if (!out) return D3DERR_INVALIDCALL_;
  void* d = trt_Direct3DCreate9(sdk_version);
  if (!d) return D3DERR_NOTIMPL_;
  *out = d;
  return D3D_OK_;
}

// CreateDevice(adapter, type, hwnd, flags, D3DPRESENT_PARAMETERS*, out_dev)
int TRT_GUEST_ABI d9_CreateDevice(void*, u32 adapter, u32 dev_type, void* hwnd,
                                  u32 flags, void* pp, void** out_dev) {
  (void)adapter; (void)hwnd;
  char line[192];
  snprintf(line, sizeof(line),
           "d3d9: CreateDevice(type=%u flags=0x%x) -> bridge device",
           dev_type, flags);
  trt_core_log(line);
  if (!out_dev || !pp) return D3DERR_INVALIDCALL_;
  if (!g_hooks.present) return D3DERR_NOTIMPL_;  // no host backend bound
  // D3DPRESENT_PARAMETERS: +0 BackBufferWidth, +4 Height
  u32* p = (u32*)pp;
  if (g_hooks.set_viewport) {
    d3d9_viewport vp{0, 0, p[0], p[1], 0.f, 1.f};
    if (!vp.w || !vp.h) { vp.w = g_hooks.width; vp.h = g_hooks.height; }
    g_hooks.set_viewport(&vp, g_hooks.user);
  }
  if (!g_dev) {
    g_dev = obj_alloc(g_dev9_vtbl, 0, 0);
    if (!g_dev) return D3DERR_NOTIMPL_;
  }
  ++g_dev->refcount;
  *out_dev = g_dev;
  return D3D_OK_;
}

// ---- resource creation (device slots 23-30, 86, 91, 106) ----
int TRT_GUEST_ABI dev_CreateTexture(void*, u32 w, u32 h, u32 levels, u32 usage,
                                    u32 fmt, u32 pool, void** out) {
  (void)pool;
  if (!out) return D3DERR_INVALIDCALL_;
  const u32 hnd = g_hooks.create_tex
                      ? g_hooks.create_tex(w, h, levels ? levels : 1, usage,
                                           fmt, g_hooks.user)
                      : gpu_alloc(1, w, h, levels ? levels : 1, 0, usage, fmt);
  d9_obj* o = obj_alloc(g_tex9_vtbl, hnd, 1);
  if (!o) return D3DERR_NOTIMPL_;
  *out = o;
  char line[160];
  snprintf(line, sizeof(line), "d3d9: CreateTexture(%ux%u fmt=%u) -> h%u", w,
           h, fmt, hnd);
  trt_core_log(line);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_CreateVertexBuffer(void*, u32 bytes, u32 usage, u32 fvf,
                                         u32 pool, void** out, void** handle) {
  (void)usage; (void)pool; (void)handle;
  if (!out) return D3DERR_INVALIDCALL_;
  const u32 hnd = g_hooks.create_buf
                      ? g_hooks.create_buf(bytes, 0, g_hooks.user)
                      : gpu_alloc(2, 0, 0, 1, bytes, usage, fvf);
  d9_obj* o = obj_alloc(g_vb9_vtbl, hnd, 2);
  if (!o) return D3DERR_NOTIMPL_;
  *out = o;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_CreateIndexBuffer(void*, u32 bytes, u32 usage, u32 fmt,
                                        u32 pool, void** out, void** handle) {
  (void)usage; (void)fmt; (void)pool; (void)handle;
  if (!out) return D3DERR_INVALIDCALL_;
  const u32 hnd = g_hooks.create_buf
                      ? g_hooks.create_buf(bytes, 1, g_hooks.user)
                      : gpu_alloc(3, 0, 0, 1, bytes, usage, fmt);
  d9_obj* o = obj_alloc(g_ib9_vtbl, hnd, 3);
  if (!o) return D3DERR_NOTIMPL_;
  *out = o;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_CreateRenderTarget(void*, u32 w, u32 h, u32 fmt, u32 multi,
                                         u32 msq, int windowed, void** out,
                                         void** handle) {
  (void)multi; (void)msq; (void)windowed; (void)handle;
  if (!out) return D3DERR_INVALIDCALL_;
  const u32 hnd = g_hooks.create_tex
                      ? g_hooks.create_tex(w, h, 1, 1 /*RENDERTARGET*/, fmt,
                                           g_hooks.user)
                      : gpu_alloc(4, w, h, 1, 0, 1, fmt);
  d9_obj* o = obj_alloc(g_surf9_vtbl, hnd, 4);
  if (!o) return D3DERR_NOTIMPL_;
  *out = o;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_CreateDepthStencilSurface(void* self, u32 w, u32 h,
                                                u32 fmt, u32 multi, u32 msq,
                                                int discard, int zenable,
                                                void** out) {
  (void)discard; (void)zenable;
  return dev_CreateRenderTarget(self, w, h, fmt, multi, msq, 0, out, nullptr);
}
int TRT_GUEST_ABI dev_CreateVertexShader(void*, const u32* code, void** out) {
  (void)code;
  if (!out) return D3DERR_INVALIDCALL_;
  d9_obj* o = obj_alloc(g_vs9_vtbl, 0, 5);
  if (!o) return D3DERR_NOTIMPL_;
  *out = o;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_CreatePixelShader(void*, const u32* code, void** out) {
  (void)code;
  if (!out) return D3DERR_INVALIDCALL_;
  d9_obj* o = obj_alloc(g_ps9_vtbl, 0, 6);
  if (!o) return D3DERR_NOTIMPL_;
  *out = o;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_CreateVertexDeclaration(void*, const void* elems, void** out) {
  (void)elems;
  if (!out) return D3DERR_INVALIDCALL_;
  d9_obj* o = obj_alloc(g_decl9_vtbl, 0, 7);
  if (!o) return D3DERR_NOTIMPL_;
  *out = o;
  return D3D_OK_;
}

// ---- state marshalling ----
int TRT_GUEST_ABI dev_SetTransform(void*, u32 kind, const void* m) {
  if (g_hooks.set_transform && m)
    g_hooks.set_transform(kind, (const float*)m, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetViewport(void*, const void* vp) {
  if (g_hooks.set_viewport && vp)
    g_hooks.set_viewport((const d3d9_viewport*)vp, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetRenderState(void*, u32 state, u32 value) {
  if (g_hooks.set_rs) g_hooks.set_rs(state, value, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_GetRenderState(void*, u32, u32* out) {
  if (out) *out = 0;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetTexture(void*, u32 stage, void* tex) {
  const u32 hnd = tex ? ((d9_obj*)tex)->handle : 0;
  if (g_hooks.set_texture) g_hooks.set_texture(stage, hnd, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetSamplerState(void*, u32 stage, u32 state, u32 value) {
  if (g_hooks.set_sampler) g_hooks.set_sampler(stage, state, value, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetTextureStageState(void*, u32 stage, u32 state, u32 value) {
  if (g_hooks.set_sampler) g_hooks.set_sampler(stage, state, value, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetFVF(void*, u32 fvf) {
  g_cur_fvf = fvf;
  if (g_hooks.set_fvf) g_hooks.set_fvf(fvf, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetVertexDeclaration(void*, void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev_SetVertexShader(void*, void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev_SetPixelShader(void*, void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev_SetVertexShaderConstantF(void*, u32, const float*, u32) {
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetPixelShaderConstantF(void*, u32, const float*, u32) {
  return D3D_OK_;
}

// ---- frame lifecycle ----
int TRT_GUEST_ABI dev_BeginScene(void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev_EndScene(void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev_Clear(void*, u32, const void*, u32 flags, u32 color,
                            float z, u32 stencil) {
  if (g_hooks.clear) g_hooks.clear(flags, color, z, stencil, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_Present(void*, const void*, const void*, const void*,
                              void*) {
  if (g_hooks.present) g_hooks.present(g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetRenderTarget(void*, u32 index, void* rt) {
  if (index != 0) return D3D_OK_;  // only MRT slot 0 maps
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetDepthStencilSurface(void*, void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev_Reset(void*, void*) {
  char line[96];
  snprintf(line, sizeof(line), "d3d9: Reset -> D3D_OK (device-lost recovery)");
  trt_core_log(line);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_GetBackBuffer(void*, u32, u32, u32, void** out) {
  if (!out) return D3DERR_INVALIDCALL_;
  if (!g_backbuffer) {
    g_backbuffer = obj_alloc(g_surf9_vtbl, 0, 4);
    if (!g_backbuffer) return D3DERR_NOTIMPL_;
  }
  ++g_backbuffer->refcount;
  *out = g_backbuffer;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_TestCooperativeLevel(void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev_GetDirect3D(void*, void** out) {
  if (!out || !g_d3d9_root) return D3DERR_INVALIDCALL_;
  ++g_d3d9_root->refcount;
  *out = g_d3d9_root;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_GetDeviceCaps(void* self, void* out) {
  (void)self;
  return d9_GetDeviceCaps(nullptr, 0, 0, out);
}
void TRT_GUEST_ABI dev_SetCursorPosition(void*, u32, u32, u32) {}
int TRT_GUEST_ABI dev_ShowCursor(void*, int b) { return b; }
int TRT_GUEST_ABI dev_SetCursorProperties(void*, u32, u32, void*) {
  return D3D_OK_;
}
u32 TRT_GUEST_ABI dev_GetAvailableTextureMem(void*) { return 0x10000000; }
int TRT_GUEST_ABI dev_EvictManagedResources(void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev_GetDisplayMode(void*, u32 swapchain, void* out) {
  (void)swapchain;
  return d9_GetAdapterDisplayMode(nullptr, 0, out);
}

// ---- draws ----
int TRT_GUEST_ABI dev_DrawPrimitive(void*, u32 prim, u32 start_v, u32 count) {
  g_last_draw = (prim << 16) | (count & 0xFFFF);
  if (g_hooks.draw)
    g_hooks.draw(prim, g_stream_vb[0], 0, 0, start_v, count * 3 + 1, 0, count,
                 g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_DrawIndexedPrimitive(void*, u32 prim, i32 base_idx,
                                           u32 min_idx, u32 num_verts,
                                           u32 start_idx, u32 count) {
  g_last_draw = (prim << 16) | (count & 0xFFFF);
  if (g_hooks.draw)
    g_hooks.draw(prim, g_stream_vb[0], g_index_ib, (u32)base_idx, min_idx,
                 num_verts, start_idx, count, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_DrawPrimitiveUP(void*, u32 prim, u32 count, const void*,
                                      u32) {
  g_last_draw = (prim << 16) | (count & 0xFFFF);
  if (g_hooks.draw)
    g_hooks.draw(prim, 0, 0, 0, 0, count * 3 + 1, 0, count, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_DrawIndexedPrimitiveUP(void*, u32 prim, u32 min_idx,
                                             u32 num_verts, u32 count,
                                             const void*, u32, const void*, u32) {
  g_last_draw = (prim << 16) | (count & 0xFFFF);
  if (g_hooks.draw)
    g_hooks.draw(prim, 0, 0, 0, min_idx, num_verts, 0, count, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetStreamSource(void*, u32 stream, void* vb, u32, u32) {
  if (stream < 2) g_stream_vb[stream] = vb ? ((d9_obj*)vb)->handle : 0;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetIndices(void*, void* ib) {
  g_index_ib = ib ? ((d9_obj*)ib)->handle : 0;
  return D3D_OK_;
}

// ---- locks. With a host backend bound the backend owns the memory; the
// hook returns the pointer. Offline, a shared scratch buffer serves so the
// guest can write and read back. ----
int TRT_GUEST_ABI buf_Lock(void* self, u32 offset, u32 bytes, void** data,
                           u32) {
  if (!data) return D3DERR_INVALIDCALL_;
  if (g_hooks.lock) {
    *data = g_hooks.lock(((d9_obj*)self)->handle, 0, offset, bytes, g_hooks.user);
    if (*data) return D3D_OK_;
  }
  if (offset >= sizeof(g_scratch)) return D3DERR_INVALIDCALL_;
  if (bytes == 0 || offset + bytes > sizeof(g_scratch))
    bytes = (u32)sizeof(g_scratch) - offset;
  *data = g_scratch + offset;
  return D3D_OK_;
}
int TRT_GUEST_ABI buf_Unlock(void* self) {
  if (g_hooks.unlock) g_hooks.unlock(((d9_obj*)self)->handle, g_hooks.user);
  return D3D_OK_;
}
int TRT_GUEST_ABI tex_LockRect(void* self, u32 level, void* lr, const void*,
                               u32) {
  if (!lr) return D3DERR_INVALIDCALL_;
  // D3DLOCKED_RECT { INT Pitch; void* pBits; } — x64: pBits at +8
  i32* p = (i32*)lr;
  void* bits = nullptr;
  if (g_hooks.lock) {
    bits = g_hooks.lock(((d9_obj*)self)->handle, level, 0, 0, g_hooks.user);
    if (!bits) bits = g_scratch;
  } else {
    bits = g_scratch;
  }
  p[0] = 4096;
  *(void**)(p + 2) = bits;
  return D3D_OK_;
}
int TRT_GUEST_ABI tex_UnlockRect(void* self, u32 level) {
  if (g_hooks.unlock) g_hooks.unlock(((d9_obj*)self)->handle, g_hooks.user);
  (void)level;
  return D3D_OK_;
}

// ---- texture/resource shared slots ----
int TRT_GUEST_ABI res_GetDevice(void*, void** out) {
  if (!out || !g_dev) return D3DERR_INVALIDCALL_;
  ++g_dev->refcount;
  *out = g_dev;
  return D3D_OK_;
}
int TRT_GUEST_ABI res_SetPrivateData(void*, const void*, const void*, u32, u32) {
  return D3D_OK_;
}
int TRT_GUEST_ABI res_GetPrivateData(void*, const void*, void*, u32*) {
  return D3DERR_NOTIMPL_;
}
int TRT_GUEST_ABI res_FreePrivateData(void*, const void*) { return D3D_OK_; }
u32 TRT_GUEST_ABI res_SetPriority(void*, u32 p) { return p; }
u32 TRT_GUEST_ABI res_GetPriority(void*) { return 0; }
void TRT_GUEST_ABI res_PreLoad(void*) {}
u32 TRT_GUEST_ABI res_GetType(void* self) {
  // D3DRESOURCETYPE (d3d9types.h): SURFACE=1, TEXTURE=3, VERTEXBUFFER=6,
  // INDEXBUFFER=7
  const d9_obj* o = (d9_obj*)self;
  switch (o->kind) {
    case 1: return 3;  // D3DRTYPE_TEXTURE
    case 2: return 6;  // D3DRTYPE_VERTEXBUFFER
    case 3: return 7;  // D3DRTYPE_INDEXBUFFER
    case 4: return 1;  // D3DRTYPE_SURFACE
    default: return 3;
  }
}
u32 TRT_GUEST_ABI tex_SetLOD(void*, u32 lod) { return lod; }
u32 TRT_GUEST_ABI tex_GetLOD(void*) { return 0; }
u32 TRT_GUEST_ABI tex_GetLevelCount(void* self) {
  const gpu_obj* g = gpu_at(((d9_obj*)self)->handle);
  return g ? g->levels : 1;
}
u32 TRT_GUEST_ABI tex_SetAutoGenFilterType(void*, u32 t) { return t; }
u32 TRT_GUEST_ABI tex_GetAutoGenFilterType(void*) {
  return 2;  // D3DTEXF_LINEAR
}
void TRT_GUEST_ABI tex_GenerateMipSubLevels(void*) {}
int TRT_GUEST_ABI tex_GetLevelDesc(void*, u32, void*) {
  return D3DERR_NOTIMPL_;
}
int TRT_GUEST_ABI tex_GetSurfaceLevel(void*, u32, void**) {
  return D3DERR_NOTIMPL_;
}
int TRT_GUEST_ABI tex_AddDirtyRect(void*, const void*) { return D3D_OK_; }
int TRT_GUEST_ABI shader_GetFunction(void*, void*, u32*) {
  return D3DERR_NOTIMPL_;
}

// ---- the remaining device slots: fail VISIBLY with the slot number ----
// one named function per slot so the log is exact (a shared stub would see
// the guest's `this` in its first argument, not a slot number)
#define DEV_STUB_FN(n, name)                                              \
  int TRT_GUEST_ABI dev_stub_##n(void* self) {                            \
    (void)self;                                                           \
    char line[96];                                                        \
    snprintf(line, sizeof(line), "d3d9: device slot %u (%s) called "      \
                                 "(unimplemented)", n, name);             \
    trt_core_log(line);                                                   \
    return D3DERR_NOTIMPL_;                                               \
  }

DEV_STUB_FN(9, "GetCreationParameters")
DEV_STUB_FN(13, "CreateAdditionalSwapChain")
DEV_STUB_FN(14, "GetSwapChain")
DEV_STUB_FN(19, "GetRasterStatus")
DEV_STUB_FN(22, "GetGammaRamp")
DEV_STUB_FN(24, "CreateVolumeTexture")
DEV_STUB_FN(25, "CreateCubeTexture")
DEV_STUB_FN(30, "UpdateSurface")
DEV_STUB_FN(31, "UpdateTexture")
DEV_STUB_FN(32, "GetRenderTargetData")
DEV_STUB_FN(33, "GetFrontBufferData")
DEV_STUB_FN(34, "StretchRect")
DEV_STUB_FN(35, "ColorFill")
DEV_STUB_FN(36, "CreateOffscreenPlainSurface")
DEV_STUB_FN(38, "GetRenderTarget")
DEV_STUB_FN(40, "GetDepthStencilSurface")
DEV_STUB_FN(45, "GetTransform")
DEV_STUB_FN(46, "MultiplyTransform")
DEV_STUB_FN(48, "GetViewport")
DEV_STUB_FN(50, "GetMaterial")
DEV_STUB_FN(52, "GetLight")
DEV_STUB_FN(54, "GetLightEnable")
DEV_STUB_FN(56, "GetClipPlane")
DEV_STUB_FN(59, "CreateStateBlock")
DEV_STUB_FN(60, "BeginStateBlock")
DEV_STUB_FN(61, "EndStateBlock")
DEV_STUB_FN(63, "GetClipStatus")
DEV_STUB_FN(64, "GetTexture")
DEV_STUB_FN(66, "GetTextureStageState")
DEV_STUB_FN(68, "GetSamplerState")
DEV_STUB_FN(72, "GetPaletteEntries")
DEV_STUB_FN(74, "GetCurrentTexturePalette")
DEV_STUB_FN(76, "GetScissorRect")
DEV_STUB_FN(85, "ProcessVertices")
DEV_STUB_FN(88, "GetVertexDeclaration")
DEV_STUB_FN(93, "GetVertexShader")
DEV_STUB_FN(95, "GetVertexShaderConstantF")
DEV_STUB_FN(97, "GetVertexShaderConstantI")
DEV_STUB_FN(99, "GetVertexShaderConstantB")
DEV_STUB_FN(101, "GetStreamSource")
DEV_STUB_FN(103, "GetStreamSourceFreq")
DEV_STUB_FN(105, "GetIndices")
DEV_STUB_FN(108, "GetPixelShader")
DEV_STUB_FN(110, "GetPixelShaderConstantF")
DEV_STUB_FN(112, "GetPixelShaderConstantI")
DEV_STUB_FN(114, "GetPixelShaderConstantB")
DEV_STUB_FN(115, "DrawRectPatch")
DEV_STUB_FN(116, "DrawTriPatch")
DEV_STUB_FN(117, "DeletePatch")
DEV_STUB_FN(118, "CreateQuery")
int TRT_GUEST_ABI dev_SetDialogBoxMode(void*, int) { return D3D_OK_; }
int TRT_GUEST_ABI dev_SetGammaRamp(void*, u32, const void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev_SetMaterial(void*, const void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev_SetLight(void*, u32, const void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev_LightEnable(void*, u32, int) { return D3D_OK_; }
int TRT_GUEST_ABI dev_SetClipPlane(void*, u32, const float*) { return D3D_OK_; }
int TRT_GUEST_ABI dev_SetClipStatus(void*, const void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev_ValidateDevice(void*, u32*) { return D3D_OK_; }
int TRT_GUEST_ABI dev_SetPaletteEntries(void*, u32, const void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev_SetCurrentTexturePalette(void*, u32) { return D3D_OK_; }
int TRT_GUEST_ABI dev_SetScissorRect(void*, const void*) { return D3D_OK_; }
int TRT_GUEST_ABI dev_SetSoftwareVertexProcessing(void*, int) { return D3D_OK_; }
int TRT_GUEST_ABI dev_GetSoftwareVertexProcessing(void*) { return 0; }
void TRT_GUEST_ABI dev_SetNPatchMode(void*, float) {}
float TRT_GUEST_ABI dev_GetNPatchMode(void*) { return 0.f; }
int TRT_GUEST_ABI dev_GetFVF(void*, u32* out) {
  if (out) *out = g_cur_fvf;
  return D3D_OK_;
}
int TRT_GUEST_ABI dev_SetVertexShaderConstantI(void*, u32, const int*, u32) { return D3D_OK_; }
int TRT_GUEST_ABI dev_SetVertexShaderConstantB(void*, u32, const int*, u32) { return D3D_OK_; }
int TRT_GUEST_ABI dev_SetStreamSourceFreq(void*, u32, u32) { return D3D_OK_; }
int TRT_GUEST_ABI dev_SetPixelShaderConstantI(void*, u32, const int*, u32) { return D3D_OK_; }
int TRT_GUEST_ABI dev_SetPixelShaderConstantB(void*, u32, const int*, u32) { return D3D_OK_; }
u32 TRT_GUEST_ABI dev_GetNumberOfSwapChains(void*) { return 0; }

// ---- vtables (SDK order) ----
const void* g_d3d9_vtbl[17] = {
    (const void*)&d9_QI,
    (const void*)&d9_AR,
    (const void*)&d9_Rel,
    (const void*)&d9_RegisterSoftwareDevice,          // 3
    (const void*)&d9_GetAdapterCount,                 // 4
    (const void*)&d9_GetAdapterIdentifier,            // 5
    (const void*)&d9_GetAdapterModeCount,             // 6
    (const void*)&d9_EnumAdapterModes,                // 7
    (const void*)&d9_GetAdapterDisplayMode,           // 8
    (const void*)&d9_CheckDeviceType,                 // 9
    (const void*)&d9_CheckDeviceFormat,               // 10
    (const void*)&d9_CheckDeviceMultiSampleType,      // 11
    (const void*)&d9_CheckDepthStencilMatch,          // 12
    (const void*)&d9_CheckDeviceFormatConversion,     // 13
    (const void*)&d9_GetDeviceCaps,                   // 14
    (const void*)&d9_GetAdapterMonitor,               // 15
    (const void*)&d9_CreateDevice,                    // 16
};

const void* g_dev9_vtbl[119] = {
    (const void*)&d9_QI,                              // 0
    (const void*)&d9_AR,                              // 1
    (const void*)&d9_Rel,                             // 2
    (const void*)&dev_TestCooperativeLevel,           // 3
    (const void*)&dev_GetAvailableTextureMem,         // 4
    (const void*)&dev_EvictManagedResources,          // 5
    (const void*)&dev_GetDirect3D,                    // 6
    (const void*)&dev_GetDeviceCaps,                  // 7
    (const void*)&dev_GetDisplayMode,                 // 8
    (const void*)&dev_stub_9,          // 9
    (const void*)&dev_SetCursorProperties,            // 10
    (const void*)&dev_SetCursorPosition,              // 11
    (const void*)&dev_ShowCursor,                     // 12
    (const void*)&dev_stub_13,      // 13
    (const void*)&dev_stub_14,                   // 14
    (const void*)&dev_GetNumberOfSwapChains,          // 15
    (const void*)&dev_Reset,                          // 16
    (const void*)&dev_Present,                        // 17
    (const void*)&dev_GetBackBuffer,                  // 18
    (const void*)&dev_stub_19,                // 19
    (const void*)&dev_SetDialogBoxMode,               // 20
    (const void*)&dev_SetGammaRamp,                   // 21
    (const void*)&dev_stub_22,                   // 22
    (const void*)&dev_CreateTexture,                  // 23
    (const void*)&dev_stub_24,  // 24 CreateVolumeTexture
    (const void*)&dev_stub_25,  // 25 CreateCubeTexture
    (const void*)&dev_CreateVertexBuffer,             // 26
    (const void*)&dev_CreateIndexBuffer,              // 27
    (const void*)&dev_CreateRenderTarget,             // 28
    (const void*)&dev_CreateDepthStencilSurface,      // 29
    (const void*)&dev_stub_30,                  // 30
    (const void*)&dev_stub_31,                  // 31
    (const void*)&dev_stub_32,            // 32
    (const void*)&dev_stub_33,             // 33
    (const void*)&dev_stub_34,                    // 34
    (const void*)&dev_stub_35,                      // 35
    (const void*)&dev_stub_36,    // 36
    (const void*)&dev_SetRenderTarget,                // 37
    (const void*)&dev_stub_38,                // 38
    (const void*)&dev_SetDepthStencilSurface,         // 39
    (const void*)&dev_stub_40,         // 40
    (const void*)&dev_BeginScene,                     // 41
    (const void*)&dev_EndScene,                       // 42
    (const void*)&dev_Clear,                          // 43
    (const void*)&dev_SetTransform,                   // 44
    (const void*)&dev_stub_45,                   // 45
    (const void*)&dev_stub_46,              // 46
    (const void*)&dev_SetViewport,                    // 47
    (const void*)&dev_stub_48,                    // 48
    (const void*)&dev_SetMaterial,                    // 49
    (const void*)&dev_stub_50,                    // 50
    (const void*)&dev_SetLight,                       // 51
    (const void*)&dev_stub_52,                       // 52
    (const void*)&dev_LightEnable,                    // 53
    (const void*)&dev_stub_54,  // 54 GetLightEnable
    (const void*)&dev_SetClipPlane,                   // 55
    (const void*)&dev_stub_56,                   // 56
    (const void*)&dev_SetRenderState,                 // 57
    (const void*)&dev_GetRenderState,                 // 58
    (const void*)&dev_stub_59,               // 59
    (const void*)&dev_stub_60,                // 60
    (const void*)&dev_stub_61,                  // 61
    (const void*)&dev_SetClipStatus,                  // 62
    (const void*)&dev_stub_63,                  // 63
    (const void*)&dev_stub_64,                     // 64
    (const void*)&dev_SetTexture,                     // 65
    (const void*)&dev_stub_66,           // 66
    (const void*)&dev_SetTextureStageState,           // 67
    (const void*)&dev_stub_68,                // 68
    (const void*)&dev_SetSamplerState,                // 69
    (const void*)&dev_ValidateDevice,                 // 70
    (const void*)&dev_SetPaletteEntries,              // 71
    (const void*)&dev_stub_72,              // 72
    (const void*)&dev_SetCurrentTexturePalette,       // 73
    (const void*)&dev_stub_74,       // 74
    (const void*)&dev_SetScissorRect,                 // 75
    (const void*)&dev_stub_76,                 // 76
    (const void*)&dev_SetSoftwareVertexProcessing,    // 77
    (const void*)&dev_GetSoftwareVertexProcessing,    // 78
    (const void*)&dev_SetNPatchMode,                  // 79
    (const void*)&dev_GetNPatchMode,                  // 80
    (const void*)&dev_DrawPrimitive,                  // 81
    (const void*)&dev_DrawIndexedPrimitive,           // 82
    (const void*)&dev_DrawPrimitiveUP,                // 83
    (const void*)&dev_DrawIndexedPrimitiveUP,         // 84
    (const void*)&dev_stub_85,                // 85
    (const void*)&dev_CreateVertexDeclaration,        // 86
    (const void*)&dev_SetVertexDeclaration,           // 87
    (const void*)&dev_stub_88,           // 88
    (const void*)&dev_SetFVF,                         // 89
    (const void*)&dev_GetFVF,                         // 90
    (const void*)&dev_CreateVertexShader,             // 91
    (const void*)&dev_SetVertexShader,                // 92
    (const void*)&dev_stub_93,                // 93
    (const void*)&dev_SetVertexShaderConstantF,       // 94
    (const void*)&dev_stub_95,       // 95
    (const void*)&dev_SetVertexShaderConstantI,       // 96
    (const void*)&dev_stub_97,       // 97
    (const void*)&dev_SetVertexShaderConstantB,       // 98
    (const void*)&dev_stub_99,       // 99
    (const void*)&dev_SetStreamSource,                // 100
    (const void*)&dev_stub_101,                // 101
    (const void*)&dev_SetStreamSourceFreq,            // 102
    (const void*)&dev_stub_103,            // 103
    (const void*)&dev_SetIndices,                     // 104
    (const void*)&dev_stub_105,                     // 105
    (const void*)&dev_CreatePixelShader,              // 106
    (const void*)&dev_SetPixelShader,                 // 107
    (const void*)&dev_stub_108,                 // 108
    (const void*)&dev_SetPixelShaderConstantF,        // 109
    (const void*)&dev_stub_110,        // 110
    (const void*)&dev_SetPixelShaderConstantI,        // 111
    (const void*)&dev_stub_112,        // 112
    (const void*)&dev_SetPixelShaderConstantB,        // 113
    (const void*)&dev_stub_114,        // 114
    (const void*)&dev_stub_115,                  // 115
    (const void*)&dev_stub_116,                   // 116
    (const void*)&dev_stub_117,                    // 117
    (const void*)&dev_stub_118,                    // 118
};

const void* g_tex9_vtbl[22] = {
    (const void*)&d9_QI, (const void*)&d9_AR, (const void*)&d9_Rel,
    (const void*)&res_GetDevice,          // 3
    (const void*)&res_SetPrivateData,     // 4
    (const void*)&res_GetPrivateData,     // 5
    (const void*)&res_FreePrivateData,    // 6
    (const void*)&res_SetPriority,        // 7
    (const void*)&res_GetPriority,        // 8
    (const void*)&res_PreLoad,            // 9
    (const void*)&res_GetType,            // 10
    (const void*)&tex_SetLOD,             // 11
    (const void*)&tex_GetLOD,             // 12
    (const void*)&tex_GetLevelCount,      // 13
    (const void*)&tex_SetAutoGenFilterType,  // 14
    (const void*)&tex_GetAutoGenFilterType,  // 15
    (const void*)&tex_GenerateMipSubLevels,  // 16
    (const void*)&tex_GetLevelDesc,       // 17
    (const void*)&tex_GetSurfaceLevel,    // 18
    (const void*)&tex_LockRect,           // 19
    (const void*)&tex_UnlockRect,         // 20
    (const void*)&tex_AddDirtyRect,       // 21
};

const void* g_vb9_vtbl[13] = {
    (const void*)&d9_QI, (const void*)&d9_AR, (const void*)&d9_Rel,
    (const void*)&res_GetDevice, (const void*)&res_SetPrivateData,
    (const void*)&res_GetPrivateData, (const void*)&res_FreePrivateData,
    (const void*)&res_SetPriority, (const void*)&res_GetPriority,
    (const void*)&res_PreLoad, (const void*)&res_GetType,
    (const void*)&buf_Lock,    // 11
    (const void*)&buf_Unlock,  // 12
};

const void* g_ib9_vtbl[13] = {
    (const void*)&d9_QI, (const void*)&d9_AR, (const void*)&d9_Rel,
    (const void*)&res_GetDevice, (const void*)&res_SetPrivateData,
    (const void*)&res_GetPrivateData, (const void*)&res_FreePrivateData,
    (const void*)&res_SetPriority, (const void*)&res_GetPriority,
    (const void*)&res_PreLoad, (const void*)&res_GetType,
    (const void*)&buf_Lock, (const void*)&buf_Unlock,
};

const void* g_vs9_vtbl[5] = {
    (const void*)&d9_QI, (const void*)&d9_AR, (const void*)&d9_Rel,
    (const void*)&res_GetDevice,
    (const void*)&shader_GetFunction,  // 4
};
const void* g_ps9_vtbl[5] = {
    (const void*)&d9_QI, (const void*)&d9_AR, (const void*)&d9_Rel,
    (const void*)&res_GetDevice, (const void*)&shader_GetFunction,
};
const void* g_decl9_vtbl[5] = {
    (const void*)&d9_QI, (const void*)&d9_AR, (const void*)&d9_Rel,
    (const void*)&res_GetDevice, (const void*)&shader_GetFunction,
};

const void* g_surf9_vtbl[11] = {
    (const void*)&d9_QI, (const void*)&d9_AR, (const void*)&d9_Rel,
    (const void*)&res_GetDevice, (const void*)&res_SetPrivateData,
    (const void*)&res_GetPrivateData, (const void*)&res_FreePrivateData,
    (const void*)&res_SetPriority, (const void*)&res_GetPriority,
    (const void*)&res_PreLoad, (const void*)&res_GetType,
};

int trt_bridge_register_d3d9(void) {
  static const trt_named_fn k_fns[] = {
      {"Direct3DCreate9", (void*)&trt_Direct3DCreate9},
      {"Direct3DCreate9Ex", (void*)&trt_Direct3DCreate9Ex},
  };
  trt_module_register("d3d9.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  trt_module_register("d3d9core.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

u32 trt_d3d9_last_draw(void) { return g_last_draw; }

void trt_d3d9_set_hooks(const d3d9_hooks* hooks) {
  if (hooks) g_hooks = *hooks;
}
const d3d9_hooks* trt_d3d9_hooks_get(void) { return &g_hooks; }

}  // extern "C"
