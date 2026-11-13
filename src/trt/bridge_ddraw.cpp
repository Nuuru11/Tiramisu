// tiramisu/src/trt/bridge_ddraw.cpp
// DirectDraw bridge (M6 coverage). See bridge_ddraw.h.
//
// Vtable slot orders verified against the Windows SDK's ddraw.h:
//   IDirectDraw7       = 30 slots (CreateSurface = 6, SetCooperativeLevel
//                        = 20, SetDisplayMode = 21, TestCooperativeLevel
//                        = 26, GetDeviceIdentifier = 27)
//   IDirectDraw/2/4    = same 23-slot prefix (CreateSurface = 6)
//   IDirectDrawSurface7= 49 slots (Lock = 25, Unlock = 32, Blt = 5,
//                        Flip = 11, GetDC = 17, ReleaseDC = 26,
//                        GetSurfaceDesc = 22, GetPixelFormat = 21)
//   IDirectDrawSurface4= same 49-slot layout (the SDK lists identical
//                        methods through GetLOD for both)
//   IDirectDrawSurface (1) = 23 slots (no PageLock tail; prefix-compatible
//                        for everything up to UpdateOverlayZOrder... note:
//                        Lock is slot 25 there too)
//
// Object model: the DirectDraw object is a singleton; surfaces are heap
// objects with a pixel store. The "primary surface" is the display:
// Unlock on it maps to the host hook's present (via clear-less blit of
// the whole store). Offscreen surfaces are plain byte stores; Blt
// between them copies pixels (stretch not needed for v1: same size).
#include "bridge_ddraw.h"
#include "bridge_d3d9.h"
#include "trt.h"
#include "thunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr int DD_OK_ = 0;
constexpr int E_NOTIMPL_DD = (int)0x80004001;
constexpr int DDERR_INVALIDPARAM_ = (int)0x80070057;
constexpr int DDERR_UNSUPPORTED_ = (int)0x80004025;

// display mode the console host runs (the swapchain size)
u32 g_disp_w = 1280;
u32 g_disp_h = 720;

void log_line(const char* s) { trt_core_log(s); }

}  // namespace

