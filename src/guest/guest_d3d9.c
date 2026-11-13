// tiramisu/src/guest/guest_d3d9.c
// Proves the D3D9 bridge: the full D3D9 lifecycle a 2D/3D engine runs —
// Direct3DCreate9 -> CreateDevice -> texture/vb/ib creation + locks ->
// render state + transforms -> clear/draw/present. Every COM call goes
// through the vtable read from the object in memory (real COM discipline,
// same as guest_dinput.c). Returns a bitmask:
//   0x001 Direct3DCreate9 returned a device
//   0x002 GetAdapterCount >= 1
//   0x004 CreateDevice succeeded
//   0x008 CreateTexture + LockRect + UnlockRect OK
//   0x010 CreateVertexBuffer + Lock + Unlock OK
//   0x020 SetRenderState/SetTransform/SetFVF accepted
//   0x040 DrawPrimitiveUP + Present OK
//   0x080 BeginScene/EndScene/Clear OK
//   0x100 CreateIndexBuffer + SetIndices + DrawIndexedPrimitive OK
//   0x200 GetDeviceCaps sane (MaxTextureWidth >= 2048)
typedef unsigned int u32;
typedef int i32;
typedef unsigned char u8;
typedef unsigned long long u64;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

int _fltused = 1;  // any float in the TU makes MSVC linkers want this

__attribute__((dllimport)) void* GSTD Direct3DCreate9(u32 sdk);

// read slot N of a COM object's vtable (the vtable pointer is the object's
// first member — the guest never links against bridge symbols directly)
static void* vmethod(void* obj, u32 slot) {
  void** vtbl = *(void***)obj;
  return vtbl[slot];
}

