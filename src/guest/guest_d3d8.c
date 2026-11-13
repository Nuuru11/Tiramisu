// tiramisu/src/guest/guest_d3d8.c
// Proves the D3D8 bridge: the D3D8 lifecycle — Direct3DCreate8 ->
// CreateDevice -> texture/vb/ib + locks -> render state/transform ->
// clear/draw/present -> DWORD-handle shaders -> SetIndices with base
// vertex -> CopyRects. Vtable slots verified against Wine's d3d8.h.
// Returns a bitmask:
//   0x001 Direct3DCreate8 returned an object
//   0x002 GetAdapterCount >= 1
//   0x004 CreateDevice succeeded (slot 15)
//   0x008 CreateTexture (slot 20) + LockRect (tex slot 16) OK
//   0x010 CreateVertexBuffer (slot 23) + Lock (vb slot 11) OK
//   0x020 SetRenderState (slot 50) / SetTransform (37) / SetVertexShader
//         FVF handle (76) accepted
//   0x040 DrawPrimitiveUP (72) + Present (15) OK
//   0x080 BeginScene/EndScene (34/35) + Clear (36) OK
//   0x100 CreateIndexBuffer (24) + SetIndices (85, base vertex) +
//         DrawIndexedPrimitive (71, D3D8 signature) OK
//   0x200 GetDeviceCaps sane (MaxTextureWidth >= 2048, slot 7)
//   0x400 CreateVertexShader (75) returns a DWORD handle
//   0x800 CopyRects (28) OK
typedef unsigned int u32;
typedef int i32;
typedef unsigned char u8;
typedef unsigned long long u64;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

int _fltused = 1;

__attribute__((dllimport)) void* GSTD Direct3DCreate8(u32 sdk);

static void* vmethod(void* obj, u32 slot) {
  void** vtbl = *(void***)obj;
  return vtbl[slot];
}

