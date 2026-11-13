// tiramisu/src/guest/guest_ddraw.c
// Proves the DirectDraw bridge: the full ddraw lifecycle a 2D game runs —
// DirectDrawCreateEx -> SetCooperativeLevel -> SetDisplayMode ->
// CreateSurface (primary) -> Lock/write pixels/Unlock -> Flip.
// Every COM call goes through the vtable read from the object in memory.
// Returns a bitmask:
//   0x001 DirectDrawCreateEx returned an IDirectDraw7
//   0x002 SetCooperativeLevel accepted
//   0x004 SetDisplayMode accepted
//   0x008 CreateSurface (primary) returned a surface
//   0x010 Lock handed back a pixel pointer with a sane pitch
//   0x020 pixels written, Unlock accepted
//   0x040 GetPixelFormat reports 32bpp RGB masks
//   0x080 offscreen surface created
//   0x100 BltFast copied offscreen -> primary
//   0x200 Flip accepted
//   0x400 GetCaps reported blit capability
//   0x800 GetDisplayMode round-trips the mode
typedef unsigned int u32;
typedef int i32;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long long u64;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

int _fltused = 1;

__attribute__((dllimport)) u32 GSTD DirectDrawCreateEx(void* guid,
                                                       void** out_dd,
                                                       const void* iid,
                                                       void* unk);

static void* vmethod(void* obj, u32 slot) {
  void** vtbl = *(void***)obj;
  return vtbl[slot];
}