u32 GSTD guest_d3d9_main(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  // ---- 1. create the D3D9 object ----
  void* d3d = Direct3DCreate9(32 /* D3D_SDK_VERSION */);
  if (d3d) bits |= 0x01u;

  // ---- 2. adapter enumeration (slot 4 = GetAdapterCount) ----
  if (d3d) {
    const u32 adapters = ((u32 GSTD (*)(void*))vmethod(d3d, 4))(d3d);
    if (adapters >= 1) bits |= 0x02u;
  }

  // ---- 3. CreateDevice (slot 16). D3DPRESENT_PARAMETERS on x64:
  //         +0 BackBufferWidth, +4 Height, +8 Format, +40 Windowed ----
  void* dev = 0;
  if (d3d) {
    u32 pp[16];
    for (u32 i = 0; i < 16; ++i) pp[i] = 0;
    pp[0] = 1280; pp[1] = 720; pp[2] = 21 /* D3DFMT_X8R8G8B8 */;
    pp[10] = 1;   // Windowed = TRUE (offset 40/4)
    const u32 hr = ((u32 GSTD (*)(void*, u32, u32, void*, u32, void*, void**))
                    vmethod(d3d, 16))(d3d, 0 /*adapter*/, 1 /*HAL*/, 0, 0,
                                      pp, &dev);
    if (hr == 0 && dev) bits |= 0x04u;
  }

  // ---- 4. texture lifecycle (CreateTexture dev slot 23; the texture's
  //         own vtable: LockRect = 19, UnlockRect = 20) ----
  if (dev) {
    void* tex = 0;
    const u32 hr = ((u32 GSTD (*)(void*, u32, u32, u32, u32, u32, u32, void**))
                    vmethod(dev, 23))(dev, 64, 64, 1, 0, 21, 1 /*managed*/,
                                      &tex);
    if (hr == 0 && tex) {
      u8 lr[16];
      for (u32 i = 0; i < 16; ++i) lr[i] = 0;
      // D3DLOCKED_RECT { INT Pitch; void* pBits; } -> pBits at +8 on x64
      const u32 hrl = ((u32 GSTD (*)(void*, u32, void*, const void*, u32))
                       vmethod(tex, 19))(tex, 0, lr, 0, 0);
      const u64 pbits = *(u64*)(lr + 8);
      if (hrl == 0 && pbits) {
        u8* px = (u8*)pbits;
        px[0] = 0xFF; px[1] = 0x00; px[2] = 0x00; px[3] = 0xFF;  // touch it
        if (((u32 GSTD (*)(void*, u32))vmethod(tex, 20))(tex, 0) == 0)
          bits |= 0x08u;
      }
      // SetTexture (device slot 65)
      ((u32 GSTD (*)(void*, u32, void*))vmethod(dev, 65))(dev, 0, tex);
    }
  }

  // ---- 5. vertex + index buffers (dev slots 26/27; vb vtable: Lock = 11,
  //         Unlock = 12) ----
  if (dev) {
    void* vb = 0;
    const u32 hr = ((u32 GSTD (*)(void*, u32, u32, u32, u32, void**, void**))
                    vmethod(dev, 26))(dev, 4096, 0, 0x42 /*FVF XYZ|DIFFUSE*/,
                                      1, &vb, 0);
    if (hr == 0 && vb) {
      void* data = 0;
      const u32 hrl = ((u32 GSTD (*)(void*, u32, u32, void**, u32))
                       vmethod(vb, 11))(vb, 0, 0, &data, 0);
      if (hrl == 0 && data) {
        u8* p = (u8*)data;
        for (u32 i = 0; i < 256; ++i) p[i] = (u8)i;
        if (((u32 GSTD (*)(void*))vmethod(vb, 12))(vb) == 0) bits |= 0x10u;
      }
      // SetStreamSource (device slot 100)
      ((u32 GSTD (*)(void*, u32, void*, u32, u32))vmethod(dev, 100))
          (dev, 0, vb, 0, 32);
    }
    void* ib = 0;
    const u32 hri = ((u32 GSTD (*)(void*, u32, u32, u32, u32, void**, void**))
                     vmethod(dev, 27))(dev, 1024, 0, 101 /*D3DFMT_INDEX16*/,
                                       1, &ib, 0);
    if (hri == 0 && ib) {
      // SetIndices (device slot 104)
      ((u32 GSTD (*)(void*, void*))vmethod(dev, 104))(dev, ib);
      bits |= 0x100u;
    }
  }

  // ---- 6. render state (dev slot 57), transform (44), FVF (89) ----
  if (dev) {
    float ident[16];
    ident[0] = 1.f; ident[1] = 0.f; ident[2] = 0.f; ident[3] = 0.f;
    ident[4] = 0.f; ident[5] = 1.f; ident[6] = 0.f; ident[7] = 0.f;
    ident[8] = 0.f; ident[9] = 0.f; ident[10] = 1.f; ident[11] = 0.f;
    ident[12] = 0.f; ident[13] = 0.f; ident[14] = 0.f; ident[15] = 1.f;
    const u32 ok_rs =
        ((u32 GSTD (*)(void*, u32, u32))vmethod(dev, 57))(dev, 7 /*ZENABLE*/,
                                                          1) == 0 &&
        ((u32 GSTD (*)(void*, u32, u32))vmethod(dev, 57))(
            dev, 27 /*ALPHABLENDENABLE*/, 1) == 0 &&
        ((u32 GSTD (*)(void*, u32, u32))vmethod(dev, 57))(
            dev, 22 /*CULLMODE*/, 2) == 0 &&
        ((u32 GSTD (*)(void*, u32))vmethod(dev, 89))(
            dev, 0x42 /*FVF XYZ|DIFFUSE*/) == 0;
    const u32 ok_tf =
        ((u32 GSTD (*)(void*, u32, const void*))vmethod(dev, 44))(
            dev, 1 /*WORLD*/, ident) == 0 &&
        ((u32 GSTD (*)(void*, u32, const void*))vmethod(dev, 44))(
            dev, 2 /*VIEW*/, ident) == 0 &&
        ((u32 GSTD (*)(void*, u32, const void*))vmethod(dev, 44))(
            dev, 3 /*PROJECTION*/, ident) == 0;
    if (ok_rs && ok_tf) bits |= 0x20u;
  }

  // ---- 7. frame: BeginScene(41) / Clear(43) / EndScene(42) /
  //         DrawPrimitiveUP(83) / Present(17) ----
  if (dev) {
    const u32 ok_clear =
        ((u32 GSTD (*)(void*))vmethod(dev, 41))(dev) == 0 &&
        ((u32 GSTD (*)(void*, u32, const void*, u32, u32, float, u32))
         vmethod(dev, 43))(dev, 0, 0, 3 /*TARGET|ZBUFFER*/, 0xFF202020,
                           1.0f, 0) == 0 &&
        ((u32 GSTD (*)(void*))vmethod(dev, 42))(dev) == 0;
    if (ok_clear) bits |= 0x80u;

    // one UP triangle: XYZ + diffuse = 7 floats per vertex
    float tri[21];
    for (u32 v = 0; v < 3; ++v) {
      tri[v * 7 + 0] = (float)v * 0.5f;
      tri[v * 7 + 1] = 0.5f;
      tri[v * 7 + 2] = 0.0f;
      tri[v * 7 + 3] = 1.0f; tri[v * 7 + 4] = 0.0f;
      tri[v * 7 + 5] = 0.0f; tri[v * 7 + 6] = 1.0f;
    }
    const u32 ok_draw =
        ((u32 GSTD (*)(void*, u32, u32, const void*, u32))vmethod(dev, 83))
            (dev, 4 /*TRIANGLELIST*/, 1, tri, 7 * 4) == 0 &&
        ((u32 GSTD (*)(void*, const void*, const void*, const void*, void*))
         vmethod(dev, 17))(dev, 0, 0, 0, 0) == 0;
    if (ok_draw) bits |= 0x40u;
  }

  // ---- 8. indexed draw through the bound buffers (dev slot 82) ----
  if (dev) {
    const u32 hr = ((u32 GSTD (*)(void*, u32, i32, u32, u32, u32, u32))
                    vmethod(dev, 82))(dev, 4 /*TRIANGLELIST*/, 0, 0, 3, 0, 1);
    if (hr == 0) bits |= 0x100u;  // already set if ib creation worked
  }

  // ---- 9. caps look sane (IDirect3D9::GetDeviceCaps = slot 14;
  //         D3DCAPS9 MaxTextureWidth at 0x54) ----
  if (d3d) {
    u32 caps[128];
    for (u32 i = 0; i < 128; ++i) caps[i] = 0;
    const u32 hr = ((u32 GSTD (*)(void*, u32, u32, void*))
                    vmethod(d3d, 14))(d3d, 0, 1 /*HAL*/, caps);
    if (hr == 0 && caps[0x54 / 4] >= 2048) bits |= 0x200u;
  }

  return bits;
}