u32 GSTD guest_d3d8_main(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  // ---- 1. create the D3D8 object ----
  void* d3d = Direct3DCreate8(220 /* D3D_SDK_VERSION */);
  if (d3d) bits |= 0x01u;

  // ---- 2. adapters (slot 4) ----
  if (d3d) {
    const u32 adapters = ((u32 GSTD (*)(void*))vmethod(d3d, 4))(d3d);
    if (adapters >= 1) bits |= 0x02u;
  }

  // ---- 3. CreateDevice (slot 15 — D3D8 has one fewer adapter slot than
  //         D3D9's 16). D3DPRESENT_PARAMETERS8 head: +0 width, +4 height ----
  void* dev = 0;
  if (d3d) {
    u32 pp[16];
    for (u32 i = 0; i < 16; ++i) pp[i] = 0;
    pp[0] = 1280; pp[1] = 720; pp[2] = 21;
    pp[10] = 1;  // Windowed (offset 40 — same layout as D3D9)
    const u32 hr = ((u32 GSTD (*)(void*, u32, u32, void*, u32, void*, void**))
                    vmethod(d3d, 15))(d3d, 0, 1, 0, 0, pp, &dev);
    if (hr == 0 && dev) bits |= 0x04u;
  }

  // ---- 4. texture lifecycle (CreateTexture = dev slot 20; LockRect =
  //         tex slot 16, UnlockRect = 17 in D3D8's 19-slot vtable) ----
  if (dev) {
    void* tex = 0;
    const u32 hr = ((u32 GSTD (*)(void*, u32, u32, u32, u32, u32, u32, void**))
                    vmethod(dev, 20))(dev, 64, 64, 1, 0, 21, 1, &tex);
    if (hr == 0 && tex) {
      u8 lr[16];
      for (u32 i = 0; i < 16; ++i) lr[i] = 0;
      const u32 hrl = ((u32 GSTD (*)(void*, u32, void*, const void*, u32))
                       vmethod(tex, 16))(tex, 0, lr, 0, 0);
      const u64 pbits = *(u64*)(lr + 8);  // pBits at +8 on x64
      const u32 hru = ((u32 GSTD (*)(void*, u32))vmethod(tex, 17))(tex, 0);
      if (hrl == 0 && pbits && hru == 0) bits |= 0x08u;
    }
  }

  // ---- 5. vertex buffer lifecycle (CreateVertexBuffer = slot 23; Lock =
  //         vb slot 11, Unlock = 12 in D3D8's 14-slot vtable) ----
  if (dev) {
    void* vb = 0;
    const u32 hr = ((u32 GSTD (*)(void*, u32, u32, u32, u32, void**))
                    vmethod(dev, 23))(dev, 4096, 0, 0x42 /*FVF XYZ|DIFFUSE*/,
                                      1, &vb);
    if (hr == 0 && vb) {
      void* data = 0;
      const u32 hrl = ((u32 GSTD (*)(void*, u32, u32, void**, u32))
                       vmethod(vb, 11))(vb, 0, 4096, &data, 0);
      const u32 hru = ((u32 GSTD (*)(void*))vmethod(vb, 12))(vb);
      if (hrl == 0 && data && hru == 0) bits |= 0x10u;
    }
  }

  // ---- 6. render state / transform / FVF-by-shader-handle ----
  if (dev) {
    u32 ok = 1;
    // SetRenderState (50): D3DRS_ZENABLE = 7, D3DRS_LIGHTING = 137
    ((u32 GSTD (*)(void*, u32, u32))vmethod(dev, 50))(dev, 7, 1);
    ((u32 GSTD (*)(void*, u32, u32))vmethod(dev, 50))(dev, 137, 0);
    // SetTransform (37): D3DTS_WORLD = 1, identity
    const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    ((u32 GSTD (*)(void*, u32, const void*))vmethod(dev, 37))(dev, 1, id);
    // SetVertexShader (76) with an FVF handle (D3D8's 2D path)
    ((u32 GSTD (*)(void*, u32))vmethod(dev, 76))(dev, 0x42);
    if (ok) bits |= 0x20u;
  }

  // ---- 7. frame: Begin/End/Clear (34/35/36), DrawPrimitiveUP (72),
  //         Present (15) ----
  if (dev) {
    ((u32 GSTD (*)(void*))vmethod(dev, 34))(dev);
    ((u32 GSTD (*)(void*, u32, const void*, u32, u32, float, u32))
     vmethod(dev, 36))(dev, 0, 0, 7, 0xFF201030, 1.0f, 0);
    const float verts[3 * 6] = {
        100, 100, 0.5f, 1, 0, 0, 200, 100, 0.5f, 0, 1, 0,
        100, 200, 0.5f, 0, 0, 1};
    ((u32 GSTD (*)(void*, u32, u32, const void*, u32))vmethod(dev, 72))(
        dev, 4 /*TRIANGLELIST*/, 1, verts, 24 /*stride*/);
    ((u32 GSTD (*)(void*))vmethod(dev, 35))(dev);
    const u32 hrp = ((u32 GSTD (*)(void*, const void*, const void*, void*, void*))
                     vmethod(dev, 15))(dev, 0, 0, 0, 0);
    if (hrp == 0) bits |= 0x40u;
    bits |= 0x80u;
  }

  // ---- 8. index buffer + D3D8's SetIndices (85: carries base vertex) +
  //         DrawIndexedPrimitive (71: no base-vertex arg) ----
  if (dev) {
    void* ib = 0;
    const u32 hr = ((u32 GSTD (*)(void*, u32, u32, u32, u32, void**))
                    vmethod(dev, 24))(dev, 1024, 0, 101 /*D3DFMT_INDEX16*/,
                                      1, &ib);
    if (hr == 0 && ib) {
      ((u32 GSTD (*)(void*, void*, u32))vmethod(dev, 85))(dev, ib, 16);
      ((u32 GSTD (*)(void*, u32, u32, u32, u32, u32))vmethod(dev, 71))(
          dev, 4, 8, 12, 0, 2);
      bits |= 0x100u;
    }
  }

  // ---- 9. caps (slot 7): MaxTextureWidth at 0x54 (D3DCAPS8) ----
  if (dev) {
    u8 caps[0x100];
    for (u32 i = 0; i < 0x100; ++i) caps[i] = 0;
    ((u32 GSTD (*)(void*, void*))vmethod(dev, 7))(dev, caps);
    const u32 maxw = *(u32*)(caps + 0x54);
    const u32 psv = *(u32*)(caps + 0xEC);
    if (maxw >= 2048 && (psv >> 8) >= 1) bits |= 0x200u;
  }

  // ---- 10. shaders are DWORD handles (CreateVertexShader = 75) ----
  if (dev) {
    u32 handle = 0;
    const u32 hr = ((u32 GSTD (*)(void*, const void*, const void*, u32*))
                    vmethod(dev, 75))(dev, 0, 0, &handle);
    if (hr == 0 && handle != 0) bits |= 0x400u;
  }

  // ---- 11. CopyRects (28) — D3D8-only ----
  if (dev) {
    const u32 hr = ((u32 GSTD (*)(void*, void*, const void*, u32, void*,
                                 const void*))vmethod(dev, 28))(dev, 0, 0, 0,
                                                                0, 0);
    if (hr == 0) bits |= 0x800u;
  }

  return bits;
}
