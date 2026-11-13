// tiramisu/src/guest/guest_dsound.c
// Proves the DirectSound bridge: the full dsound lifecycle a game runs —
// DirectSoundCreate8 -> SetCooperativeLevel -> CreateSoundBuffer ->
// Lock/write PCM/Unlock -> Play -> Stop. Every COM call goes through the
// vtable read from the object in memory (real COM discipline).
// Returns a bitmask:
//   0x01 DirectSoundCreate8 returned a device
//   0x02 GetCaps sane (stereo 16-bit secondary)
//   0x04 SetCooperativeLevel accepted
//   0x08 CreateSoundBuffer returned a buffer
//   0x10 Lock handed back a writable pointer
//   0x20 Unlock + Play accepted
//   0x40 GetStatus reports PLAYING
//   0x80 Stop accepted
//   0x100 GetFormat round-trips a PCM WAVEFORMATEX
//   0x200 DirectSoundEnumerateW ran without crashing
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

__attribute__((dllimport)) u32 GSTD DirectSoundCreate8(const void* guid,
                                                       void** out_ds,
                                                       void* unk);
__attribute__((dllimport)) u32 GSTD DirectSoundEnumerateW(void* cb, void* ctx);

// read slot N of a COM object's vtable
static void* vmethod(void* obj, u32 slot) {
  void** vtbl = *(void***)obj;
  return vtbl[slot];
}

u32 GSTD guest_dsound_main(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  // 1. device
  void* ds = 0;
  if (DirectSoundCreate8(0, &ds, 0) == 0 && ds) bits |= 0x01u;

  // 2. caps (IDirectSound8::GetCaps = slot 4; DSCAPS dwFlags at +4)
  if (ds) {
    u32 caps[8];
    for (u32 i = 0; i < 8; ++i) caps[i] = 0;
    caps[0] = 32;  // dwSize
    const u32 hr = ((u32 GSTD (*)(void*, void*))vmethod(ds, 4))(ds, caps);
    if (hr == 0 && (caps[1] & 0x00000200) /* SECONDARYSTEREO */)
      bits |= 0x02u;
  }

  // 3. cooperative level (slot 6)
  if (ds) {
    if (((u32 GSTD (*)(void*, void*, u32))vmethod(ds, 6))(ds, (void*)0x1234,
                                                          2 /*PRIORITY*/) == 0)
      bits |= 0x04u;
  }

  // 4. secondary buffer (slot 3). DSBUFFERDESC x64:
  //    +0 dwSize(32), +4 dwFlags, +8 dwBufferBytes, +12 reserved,
  //    +16 LPWAVEFORMATEX, +24 GUID
  u8 desc[40];
  for (u32 i = 0; i < 40; ++i) desc[i] = 0;
  u8 wfx[18];
  for (u32 i = 0; i < 18; ++i) wfx[i] = 0;
  wfx[0] = 0x01; wfx[1] = 0x00;        // WAVE_FORMAT_PCM
  wfx[2] = 0x02; wfx[3] = 0x00;        // nChannels = 2
  *(u32*)(wfx + 4) = 44100;            // nSamplesPerSec
  *(u32*)(wfx + 8) = 44100 * 4;        // nAvgBytesPerSec
  wfx[12] = 0x04; wfx[13] = 0x00;      // nBlockAlign = 4
  wfx[14] = 0x10; wfx[15] = 0x00;      // wBitsPerSample = 16
  *(u32*)(desc + 0) = 32;              // dwSize
  *(u32*)(desc + 4) = 0x00008000;      // DSBCAPS_GETCURRENTPOSITION2
  *(u32*)(desc + 8) = 88200;           // 0.5 s of stereo 16-bit
  *(u64*)(desc + 16) = (u64)wfx;

  void* buf = 0;
  if (ds) {
    const u32 hr = ((u32 GSTD (*)(void*, const void*, void**, void*))
                    vmethod(ds, 3))(ds, desc, &buf, 0);
    if (hr == 0 && buf) bits |= 0x08u;
  }

  // 5. lock + write PCM (IDirectSoundBuffer8: Lock = 11, Unlock = 19)
  if (buf) {
    void* p1 = 0;
    u32 b1 = 0;
    void* p2 = 0;
    u32 b2 = 0;
    const u32 hrl = ((u32 GSTD (*)(void*, u32, u32, void**, u32*, void**,
                                   u32*, u32))vmethod(buf, 11))
                        (buf, 0, 0, &p1, &b1, &p2, &b2, 0);
    if (hrl == 0 && p1 && b1) {
      bits |= 0x10u;
      // a soft sine-ish tone: PCM bytes the sink will actually see
      u8* pcm = (u8*)p1;
      for (u32 i = 0; i + 1 < b1; i += 2) {
        const u16 s = (u16)(i ^ (i << 1));  // deterministic non-zero pattern
        pcm[i] = (u8)(s & 0xFF);
        pcm[i + 1] = (u8)(s >> 8);
      }
      // Unlock (slot 19) then Play (slot 12, DSBPLAY_LOOPING = 1)
      const u32 ok =
          ((u32 GSTD (*)(void*, void*, u32, void*, u32))vmethod(buf, 19))
              (buf, p1, b1, p2, b2) == 0 &&
          ((u32 GSTD (*)(void*, u32, u32, u32))vmethod(buf, 12))
              (buf, 0, 0, 0) == 0;
      if (ok) bits |= 0x20u;
    }
  }

  // 6. status reports playing (slot 9)
  if (buf) {
    u32 st = 0;
    if (((u32 GSTD (*)(void*, u32*))vmethod(buf, 9))(buf, &st) == 0 &&
        (st & 0x1u))
      bits |= 0x40u;
  }

  // 7. stop (slot 18)
  if (buf) {
    if (((u32 GSTD (*)(void*))vmethod(buf, 18))(buf) == 0) bits |= 0x80u;
  }

  // 8. GetFormat round-trip (slot 5)
  if (buf) {
    u8 gf[18];
    for (u32 i = 0; i < 18; ++i) gf[i] = 0;
    u32 sz = 18;
    const u32 hr = ((u32 GSTD (*)(void*, void*, u32*))vmethod(buf, 5))
                       (buf, gf, &sz);
    if (hr == 0 && sz == 18 && gf[0] == 0x01) bits |= 0x100u;
  }

  // 9. enumeration runs
  if (DirectSoundEnumerateW(0, 0) == 0) bits |= 0x200u;

  return bits;
}