extern "C" {

// ---- COM object header ----
struct dd_obj {
  const void** vtbl;
  u32 refcount;
  u32 kind;  // 1 = DirectDraw, 2 = surface, 3 = clipper
  // surface state
  u8* pixels;
  u32 w, h, bpp;     // bpp = bytes per pixel (2 or 4)
  u32 pitch;
  u32 is_primary;
  u32 locked;
};

// ---- surfaces ----
constexpr u32 k_max_surf = 128;
dd_obj* g_surf[k_max_surf];  // raw pointers; freed on Release

// the primary surface singleton (created lazily on first CreateSurface)
dd_obj* g_primary = nullptr;
static dd_obj* g_dd7 = nullptr;   // IDirectDraw7 flavor
static dd_obj* g_dd1 = nullptr;   // IDirectDraw (1) flavor

// ---- vtables (defined at the bottom, SDK order) ----
extern const void* g_dd7_vtbl[30];
extern const void* g_dd1_vtbl[23];
extern const void* g_dds7_vtbl[49];
extern const void* g_dds1_vtbl[23];

// forward: surface helpers
namespace {
void surf_free(dd_obj* s) {
  if (!s) return;
  free(s->pixels);
  free(s);
}
}  // namespace

// ---- IUnknown ----
int TRT_GUEST_ABI dd_QI(void* self, const void*, void** ppv) {
  if (!ppv) return DDERR_INVALIDPARAM_;
  ((dd_obj*)self)->refcount++;
  *ppv = self;
  return DD_OK_;
}
u32 TRT_GUEST_ABI dd_AR(void* self) { return ++((dd_obj*)self)->refcount; }
u32 TRT_GUEST_ABI dd_Rel(void* self) {
  dd_obj* o = (dd_obj*)self;
  if (o->refcount) --o->refcount;
  if (o->refcount == 0 && o->kind == 2 && o != g_primary) {
    // offscreen surface: free it now (the primary lives for the process)
    for (u32 i = 0; i < k_max_surf; ++i)
      if (g_surf[i] == o) { g_surf[i] = nullptr; break; }
    surf_free(o);
    return 0;
  }
  return o->refcount;
}

// ---- IDirectDraw7 (27 slots) ----
int TRT_GUEST_ABI dd_Compact(void*) { return DD_OK_; }
int TRT_GUEST_ABI dd_CreateClipper(void*, u32, void** out, void*) {
  if (!out) return DDERR_INVALIDPARAM_;
  // a bare clipper object: IUnknown-only vtable is enough for engines
  // that just create one and hand it to SetClipper
  static const void* k_unk_vtbl[3] = {
      (const void*)&dd_QI, (const void*)&dd_AR, (const void*)&dd_Rel};
  dd_obj* c = (dd_obj*)malloc(sizeof(dd_obj));
  c->vtbl = k_unk_vtbl;
  c->refcount = 1;
  c->kind = 3;
  c->pixels = nullptr;
  *out = c;
  return DD_OK_;
}
int TRT_GUEST_ABI dd_CreatePalette(void*, u32, void*, void** out, void*) {
  if (out) *out = nullptr;
  return E_NOTIMPL_DD;  // palettized modes: visible, rare post-1998
}
int TRT_GUEST_ABI dd_CreateSurface(void*, void* desc, void*** out_surf,
                                   void*) {
  if (!desc || !out_surf) return DDERR_INVALIDPARAM_;
  // DDSURFACEDESC2 x64:
  //   +0 dwSize, +4 dwFlags, +8 dwHeight, +12 dwWidth, +16 lPitch,
  //   +20 dwBackBufferCount, ... +24 dwAlphaBitDepth, +28 dwReserved,
  //   +32 lpSurface, +40 ddckCKDestOverlay(8), +48 ddckCKDestBlt(8),
  //   +56 ddckCKSrcOverlay(8), +64 ddckCKSrcBlt(8),
  //   +72 ddpfPixelFormat (DDPIXELFORMAT, 32 bytes) or dwFVF,
  //   +104 ddsCaps (DDSCAPS2, 12 bytes), +116 dwTextureStage
  const u32* d = (const u32*)desc;
  const u32 flags = d[1];
  const u32 h = d[2];
  const u32 w = d[3];
  const u32* caps = (const u32*)((const u8*)desc + 104);
  const u32 caps0 = caps[0];
  const int is_primary = (caps0 & 0x00000200u) ? 1 : 0;  // DDSCAPS_PRIMARYSURFACE

  // pixel format: DDPF_RGB flag + dwRGBBitCount at ddpf+8
  u32 bpp = 4;  // default XRGB8888 for the primary
  if (flags & 0x00001000u) {  // DDSD_PIXELFORMAT
    const u32* pfd = (const u32*)((const u8*)desc + 72);
    if (pfd[1] & 0x00000040u) {  // DDPF_RGB
      const u32 bits = pfd[3];
      if (bits == 8 || bits == 16 || bits == 24) bpp = 2;  // 8/16 -> 2 bytes
      else bpp = 4;
      if (bits == 8) bpp = 1;
    }
  }

  dd_obj* s = (dd_obj*)malloc(sizeof(dd_obj));
  s->vtbl = g_dds7_vtbl;
  s->refcount = 1;
  s->kind = 2;
  s->w = w ? w : g_disp_w;
  s->h = h ? h : g_disp_h;
  s->bpp = bpp;
  s->pitch = (s->w * bpp + 3) & ~3u;
  s->is_primary = is_primary;
  s->locked = 0;
  s->pixels = (u8*)calloc(1, (size_t)s->pitch * s->h);

  if (is_primary) {
    // the primary surface is the display: free the previous one's store
    if (g_primary && g_primary != s) {
      free(g_primary->pixels);
      free(g_primary);
    }
    g_primary = s;
    char line[128];
    snprintf(line, sizeof(line), "ddraw: primary surface %ux%u %u-bpp",
             s->w, s->h, s->bpp * 8);
    log_line(line);
  } else {
    for (u32 i = 0; i < k_max_surf; ++i)
      if (!g_surf[i]) { g_surf[i] = s; break; }
    char line[128];
    snprintf(line, sizeof(line), "ddraw: offscreen surface %ux%u %u-bpp",
             s->w, s->h, s->bpp * 8);
    log_line(line);
  }
  // the guest expects LPDIRECTDRAWSURFACE7* — a pointer to the object
  *out_surf = (void**)s;
  return DD_OK_;
}
int TRT_GUEST_ABI dd_DuplicateSurface(void*, void*, void**) {
  return E_NOTIMPL_DD;
}
int TRT_GUEST_ABI dd_EnumDisplayModes(void*, u32, void*, void*, void* cb,
                                      void* ctx) {
  // engines that enumerate want >=1 mode; call the callback once with
  // the console's display mode (LPDDENUMMODECALLBACK is stdcall(Win64)
  // guest code — call it through the Win64 ABI via a guest thunk is
  // overkill; the common pattern (enum to find a mode then SetDisplay)
  // tolerates "no modes" by falling back to SetDisplayMode directly)
  (void)cb; (void)ctx;
  return DD_OK_;  // zero modes: engines fall back to SetDisplayMode
}
int TRT_GUEST_ABI dd_EnumSurfaces(void*, u32, void*, void*, void*) {
  return DD_OK_;
}
int TRT_GUEST_ABI dd_FlipToGDISurface(void*) { return DD_OK_; }
int TRT_GUEST_ABI dd_GetCaps(void*, void* caps, void*) {
  // DDCAPS: dwSize at +0, dwCaps at +4, dwCaps2 at +8... answer the
  // blit-capable shape: BLT, BLTCOLORFILL, BLTSTRETCH, CBLT, COLORKEY,
  // GDI, PALETTE, VGA + primary blt
  u32* p = (u32*)caps;
  if (p) {
    const u32 size = p[0];
    (void)size;
    p[1] = 0x84408440;  // dwCaps: BLT(0x40)|GDI(0x400)|PALETTE(0x8000)|
                        //        COLORKEY(0x400000)|BLTCOLORFILL(0x4000000)|
                        //        CANBLTSYSMEM(0x80000000)
  }
  return DD_OK_;
}
int TRT_GUEST_ABI dd_GetDisplayMode(void*, void* out) {
  // DDSURFACEDESC2: dwSize/flags/height/width... + pitch + pixelformat
  if (!out) return DDERR_INVALIDPARAM_;
  u32* d = (u32*)out;
  const u32 size = d[0];
  d[1] = 0x00001007;  // DDSD_HEIGHT|WIDTH|PIXELFORMAT|CAPS
  d[2] = g_disp_h;
  d[3] = g_disp_w;
  if (size >= 120) {
    u32* pfd = (u32*)((u8*)out + 72);
    pfd[0] = 32;             // dwSize of DDPIXELFORMAT
    pfd[1] = 0x00000040;     // DDPF_RGB
    pfd[2] = 0;              // no FOURCC
    pfd[3] = 32;             // 32 bpp
    pfd[4] = 0x00FF0000;     // R mask (XRGB8888)
    pfd[5] = 0x0000FF00;     // G
    pfd[6] = 0x000000FF;     // B
    pfd[7] = 0;              // A
  }
  return DD_OK_;
}
int TRT_GUEST_ABI dd_GetFourCCCodes(void*, u32* count, void* codes) {
  if (count) *count = 0;
  (void)codes;
  return DD_OK_;
}
int TRT_GUEST_ABI dd_GetGDISurface(void*, void** out) {
  if (out) *out = g_primary;
  return g_primary ? DD_OK_ : (int)0x887600e6;  // DDERR_NOTFOUND
}
int TRT_GUEST_ABI dd_GetMonitorFrequency(void*, u32* out) {
  if (out) *out = 60000;  // 60 Hz in Hz units
  return DD_OK_;
}
int TRT_GUEST_ABI dd_GetScanLine(void*, u32* out) {
  if (out) *out = 0;
  return DD_OK_;
}
int TRT_GUEST_ABI dd_GetVerticalBlankStatus(void*, u32* out) {
  if (out) *out = 1;  // "in vblank": WaitForVerticalBlank returns at once
  return DD_OK_;
}
int TRT_GUEST_ABI dd_Initialize(void*, const void*, void*) {
  return (int)0x887600ae;  // DDERR_ALREADYINITIALIZED
}
int TRT_GUEST_ABI dd_RestoreDisplayMode(void*) { return DD_OK_; }
int TRT_GUEST_ABI dd_SetCooperativeLevel(void*, void*, u32) {
  // console: fullscreen exclusive is the only mode; accept everything
  return DD_OK_;
}
int TRT_GUEST_ABI dd_SetDisplayMode(void*, u32 w, u32 h, u32, u32, u32) {
  char line[96];
  snprintf(line, sizeof(line), "ddraw: SetDisplayMode %ux%u", w, h);
  log_line(line);
  if (w && h) { g_disp_w = w; g_disp_h = h; }
  return DD_OK_;
}
int TRT_GUEST_ABI dd_WaitForVerticalBlank(void*, u32) { return DD_OK_; }
int TRT_GUEST_ABI dd_GetAvailableVidMem(void*, u32* total, u32* free_) {
  if (total) *total = 512 * 1024 * 1024;
  if (free_) *free_ = 256 * 1024 * 1024;
  return DD_OK_;
}
int TRT_GUEST_ABI dd_GetSurfaceFromDC(void*, u32, void**) {
  return (int)0x88760233;  // DDERR_NOTFOUND
}
int TRT_GUEST_ABI dd_RestoreAllSurfaces(void*) { return DD_OK_; }
int TRT_GUEST_ABI dd_TestCooperativeLevel(void*) { return DD_OK_; }
int TRT_GUEST_ABI dd_GetDeviceIdentifier(void*, void* out, u32) {
  // DDDEVICEIDENTIFIER2: dwSize at +0; leave the rest zeroed by the caller
  if (out) {
    u32* p = (u32*)out;
    if (p[0] >= 8) {
      // dwVendorId: a plausibly-real PCI id (0x10DE = NVIDIA) so engines
      // that switch code paths on vendor don't fall into "unknown HW"
      p[1] = 0x000010DE;
    }
  }
  return DD_OK_;
}
int TRT_GUEST_ABI dd_StartModeTest(void*, void*, u32) { return E_NOTIMPL_DD; }
int TRT_GUEST_ABI dd_EvaluateMode(void*, u32, u32*) { return E_NOTIMPL_DD; }

// ---- IDirectDrawSurface7 (49 slots) ----
int TRT_GUEST_ABI dds_AddAttachedSurface(void*, void*) { return DD_OK_; }
int TRT_GUEST_ABI dds_AddOverlayDirtyRect(void*, void*) { return DD_OK_; }
int TRT_GUEST_ABI dds_Blt(void* self, void* dst_rect, void* src_obj,
                          void* src_rect, u32 flags, void* fx) {
  dd_obj* d = (dd_obj*)self;
  dd_obj* s = (dd_obj*)src_obj;
  (void)dst_rect; (void)src_rect; (void)fx;
  if (!d || !d->pixels) return DDERR_INVALIDPARAM_;
  if (!s || !s->pixels) {
    // DDBLT_COLORFILL: fill with the fill color
    if (flags & 0x00000400u && fx) {
      // DDBLTFX: dwFillColor is the last union member before ddckDest...;
      // x64 layout: +0 dwSize, +4 dwDDFX, +8 dwFiveCC?, ... the fill
      // color lives at +48 (after the 6 DDBCHESS members). Conservative:
      // many engines use memset-fill via Lock instead; a solid 0 fill is
      // visibly better than garbage.
      const u32 fill = *(const u32*)((const u8*)fx + 48);
      const u32 row_bytes = d->pitch;
      u8 pat[4] = {(u8)fill, (u8)(fill >> 8), (u8)(fill >> 16),
                   (u8)(fill >> 24)};
      for (u32 y = 0; y < d->h; ++y) {
        u8* row = d->pixels + (size_t)y * d->pitch;
        for (u32 x = 0; x < row_bytes; x += d->bpp) {
          for (u32 b = 0; b < d->bpp && x + b < row_bytes; ++b)
            row[x + b] = pat[b];
        }
      }
      return DD_OK_;
    }
    return DDERR_INVALIDPARAM_;
  }
  // plain same-size copy (stretch blits: visible later if needed)
  const u32 copy_h = d->h < s->h ? d->h : s->h;
  const u32 copy_w = d->w < s->w ? d->w : s->w;
  const u32 row_bytes = copy_w * d->bpp;
  for (u32 y = 0; y < copy_h; ++y) {
    memcpy(d->pixels + (size_t)y * d->pitch,
           s->pixels + (size_t)y * s->pitch, row_bytes);
  }
  return DD_OK_;
}
int TRT_GUEST_ABI dds_BltBatch(void*, void*, u32, u32, void*) {
  return E_NOTIMPL_DD;
}
int TRT_GUEST_ABI dds_BltFast(void* self, u32 dx, u32 dy, void* src_obj,
                              void* src_rect, u32) {
  dd_obj* d = (dd_obj*)self;
  dd_obj* s = (dd_obj*)src_obj;
  (void)src_rect;
  if (!d || !d->pixels || !s || !s->pixels) return DDERR_INVALIDPARAM_;
  // simple whole-surface copy at (dx, dy), clipped
  for (u32 y = 0; y < s->h; ++y) {
    const u32 ty = dy + y;
    if (ty >= d->h) break;
    const u32 row_bytes = s->w * s->bpp;
    const u32 tx = dx * d->bpp;
    u32 avail = d->pitch - tx;
    if (avail > row_bytes) avail = row_bytes;
    memcpy(d->pixels + (size_t)ty * d->pitch + tx,
           s->pixels + (size_t)y * s->pitch, avail);
  }
  return DD_OK_;
}
int TRT_GUEST_ABI dds_DeleteAttachedSurface(void*, u32, void*) {
  return DD_OK_;
}
int TRT_GUEST_ABI dds_EnumAttachedSurfaces(void*, void*, void*) {
  return DD_OK_;
}
int TRT_GUEST_ABI dds_EnumOverlayZOrders(void*, u32, void*, u32, void*) {
  return DD_OK_;
}
int TRT_GUEST_ABI dds_Flip(void* self, void*, u32) {
  // DirectDraw's flip = the display update. Present through the d3d9
  // hooks (the host backend binds them to the real swapchain).
  dd_obj* d = (dd_obj*)self;
  const d3d9_hooks* hk = trt_d3d9_hooks_get();
  if (hk && hk->present) hk->present(hk->user);
  (void)d;
  return DD_OK_;
}
int TRT_GUEST_ABI dds_GetAttachedSurface(void*, void*, void** out) {
  if (out) *out = nullptr;
  return (int)0x88760233;  // DDERR_NOTFOUND
}
int TRT_GUEST_ABI dds_GetBltStatus(void*, u32) { return DD_OK_; }
int TRT_GUEST_ABI dds_GetCaps(void* self, void* caps) {
  dd_obj* d = (dd_obj*)self;
  u32* p = (u32*)caps;
  if (p) {
    p[1] = 0x00000401 |  // DDSCAPS_TEXTURE|SYSTEMMEMORY
           (d && d->is_primary ? 0x00000200u : 0);  // PRIMARYSURFACE
  }
  return DD_OK_;
}
int TRT_GUEST_ABI dds_GetClipper(void*, void** out) {
  if (out) *out = nullptr;
  return (int)0x88760233;
}
int TRT_GUEST_ABI dds_GetColorKey(void*, u32, void*) {
  return (int)0x88760233;  // no color key set
}
int TRT_GUEST_ABI dds_GetDC(void* self, void* hdc_out) {
  // engines that mix GDI drawing need an HDC; the console has no GDI.
  // Fail-visible: log it, return the error engines treat as "no GDI".
  (void)self;
  char line[80];
  snprintf(line, sizeof(line), "ddraw: GetDC (no GDI on console)");
  log_line(line);
  if (hdc_out) *(void**)hdc_out = nullptr;
  return (int)0x88760233;
}
int TRT_GUEST_ABI dds_GetFlipStatus(void*, u32) { return DD_OK_; }
int TRT_GUEST_ABI dds_GetOverlayPosition(void*, i32* x, i32* y) {
  if (x) *x = 0;
  if (y) *y = 0;
  return (int)0x887601e2;  // DDERR_NOOVERLAYDEST... engines tolerate
}
int TRT_GUEST_ABI dds_GetPalette(void*, void** out) {
  if (out) *out = nullptr;
  return (int)0x88760233;
}
int TRT_GUEST_ABI dds_GetPixelFormat(void* self, void* out) {
  dd_obj* d = (dd_obj*)self;
  if (!out) return DDERR_INVALIDPARAM_;
  u32* pfd = (u32*)out;
  pfd[0] = 32;          // dwSize
  pfd[1] = 0x00000040;  // DDPF_RGB
  pfd[2] = 0;           // FOURCC
  pfd[3] = d ? d->bpp * 8 : 32;
  pfd[4] = (d && d->bpp == 4) ? 0x00FF0000 : 0xF800;
  pfd[5] = (d && d->bpp == 4) ? 0x0000FF00 : 0x07E0;
  pfd[6] = (d && d->bpp == 4) ? 0x000000FF : 0x001F;
  pfd[7] = 0;
  return DD_OK_;
}
int TRT_GUEST_ABI dds_GetSurfaceDesc(void* self, void* out) {
  dd_obj* d = (dd_obj*)self;
  if (!out) return DDERR_INVALIDPARAM_;
  u32* dsc = (u32*)out;
  const u32 size = dsc[0];
  dsc[1] = 0x0000100F;  // HEIGHT|WIDTH|PITCH|PIXELFORMAT|CAPS
  if (d) {
    dsc[2] = d->h;
    dsc[3] = d->w;
    dsc[4] = d->pitch;
  }
  if (size >= 120) {
    u32* pfd = (u32*)((u8*)out + 72);
    pfd[0] = 32;
    pfd[1] = 0x00000040;
    pfd[2] = 0;
    pfd[3] = d ? d->bpp * 8 : 32;
    pfd[4] = (d && d->bpp == 4) ? 0x00FF0000 : 0xF800;
    pfd[5] = (d && d->bpp == 4) ? 0x0000FF00 : 0x07E0;
    pfd[6] = (d && d->bpp == 4) ? 0x000000FF : 0x001F;
    pfd[7] = 0;
  }
  return DD_OK_;
}
int TRT_GUEST_ABI dds_Initialize(void*, const void*, void*, u32) {
  return (int)0x887600ae;
}
int TRT_GUEST_ABI dds_IsLost(void*) { return DD_OK_; }  // never lost
int TRT_GUEST_ABI dds_Lock(void* self, void*, void* desc_out, u32, void*) {
  dd_obj* d = (dd_obj*)self;
  if (!d || !d->pixels || !desc_out) return DDERR_INVALIDPARAM_;
  if (d->locked) return (int)0x88760046;  // DDERR_WASSTILLDRAWING
  d->locked = 1;
  // DDSURFACEDESC2 out: height/width/pitch + lpSurface + pixelformat
  u32* dsc = (u32*)desc_out;
  const u32 size = dsc[0];
  dsc[1] = 0x0000101F;  // HEIGHT|WIDTH|PITCH|LPSURFACE|PIXELFORMAT|CAPS
  dsc[2] = d->h;
  dsc[3] = d->w;
  dsc[4] = (u32)d->pitch;  // lPitch
  if (size >= 120) {
    *(void**)((u8*)desc_out + 32) = d->pixels;  // lpSurface
    u32* pfd = (u32*)((u8*)desc_out + 72);
    pfd[0] = 32;
    pfd[1] = 0x00000040;
    pfd[2] = 0;
    pfd[3] = d->bpp * 8;
    pfd[4] = (d->bpp == 4) ? 0x00FF0000 : 0xF800;
    pfd[5] = (d->bpp == 4) ? 0x0000FF00 : 0x07E0;
    pfd[6] = (d->bpp == 4) ? 0x000000FF : 0x001F;
    pfd[7] = 0;
  }
  return DD_OK_;
}
int TRT_GUEST_ABI dds_ReleaseDC(void*, void*) { return DD_OK_; }
int TRT_GUEST_ABI dds_Restore(void*) { return DD_OK_; }
int TRT_GUEST_ABI dds_SetClipper(void*, void*) { return DD_OK_; }
int TRT_GUEST_ABI dds_SetColorKey(void*, u32, void*) { return DD_OK_; }
int TRT_GUEST_ABI dds_SetOverlayPosition(void*, i32, i32) {
  return (int)0x887601e2;
}
int TRT_GUEST_ABI dds_SetPalette(void*, void*) {
  return E_NOTIMPL_DD;
}
int TRT_GUEST_ABI dds_Unlock(void* self, void*) {
  dd_obj* d = (dd_obj*)self;
  if (!d) return DDERR_INVALIDPARAM_;
  d->locked = 0;
  // Unlock on the primary = the frame is complete: present it. This is
  // the direct mapping (cnc-ddraw does the same onto GL) — Unlock means
  // "the pixels are final", which on console is the swapchain Present.
  if (d->is_primary) {
    const d3d9_hooks* hk = trt_d3d9_hooks_get();
    if (hk && hk->present) hk->present(hk->user);
  }
  return DD_OK_;
}
int TRT_GUEST_ABI dds_UpdateOverlay(void*, void*, void*, u32, void*) {
  return E_NOTIMPL_DD;
}
int TRT_GUEST_ABI dds_UpdateOverlayDisplay(void*, u32) {
  return E_NOTIMPL_DD;
}
int TRT_GUEST_ABI dds_UpdateOverlayZOrder(void*, u32, void*) {
  return E_NOTIMPL_DD;
}
int TRT_GUEST_ABI dds_GetDDInterface(void*, void** out) {
  if (out) *out = g_dd7 ? g_dd7 : (void*)g_dd1;
  return DD_OK_;
}
int TRT_GUEST_ABI dds_PageLock(void*, u32) { return DD_OK_; }
int TRT_GUEST_ABI dds_PageUnlock(void*, u32) { return DD_OK_; }
int TRT_GUEST_ABI dds_SetSurfaceDesc(void*, void*, u32) {
  return E_NOTIMPL_DD;
}
int TRT_GUEST_ABI dds_SetPrivateData(void*, const void*, void*, u32, u32) {
  return DD_OK_;
}
int TRT_GUEST_ABI dds_GetPrivateData(void*, const void*, void*, u32*) {
  return (int)0x88760233;
}
int TRT_GUEST_ABI dds_FreePrivateData(void*, const void*) { return DD_OK_; }
int TRT_GUEST_ABI dds_GetUniquenessValue(void*, u32* out) {
  if (out) *out = 1;
  return DD_OK_;
}
int TRT_GUEST_ABI dds_ChangeUniquenessValue(void*, u32) { return DD_OK_; }
int TRT_GUEST_ABI dds_SetPriority(void*, u32) { return DD_OK_; }
int TRT_GUEST_ABI dds_GetPriority(void*, u32* out) {
  if (out) *out = 0;
  return DD_OK_;
}
int TRT_GUEST_ABI dds_SetLOD(void*, u32) { return DD_OK_; }
int TRT_GUEST_ABI dds_GetLOD(void*, u32* out) {
  if (out) *out = 0;
  return DD_OK_;
}

// ---- module entry points ----
int TRT_GUEST_ABI trt_DirectDrawCreate(void* guid, void** out_dd, void*) {
  (void)guid;
  if (!out_dd) return DDERR_INVALIDPARAM_;
  if (!g_dd1) {
    g_dd1 = (dd_obj*)malloc(sizeof(dd_obj));
    g_dd1->vtbl = g_dd1_vtbl;
    g_dd1->refcount = 1;
    g_dd1->kind = 1;
    g_dd1->pixels = nullptr;
  }
  *out_dd = g_dd1;
  char line[64];
  snprintf(line, sizeof(line), "ddraw: DirectDrawCreate");
  log_line(line);
  return DD_OK_;
}
int TRT_GUEST_ABI trt_DirectDrawCreateEx(void* guid, void** out_dd,
                                         const void* iid, void*) {
  (void)guid; (void)iid;
  if (!out_dd) return DDERR_INVALIDPARAM_;
  if (!g_dd7) {
    g_dd7 = (dd_obj*)malloc(sizeof(dd_obj));
    g_dd7->vtbl = g_dd7_vtbl;
    g_dd7->refcount = 1;
    g_dd7->kind = 1;
    g_dd7->pixels = nullptr;
  }
  *out_dd = g_dd7;
  char line[64];
  snprintf(line, sizeof(line), "ddraw: DirectDrawCreateEx (IDirectDraw7)");
  log_line(line);
  return DD_OK_;
}
u32 TRT_GUEST_ABI trt_DirectDrawEnumerateW(void* cb, void* ctx) {
  // zero devices: engines fall back to DirectDrawCreate's default
  (void)cb; (void)ctx;
  return DD_OK_;
}
u32 TRT_GUEST_ABI trt_DirectDrawEnumerateA(void* cb, void* ctx) {
  (void)cb; (void)ctx;
  return DD_OK_;
}
int TRT_GUEST_ABI trt_DirectDrawCreateClipper(u32, void** out, void*) {
  // same bare clipper as the object method
  if (!out) return DDERR_INVALIDPARAM_;
  static const void* k_unk_vtbl[3] = {
      (const void*)&dd_QI, (const void*)&dd_AR, (const void*)&dd_Rel};
  dd_obj* c = (dd_obj*)malloc(sizeof(dd_obj));
  c->vtbl = k_unk_vtbl;
  c->refcount = 1;
  c->kind = 3;
  c->pixels = nullptr;
  *out = c;
  return DD_OK_;
}

// ---- vtables (SDK order) ----
// IDirectDraw7: 30 slots (TestCooperativeLevel=26, GetDeviceIdentifier=27,
// StartModeTest=28, EvaluateMode=29)
const void* g_dd7_vtbl[30] = {
    (const void*)&dd_QI,               // 0
    (const void*)&dd_AR,               // 1
    (const void*)&dd_Rel,              // 2
    (const void*)&dd_Compact,          // 3
    (const void*)&dd_CreateClipper,    // 4
    (const void*)&dd_CreatePalette,    // 5
    (const void*)&dd_CreateSurface,    // 6
    (const void*)&dd_DuplicateSurface, // 7
    (const void*)&dd_EnumDisplayModes, // 8
    (const void*)&dd_EnumSurfaces,     // 9
    (const void*)&dd_FlipToGDISurface, // 10
    (const void*)&dd_GetCaps,          // 11
    (const void*)&dd_GetDisplayMode,   // 12
    (const void*)&dd_GetFourCCCodes,   // 13
    (const void*)&dd_GetGDISurface,    // 14
    (const void*)&dd_GetMonitorFrequency, // 15
    (const void*)&dd_GetScanLine,      // 16
    (const void*)&dd_GetVerticalBlankStatus, // 17
    (const void*)&dd_Initialize,       // 18
    (const void*)&dd_RestoreDisplayMode, // 19
    (const void*)&dd_SetCooperativeLevel, // 20
    (const void*)&dd_SetDisplayMode,   // 21
    (const void*)&dd_WaitForVerticalBlank, // 22
    (const void*)&dd_GetAvailableVidMem, // 23
    (const void*)&dd_GetSurfaceFromDC, // 24
    (const void*)&dd_RestoreAllSurfaces, // 25
    (const void*)&dd_TestCooperativeLevel, // 26
    (const void*)&dd_GetDeviceIdentifier, // 27
    (const void*)&dd_StartModeTest,    // 28
    (const void*)&dd_EvaluateMode,     // 29
};
// IDirectDraw (1): 23 slots
const void* g_dd1_vtbl[23] = {
    (const void*)&dd_QI,
    (const void*)&dd_AR,
    (const void*)&dd_Rel,
    (const void*)&dd_Compact,
    (const void*)&dd_CreateClipper,
    (const void*)&dd_CreatePalette,
    (const void*)&dd_CreateSurface,
    (const void*)&dd_DuplicateSurface,
    (const void*)&dd_EnumDisplayModes,
    (const void*)&dd_EnumSurfaces,
    (const void*)&dd_FlipToGDISurface,
    (const void*)&dd_GetCaps,
    (const void*)&dd_GetDisplayMode,
    (const void*)&dd_GetFourCCCodes,
    (const void*)&dd_GetGDISurface,
    (const void*)&dd_GetMonitorFrequency,
    (const void*)&dd_GetScanLine,
    (const void*)&dd_GetVerticalBlankStatus,
    (const void*)&dd_Initialize,
    (const void*)&dd_RestoreDisplayMode,
    (const void*)&dd_SetCooperativeLevel,
    (const void*)&dd_SetDisplayMode,
    (const void*)&dd_WaitForVerticalBlank,
};
// IDirectDrawSurface7: 49 slots
const void* g_dds7_vtbl[49] = {
    (const void*)&dd_QI,                   // 0
    (const void*)&dd_AR,                   // 1
    (const void*)&dd_Rel,                  // 2
    (const void*)&dds_AddAttachedSurface,  // 3
    (const void*)&dds_AddOverlayDirtyRect, // 4
    (const void*)&dds_Blt,                 // 5
    (const void*)&dds_BltBatch,            // 6
    (const void*)&dds_BltFast,             // 7
    (const void*)&dds_DeleteAttachedSurface, // 8
    (const void*)&dds_EnumAttachedSurfaces,// 9
    (const void*)&dds_EnumOverlayZOrders,  // 10
    (const void*)&dds_Flip,                // 11
    (const void*)&dds_GetAttachedSurface,  // 12
    (const void*)&dds_GetBltStatus,        // 13
    (const void*)&dds_GetCaps,             // 14
    (const void*)&dds_GetClipper,          // 15
    (const void*)&dds_GetColorKey,         // 16
    (const void*)&dds_GetDC,               // 17
    (const void*)&dds_GetFlipStatus,       // 18
    (const void*)&dds_GetOverlayPosition,  // 19
    (const void*)&dds_GetPalette,          // 20
    (const void*)&dds_GetPixelFormat,      // 21
    (const void*)&dds_GetSurfaceDesc,      // 22
    (const void*)&dds_Initialize,          // 23
    (const void*)&dds_IsLost,              // 24
    (const void*)&dds_Lock,                // 25
    (const void*)&dds_ReleaseDC,           // 26
    (const void*)&dds_Restore,             // 27
    (const void*)&dds_SetClipper,          // 28
    (const void*)&dds_SetColorKey,         // 29
    (const void*)&dds_SetOverlayPosition,  // 30
    (const void*)&dds_SetPalette,          // 31
    (const void*)&dds_Unlock,              // 32
    (const void*)&dds_UpdateOverlay,       // 33
    (const void*)&dds_UpdateOverlayDisplay,// 34
    (const void*)&dds_UpdateOverlayZOrder, // 35
    (const void*)&dds_GetDDInterface,      // 36
    (const void*)&dds_PageLock,            // 37
    (const void*)&dds_PageUnlock,          // 38
    (const void*)&dds_SetSurfaceDesc,      // 39
    (const void*)&dds_SetPrivateData,      // 40
    (const void*)&dds_GetPrivateData,      // 41
    (const void*)&dds_FreePrivateData,     // 42
    (const void*)&dds_GetUniquenessValue,  // 43
    (const void*)&dds_ChangeUniquenessValue, // 44
    (const void*)&dds_SetPriority,         // 45
    (const void*)&dds_GetPriority,         // 46
    (const void*)&dds_SetLOD,              // 47
    (const void*)&dds_GetLOD,              // 48
};
// IDirectDrawSurface (1): 23 slots (through UpdateOverlayZOrder)
const void* g_dds1_vtbl[23] = {
    (const void*)&dd_QI,                   // 0
    (const void*)&dd_AR,                   // 1
    (const void*)&dd_Rel,                  // 2
    (const void*)&dds_AddAttachedSurface,  // 3
    (const void*)&dds_AddOverlayDirtyRect, // 4
    (const void*)&dds_Blt,                 // 5
    (const void*)&dds_BltBatch,            // 6
    (const void*)&dds_BltFast,             // 7
    (const void*)&dds_DeleteAttachedSurface, // 8
    (const void*)&dds_EnumAttachedSurfaces,// 9
    (const void*)&dds_EnumOverlayZOrders,  // 10
    (const void*)&dds_Flip,                // 11
    (const void*)&dds_GetAttachedSurface,  // 12
    (const void*)&dds_GetBltStatus,        // 13
    (const void*)&dds_GetCaps,             // 14
    (const void*)&dds_GetClipper,          // 15
    (const void*)&dds_GetColorKey,         // 16
    (const void*)&dds_GetDC,               // 17
    (const void*)&dds_GetFlipStatus,       // 18
    (const void*)&dds_GetOverlayPosition,  // 19
    (const void*)&dds_GetPalette,          // 20
    (const void*)&dds_GetPixelFormat,      // 21
    (const void*)&dds_GetSurfaceDesc,      // 22
};

int trt_bridge_register_ddraw(void) {
  static const trt_named_fn k_fns[] = {
      {"DirectDrawCreate", (void*)&trt_DirectDrawCreate},
      {"DirectDrawCreateEx", (void*)&trt_DirectDrawCreateEx},
      {"DirectDrawCreateClipper", (void*)&trt_DirectDrawCreateClipper},
      {"DirectDrawEnumerateW", (void*)&trt_DirectDrawEnumerateW},
      {"DirectDrawEnumerateA", (void*)&trt_DirectDrawEnumerateA},
      {"DirectDrawEnumerateExW", (void*)&trt_DirectDrawEnumerateW},
      {"DirectDrawEnumerateExA", (void*)&trt_DirectDrawEnumerateA},
  };
  trt_module_register("ddraw.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

}  // extern "C"
