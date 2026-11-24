// tiramisu/src/guest/guest_mmedia.c
// Proves the small-gap bridges: ACM identity streams open and bogus
// codec requests fail visibly, VFW answers "no codecs", winspool
// answers "no printers" — the boot-probe shapes engines rely on.
// Returns a bitmask:
//   0x01 acmGetVersion nonzero
//   0x02 acmStreamOpen PCM->PCM opened a stream
//   0x04 acmStreamOpen ADPCM->PCM opened (real MS ADPCM decoder)
//   0x08 prepare/convert/close on the ADPCM stream produced PCM bytes
//   0x10 acmFormatTagEnum returned PCM as tag 0
//   0x20 ICInfo/ICOpen report no codecs (engine skips AVI logos)
//   0x40 DrawDibOpen reports NULL
//   0x80 EnumPrintersW returns 0 printers
//   0x100 OpenPrinterW fails (no printer)
typedef unsigned int u32;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long long u64;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

int _fltused = 1;

__attribute__((dllimport)) u32 GSTD acmGetVersion(void);
__attribute__((dllimport)) u32 GSTD acmStreamOpen(void** has_h, void* hi,
                                                  const void* wfx_src,
                                                  const void* wfx_dst,
                                                  void* filt, u64 callback,
                                                  u32 instance, u32 flags);
__attribute__((dllimport)) u32 GSTD acmStreamPrepareHeader(void* h, void* hdr,
                                                            u32 cb);
__attribute__((dllimport)) u32 GSTD acmStreamConvert(void* h, void* hdr,
                                                     u32 flags);
__attribute__((dllimport)) u32 GSTD acmStreamClose(void* h, u32 flags);
__attribute__((dllimport)) u32 GSTD acmFormatTagEnumW(void* hi, void* out,
                                                      u32* size, u32 flags);
__attribute__((dllimport)) u32 GSTD ICInfo(u32 fcc_type, u32 idx, void* out);
__attribute__((dllimport)) void* GSTD ICOpen(u32 fcc_type, u32 fcc_handler,
                                             u32 mode);
__attribute__((dllimport)) void* GSTD DrawDibOpen(void);
__attribute__((dllimport)) u32 GSTD EnumPrintersW(u32 flags, const void* name,
                                                  u32 level, u8* buf,
                                                  u32 buf_len, u32* needed,
                                                  u32* returned);
__attribute__((dllimport)) u32 GSTD OpenPrinterW(const void* name, void** h,
                                                 void* defaults);

// tiny WAVEFORMATEX builder (PCM=1, ADPCM=2)
static void wfx(u8* b, u16 tag) {
  for (u32 i = 0; i < 18; ++i) b[i] = 0;
  *(u16*)(b + 0) = tag;
  *(u16*)(b + 2) = 2;       // channels
  *(u32*)(b + 4) = 44100;   // rate
  *(u32*)(b + 8) = 44100 * 4;
  *(u16*)(b + 12) = 4;
  *(u16*)(b + 14) = 16;
}

// MS ADPCM needs its block geometry: mono 22050, 256-byte blocks, 4-bit
// samples, 500 samples per block. That is the shape a compressed WAV asset
// carries, and the shape the decoder now converts.
static void wfx_msadpcm(u8* b) {
  for (u32 i = 0; i < 20; ++i) b[i] = 0;
  *(u16*)(b + 0) = 2;                     // WAVE_FORMAT_ADPCM
  *(u16*)(b + 2) = 1;                     // mono
  *(u32*)(b + 4) = 22050;
  *(u32*)(b + 8) = 22050 * 256 / 500;     // nAvgBytesPerSec
  *(u16*)(b + 12) = 256;                  // nBlockAlign
  *(u16*)(b + 14) = 4;                    // wBitsPerSample
  *(u16*)(b + 16) = 2;                    // cbSize
  *(u16*)(b + 18) = 500;                  // wSamplesPerBlock
}

u32 GSTD guest_mmedia_main(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  // 1. version
  if (acmGetVersion() != 0) bits |= 0x01u;

  // 2. PCM->PCM stream opens
  u8 p1[18], p2[18], ad[20];
  wfx(p1, 1);
  wfx(p2, 1);
  wfx_msadpcm(ad);
  void* h = 0;
  if (acmStreamOpen(&h, 0, p1, p2, 0, 0, 0, 0) == 0 && h) bits |= 0x02u;

  // 3. ADPCM->PCM opens: the decoder is real now
  void* h2 = 0;
  if (acmStreamOpen(&h2, 0, ad, p2, 0, 0, 0, 0) == 0 && h2) bits |= 0x04u;
  if (h) acmStreamClose(h, 0);

  // 4. a real 64-bit ACMSTREAMHEADER through prepare -> convert -> close on
  //    the ADPCM stream: the decoder runs inside the guest, not just in tests.
  if (h2) {
    // Statics, not a 4 KiB stack frame: the freestanding guest has no
    // __chkstk, and a large frame would need one.
    static u8 src[256];
    static u8 dst[4096];
    for (u32 i = 0; i < 256; ++i) src[i] = 0;  // block header + silence
    u64 hdr[18];  // 144 bytes: the x64 ACMSTREAMHEADER
    for (u32 i = 0; i < 18; ++i) hdr[i] = 0;
    *(u32*)((u8*)hdr + 0) = 144;                // cbStruct
    *(void**)((u8*)hdr + 16) = (void*)src;      // pbSrc
    *(u32*)((u8*)hdr + 24) = 256;               // cbSrcLength (one block)
    *(void**)((u8*)hdr + 40) = (void*)dst;      // pbDst
    *(u32*)((u8*)hdr + 48) = 4096;              // cbDstLength
    if (acmStreamPrepareHeader(h2, hdr, 144) == 0 &&
        acmStreamConvert(h2, hdr, 144) == 0 &&
        (*(u32*)((u8*)hdr + 4) & 0x10000u) != 0 &&   // STATUSF_DONE
        *(u32*)((u8*)hdr + 52) > 0 &&                // cbDstLengthUsed
        acmStreamClose(h2, 0) == 0)
      bits |= 0x08u;
  }

  // 5. format-tag enumeration: tag 0 exists
  u32 tag[8];
  for (u32 i = 0; i < 8; ++i) tag[i] = 0;
  tag[0] = 64;  // cbStruct
  tag[1] = 0;   // index
  u32 tsz = 64;
  if (acmFormatTagEnumW(0, tag, &tsz, 0) == 0 && tag[2] == 1)
    bits |= 0x10u;

  // 6. no AVI codecs
  if (ICInfo(0x63646169 /* 'iad c' = 'vidc' LE */, 0, 0) == 0) {
    void* hic = ICOpen(0x63646169, 0x31346d64, 1);
    if (hic == 0) bits |= 0x20u;
  }

  // 7. no DrawDib
  if (DrawDibOpen() == 0) bits |= 0x40u;

  // 8. no printers
  u32 needed = 1, returned = 1;
  if (EnumPrintersW(2, 0, 1, 0, 0, &needed, &returned) == 0 && returned == 0)
    bits |= 0x80u;

  // 9. no default printer
  void* ph = 0;
  if (OpenPrinterW(0, &ph, 0) == 0) bits |= 0x100u;

  return bits;
}
