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
//   0x400 D3D9Ex path (Direct3DCreate9Ex -> CreateDeviceEx -> ResetEx ->
//         PresentEx -> CheckDeviceState -> GetDisplayModeEx)
//   0x800 Render target (CreateRenderTarget -> Set/GetRenderTarget ->
//         GetBackBuffer -> restore)
//   0x1000 Shaders + vertex declaration (CreateVertex/PixelShader,
//          SetVertex/PixelShader, SetVertex/PixelShaderConstantF,
//          CreateVertexDeclaration, SetVertexDeclaration)
//   0x2000 Sampler state + scissor rect (SetSamplerState, Set/GetScissorRect)
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
__attribute__((dllimport)) int GSTD Direct3DCreate9Ex(u32 sdk, void** out);

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

  // ---- 10. D3D9Ex: the path XNA 4.0 takes on Vista+ (Direct3DCreate9Ex ->
  //         GetAdapterLUID root slot 21 -> CreateDeviceEx root slot 20 ->
  //         CheckDeviceState dev slot 127 -> ResetEx dev slot 131 ->
  //         PresentEx dev slot 121 -> GetDisplayModeEx dev slot 132) ----
  {
    void* d3dex = 0;
    void* devex = 0;
    u32 pp[16];
    for (u32 i = 0; i < 16; ++i) pp[i] = 0;
    pp[0] = 1280; pp[1] = 720; pp[2] = 21 /* D3DFMT_X8R8G8B8 */;
    pp[10] = 1;   // Windowed = TRUE
    if (Direct3DCreate9Ex(32 /* D3D_SDK_VERSION */, &d3dex) == 0 && d3dex) {
      u64 luid = 0xFFFFFFFFFFFFFFFFull;
      const u32 hr_luid =
          ((u32 GSTD (*)(void*, u32, void*))vmethod(d3dex, 21))(d3dex, 0,
                                                               &luid);
      const u32 hr_dev =
          ((u32 GSTD (*)(void*, u32, u32, void*, u32, void*, void*, void**))
           vmethod(d3dex, 20))(d3dex, 0 /*adapter*/, 1 /*HAL*/, 0, 0, pp,
                               0 /*fullscreen mode*/, &devex);
      if (hr_luid == 0 && luid == 0 && hr_dev == 0 && devex) {
        const u32 ok_state =
            ((u32 GSTD (*)(void*, void*))vmethod(devex, 127))(devex, 0) == 0;
        const u32 ok_reset =
            ((u32 GSTD (*)(void*, void*, void*))vmethod(devex, 131))(devex, pp,
                                                                    0) == 0;
        const u32 ok_present =
            ((u32 GSTD (*)(void*, const void*, const void*, const void*,
                           const void*, u32))vmethod(devex, 121))(devex, 0, 0,
                                                                 0, 0, 0) == 0;
        u32 modeex[8];
        for (u32 i = 0; i < 8; ++i) modeex[i] = 0;
        u32 rot = 99;
        const u32 ok_mode =
            ((u32 GSTD (*)(void*, u32, void*, void*))vmethod(devex, 132))
                    (devex, 0, modeex, &rot) == 0 &&
            modeex[0] == 1920u && modeex[1] == 1080u && rot == 0;
        if (ok_state && ok_reset && ok_present && ok_mode) bits |= 0x400u;
      }
    }
  }

  // ---- 11. render target: the path Celeste's Monocle engine renders its
  //         320x180 virtual screen through (CreateRenderTarget dev slot 28,
  //         SetRenderTarget 37, GetRenderTarget 38, GetBackBuffer 18) ----
  if (dev) {
    void* rt = 0;
    const u32 hr =
        ((u32 GSTD (*)(void*, u32, u32, u32, u32, u32, int, void**, void**))
         vmethod(dev, 28))(dev, 320, 180, 21 /*X8R8G8B8*/, 0, 0, 1, &rt, 0);
    if (hr == 0 && rt) {
      const u32 ok_set =
          ((u32 GSTD (*)(void*, u32, void*))vmethod(dev, 37))(dev, 0, rt) == 0;
      void* got = 0;
      const u32 ok_get =
          ((u32 GSTD (*)(void*, u32, void**))vmethod(dev, 38))(dev, 0, &got) ==
              0 &&
          got == rt;
      void* bb = 0;
      const u32 ok_bb =
          ((u32 GSTD (*)(void*, u32, u32, u32, void**))vmethod(dev, 18))
                  (dev, 0, 0, 0, &bb) == 0 &&
          bb != 0;
      // restore the backbuffer as render target 0
      const u32 ok_restore =
          ((u32 GSTD (*)(void*, u32, void*))vmethod(dev, 37))(dev, 0, bb) == 0;
      if (ok_set && ok_get && ok_bb && ok_restore) bits |= 0x800u;
    }
  }

  // ---- 12. shaders + vertex declaration: XNA's SpriteBatch/BasicEffect
  //         path (CreateVertexShader 91, CreatePixelShader 106,
  //         SetVertexShader 92, SetPixelShader 107, SetVertexShaderConstantF
  //         94, SetPixelShaderConstantF 109, CreateVertexDeclaration 86,
  //         SetVertexDeclaration 87) ----
  if (dev) {
    // vs_2_0: dcl_position v0; add oPos, v0, c2; end
    static const u32 vs_c2[] = {
        0xFFFE0200u, 0x0200001Fu, 0x80000000u, 0x900F0000u,
        0x03000002u, 0xC00F0000u, 0x90E40000u, 0xA0E40002u, 0x0000FFFFu};
    // ps_2_0: dcl_2d s0; dcl t0.xy; texld r0, t0, s0; mov oC0, r0; end
    // (encoding verified against MojoShader: parses with 0 errors)
    static const u32 ps_tex[] = {
        0xFFFF0200u, 0x0200001Fu, 0x90000000u, 0xA00F0800u,
        0x0200001Fu, 0x80000000u, 0xB0030000u, 0x03000042u, 0x800F0000u,
        0xB0E40000u, 0xA0E40800u, 0x02000001u, 0x800F0800u, 0x80E40000u,
        0x0000FFFFu};
    // VertexPositionColorTexture: FLOAT3 pos @0, UBYTE4 color @12,
    // FLOAT2 texcoord @16 (D3DVERTEXELEMENT9 = 8 bytes each, 0xFF ends)
    static const u32 decl[] = {
        0x00000000u, 0x00000002u,  // {0, 0, FLOAT3, DEFAULT, POSITION, 0}
        0x000C0000u, 0x000A0005u,  // {0, 12, UBYTE4, DEFAULT, COLOR, 0}
        0x00100000u, 0x00050001u,  // {0, 16, FLOAT2, DEFAULT, TEXCOORD, 0}
        0x000000FFu, 0x00000000u};  // D3DDECL_END
    void* vs = 0;
    void* ps = 0;
    void* vd = 0;
    const u32 hr_vs = ((u32 GSTD (*)(void*, const u32*, void**))vmethod(dev, 91))
                          (dev, vs_c2, &vs);
    const u32 hr_ps = ((u32 GSTD (*)(void*, const u32*, void**))vmethod(dev, 106))
                          (dev, ps_tex, &ps);
    const u32 hr_vd =
        ((u32 GSTD (*)(void*, const void*, void**))vmethod(dev, 86))(dev, decl,
                                                                    &vd);
    float cf[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    const u32 ok_bind =
        hr_vs == 0 && vs != 0 && hr_ps == 0 && ps != 0 && hr_vd == 0 &&
        vd != 0 &&
        ((u32 GSTD (*)(void*, void*))vmethod(dev, 92))(dev, vs) == 0 &&
        ((u32 GSTD (*)(void*, void*))vmethod(dev, 107))(dev, ps) == 0 &&
        ((u32 GSTD (*)(void*, void*))vmethod(dev, 87))(dev, vd) == 0 &&
        ((u32 GSTD (*)(void*, u32, const float*, u32))vmethod(dev, 94))
                (dev, 0, cf, 1) == 0 &&
        ((u32 GSTD (*)(void*, u32, const float*, u32))vmethod(dev, 109))
                (dev, 0, cf, 1) == 0;
    if (ok_bind) bits |= 0x1000u;
  }

  // ---- 13. sampler state + scissor rect (SpriteBatch sets linear/clamp
  //         samplers per draw; Monocle scissor-clips the virtual screen) ----
  if (dev) {
    const u32 ok_samp =
        ((u32 GSTD (*)(void*, u32, u32, u32))vmethod(dev, 69))
                (dev, 0, 5 /*MAGFILTER*/, 2 /*LINEAR*/) == 0 &&
        ((u32 GSTD (*)(void*, u32, u32, u32))vmethod(dev, 69))
                (dev, 0, 6 /*MINFILTER*/, 2) == 0 &&
        ((u32 GSTD (*)(void*, u32, u32, u32))vmethod(dev, 69))
                (dev, 0, 7 /*MIPFILTER*/, 0 /*NONE*/) == 0 &&
        ((u32 GSTD (*)(void*, u32, u32, u32))vmethod(dev, 69))
                (dev, 0, 1 /*ADDRESSU*/, 3 /*CLAMP*/) == 0 &&
        ((u32 GSTD (*)(void*, u32, u32, u32))vmethod(dev, 69))
                (dev, 0, 2 /*ADDRESSV*/, 3) == 0;
    i32 rect[4] = {0, 0, 320, 180};
    const u32 ok_scissor =
        ((u32 GSTD (*)(void*, const void*))vmethod(dev, 75))(dev, rect) == 0;
    i32 back[4] = {-1, -1, -1, -1};
    const u32 ok_scissor_get =
        ((u32 GSTD (*)(void*, void*))vmethod(dev, 76))(dev, back) == 0 &&
        back[0] == 0 && back[1] == 0 && back[2] == 320 && back[3] == 180;
    if (ok_samp && ok_scissor && ok_scissor_get) bits |= 0x2000u;
  }

  return bits;
}