u32 GSTD guest_ddraw_main(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  // 1. device (DirectDrawCreateEx -> IDirectDraw7)
  void* dd = 0;
  if (DirectDrawCreateEx(0, &dd, 0, 0) == 0 && dd) bits |= 0x001u;

  // 2. SetCooperativeLevel (slot 20)
  if (dd) {
    if (((u32 GSTD (*)(void*, void*, u32))vmethod(dd, 20))(dd, (void*)0x1234,
                                                          0x11) == 0)
      bits |= 0x002u;  // DDSCL_FULLSCREEN|EXCLUSIVE|ALLOWREBOOT
  }

  // 3. SetDisplayMode (slot 21)
  if (dd) {
    if (((u32 GSTD (*)(void*, u32, u32, u32, u32, u32))vmethod(dd, 21))
            (dd, 1280, 720, 32, 0, 0) == 0)
      bits |= 0x004u;
  }

  // 4. primary surface (slot 6). DDSURFACEDESC2 x64:
  //    +0 dwSize(120), +4 dwFlags, +8 dwHeight, +12 dwWidth,
  //    +72 DDPIXELFORMAT (32 bytes), +104 ddsCaps (12 bytes)
  u8 desc[120];
  for (u32 i = 0; i < 120; ++i) desc[i] = 0;
  *(u32*)(desc + 0) = 120;                       // dwSize
  *(u32*)(desc + 4) = 0x00000001 | 0x00000002 |  // CAPS|HEIGHT|WIDTH
                      0x00000004 | 0x00001000;   // |PIXELFORMAT
  *(u32*)(desc + 8) = 720;                       // dwHeight
  *(u32*)(desc + 12) = 1280;                     // dwWidth
  *(u32*)(desc + 72) = 32;                       // DDPIXELFORMAT.dwSize
  *(u32*)(desc + 76) = 0x00000040;               // DDPF_RGB
  *(u32*)(desc + 80) = 0;                        // no FOURCC
  *(u32*)(desc + 84) = 32;                       // 32 bpp
  *(u32*)(desc + 88) = 0x00FF0000;               // R mask
  *(u32*)(desc + 92) = 0x0000FF00;               // G mask
  *(u32*)(desc + 96) = 0x000000FF;               // B mask
  *(u32*)(desc + 104) = 0x00000200;              // DDSCAPS_PRIMARYSURFACE

  void* prim = 0;
  if (dd) {
    const u32 hr = ((u32 GSTD (*)(void*, void*, void**, void*))vmethod(dd, 6))
                       (dd, desc, &prim, 0);
    if (hr == 0 && prim) bits |= 0x008u;
  }

  // 5. Lock (slot 25) -> pixel pointer
  u8* px = 0;
  u32 pitch = 0;
  if (prim) {
    u8 sd[120];
    for (u32 i = 0; i < 120; ++i) sd[i] = 0;
    *(u32*)(sd + 0) = 120;
    const u32 hr = ((u32 GSTD (*)(void*, void*, void*, u32, void*))
                    vmethod(prim, 25))(prim, 0, sd, 0, 0);
    if (hr == 0) {
      px = *(u8**)(sd + 32);   // lpSurface
      pitch = *(u32*)(sd + 16); // lPitch
      if (px && pitch >= 1280 * 4) bits |= 0x010u;
    }
  }

  // 6. write a gradient, Unlock (slot 32)
  if (px) {
    for (u32 y = 0; y < 720; ++y) {
      u8* row = px + (u64)y * pitch;
      for (u32 x = 0; x < 1280; ++x) {
        u32* p = (u32*)(row + x * 4);
        *p = 0xFF000000u | (x << 16) | (y << 8) | 0x40u;
      }
    }
    if (((u32 GSTD (*)(void*, void*))vmethod(prim, 32))(prim, 0) == 0)
      bits |= 0x020u;
  }

  // 7. GetPixelFormat (slot 21)
  if (prim) {
    u32 pfd[8];
    for (u32 i = 0; i < 8; ++i) pfd[i] = 0;
    if (((u32 GSTD (*)(void*, void*))vmethod(prim, 21))(prim, pfd) == 0 &&
        pfd[3] == 32 && pfd[4] == 0x00FF0000)
      bits |= 0x040u;
  }

  // 8. offscreen surface (same desc minus the primary cap)
  void* off = 0;
  *(u32*)(desc + 104) = 0x00000800;  // DDSCAPS_SYSTEMMEMORY
  if (dd) {
    const u32 hr = ((u32 GSTD (*)(void*, void*, void**, void*))vmethod(dd, 6))
                       (dd, desc, &off, 0);
    if (hr == 0 && off) bits |= 0x080u;
  }

  // 9. BltFast offscreen -> primary (slot 7). The offscreen must have
  //    content: lock, write, unlock first.
  if (off) {
    u8 sd[120];
    for (u32 i = 0; i < 120; ++i) sd[i] = 0;
    *(u32*)(sd + 0) = 120;
    if (((u32 GSTD (*)(void*, void*, void*, u32, void*))vmethod(off, 25))
            (off, 0, sd, 0, 0) == 0) {
      u8* opx = *(u8**)(sd + 32);
      if (opx) {
        for (u32 y = 0; y < 64; ++y) {
          u32* row = (u32*)(opx + (u64)y * (*(u32*)(sd + 16)));
          for (u32 x = 0; x < 64; ++x) row[x] = 0xFF00FF00u;
        }
      }
      ((u32 GSTD (*)(void*, void*))vmethod(off, 32))(off, 0);
      // BltFast(dstx=100, dsty=100, off, 0, 0)
      if (((u32 GSTD (*)(void*, u32, u32, void*, void*, u32))
           vmethod(prim, 7))(prim, 100, 100, off, 0, 0) == 0)
        bits |= 0x100u;
    }
  }

  // 10. Flip (slot 11)
  if (prim) {
    if (((u32 GSTD (*)(void*, void*, u32))vmethod(prim, 11))(prim, 0, 0) == 0)
      bits |= 0x200u;
  }

  // 11. GetCaps (slot 11 on the DirectDraw object)
  if (dd) {
    u32 caps[64];
    for (u32 i = 0; i < 64; ++i) caps[i] = 0;
    caps[0] = 256;  // dwSize
    if (((u32 GSTD (*)(void*, void*, void*))vmethod(dd, 11))(dd, caps, 0) ==
            0 &&
        (caps[1] & 0x00000040u) /* DDCAPS_BLT */)
      bits |= 0x400u;
  }

  // 12. GetDisplayMode (slot 12)
  if (dd) {
    u8 dm[120];
    for (u32 i = 0; i < 120; ++i) dm[i] = 0;
    *(u32*)(dm + 0) = 120;
    if (((u32 GSTD (*)(void*, void*))vmethod(dd, 12))(dd, dm) == 0 &&
        *(u32*)(dm + 8) == 720 && *(u32*)(dm + 12) == 1280)
      bits |= 0x800u;
  }

  return bits;
}
