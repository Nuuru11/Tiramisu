// tiramisu/src/trt/bridge_mmedia.cpp
// Small-gap bridges: msacm32, msvfw32, winspool.drv (M6 coverage).
// See bridge_mmedia.h for the design notes.
#include "bridge_mmedia.h"
#include "acm_codec.h"
#include "trt.h"
#include "thunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr int ACMERR_NOTPOSSIBLE_ = (int)0x80006001;  // ACMERR_NOTPOSSIBLE
constexpr int ACMERR_UNPREPARED_ = (int)0x80006002;    // ACMERR_UNPREPARED
constexpr int MMSYSERR_ERROR_ = 1;                    // generic mm error
constexpr int MMSYSERR_NOERROR_ = 0;
constexpr int MMSYSERR_INVALHANDLE_ = 5;
constexpr int MMSYSERR_INVALPARAM_ = 11;

void log_line(const char* s) { trt_core_log(s); }

}  // namespace

extern "C" {

// ===================== msacm32 =====================
namespace {

// One open conversion stream. The handle is a real pointer to this object;
// closing it marks the magic dead so a stale handle is rejected instead of
// silently converting into freed memory.
constexpr u32 kAcmStreamMagic = 0x41434D53u;  // 'ACMS'
constexpr u32 kAcmStreamDead = 0x44454144u;

struct acm_stream {
  u32 magic = kAcmStreamMagic;
  trt_acm_fmt src;
  trt_acm_fmt dst;
};

// ACMSTREAMHEADER (64-bit): cbStruct@0, fdwStatus@4, dwUser@8, pbSrc@16,
// cbSrcLength@24, cbSrcLengthUsed@28, dwSrcUser@32, pbDst@40, cbDstLength@48,
// cbDstLengthUsed@52, dwDstUser@56, dwReservedDriver@64.
constexpr u32 kAcmHdrBytes = 144;
constexpr u32 kAcmStatusDone = 0x00010000;
constexpr u32 kAcmStatusPrepared = 0x00020000;
constexpr u32 kAcmStatusInQueue = 0x00040000;
constexpr u32 kAcmOpenQuery = 0x00000001;     // ACM_STREAMOPENF_QUERY
constexpr u32 kAcmSizeFromDest = 0x20000000;  // ACM_STREAMSIZEF_DESTINATION

acm_stream* acm_stream_of(void* h) {
  if (!h) return nullptr;
  auto* s = (acm_stream*)h;
  return s->magic == kAcmStreamMagic ? s : nullptr;
}

u32* acm_hdr_u32(void* hdr, u32 off) { return (u32*)((u8*)hdr + off); }

bool acm_hdr_ok(const void* hdr, u32 cb) {
  if (!hdr || cb < kAcmHdrBytes) return false;
  return *(const u32*)hdr >= kAcmHdrBytes;
}

void acm_log_refused(const char* what, const trt_acm_fmt& src,
                     const trt_acm_fmt& dst) {
  char line[144];
  snprintf(line, sizeof(line),
           "msacm32: %s tag %u->%u NOTPOSSIBLE (no decoder for that pair)", what,
           src.tag, dst.tag);
  log_line(line);
}

}  // namespace

u32 TRT_GUEST_ABI acm_GetVersion(void) {
  return 0x02200000;  // ACM 2.2 (the Win7-era version every game accepts)
}

u32 TRT_GUEST_ABI acm_StreamOpen(void** has_h, void* had, const void* wfx_src,
                                 const void* wfx_dst, void* filter, u64 cb,
                                 u64 inst, u32 flags) {
  (void)filter; (void)cb; (void)inst;
  if (has_h) *has_h = nullptr;
  // A driver handle only admits that driver's codecs and we install none; the
  // documented answer is INVALHANDLE rather than a stream that cannot convert.
  if (had) return MMSYSERR_INVALHANDLE_;
  trt_acm_fmt src, dst;
  if (!trt_acm_parse(wfx_src, &src) || !trt_acm_parse(wfx_dst, &dst))
    return MMSYSERR_INVALPARAM_;
  if (!trt_acm_supported(src.tag) || !trt_acm_dst_supported(dst)) {
    acm_log_refused("StreamOpen", src, dst);
    return (u32)ACMERR_NOTPOSSIBLE_;
  }
  if (flags & kAcmOpenQuery) return MMSYSERR_NOERROR_;  // validate only
  if (!has_h) return MMSYSERR_INVALPARAM_;
  auto* s = new acm_stream();
  s->src = src;
  s->dst = dst;
  *has_h = (void*)s;
  return MMSYSERR_NOERROR_;
}
u32 TRT_GUEST_ABI acm_StreamPrepareHeader(void* h, void* hdr, u32 cb) {
  if (!acm_stream_of(h)) return MMSYSERR_INVALHANDLE_;
  if (!acm_hdr_ok(hdr, cb)) return MMSYSERR_INVALPARAM_;
  const u32 src_len = *acm_hdr_u32(hdr, 24);
  const u32 dst_len = *acm_hdr_u32(hdr, 48);
  if (!src_len || !dst_len) return MMSYSERR_INVALPARAM_;
  if (!*(const void* const*)((u8*)hdr + 16) || !*(void**)((u8*)hdr + 40))
    return MMSYSERR_INVALPARAM_;
  *acm_hdr_u32(hdr, 28) = 0;
  *acm_hdr_u32(hdr, 52) = 0;
  *acm_hdr_u32(hdr, 4) |= kAcmStatusPrepared | kAcmStatusDone;
  return MMSYSERR_NOERROR_;
}
u32 TRT_GUEST_ABI acm_StreamUnprepareHeader(void* h, void* hdr, u32 cb) {
  if (!acm_stream_of(h)) return MMSYSERR_INVALHANDLE_;
  if (!acm_hdr_ok(hdr, cb)) return MMSYSERR_INVALPARAM_;
  *acm_hdr_u32(hdr, 4) &=
      ~(kAcmStatusPrepared | kAcmStatusDone | kAcmStatusInQueue);
  return MMSYSERR_NOERROR_;
}
u32 TRT_GUEST_ABI acm_StreamConvert(void* h, void* hdr, u32 cb) {
  acm_stream* s = acm_stream_of(h);
  if (!s) return MMSYSERR_INVALHANDLE_;
  if (!acm_hdr_ok(hdr, cb)) return MMSYSERR_INVALPARAM_;
  const u32 status = *acm_hdr_u32(hdr, 4);
  if (!(status & kAcmStatusPrepared)) return (u32)ACMERR_UNPREPARED_;
  const void* src_p = *(const void* const*)((u8*)hdr + 16);
  void* dst_p = *(void**)((u8*)hdr + 40);
  const u32 src_len = *acm_hdr_u32(hdr, 24);
  const u32 dst_len = *acm_hdr_u32(hdr, 48);
  u32 used_in = 0, used_out = 0;
  if (trt_acm_convert(s->src, s->dst, src_p, src_len, dst_p, dst_len, &used_in,
                      &used_out) != 0) {
    acm_log_refused("StreamConvert", s->src, s->dst);
    return (u32)ACMERR_NOTPOSSIBLE_;
  }
  *acm_hdr_u32(hdr, 28) = used_in;
  *acm_hdr_u32(hdr, 52) = used_out;
  *acm_hdr_u32(hdr, 4) = (status | kAcmStatusDone) & ~kAcmStatusInQueue;
  return MMSYSERR_NOERROR_;
}
u32 TRT_GUEST_ABI acm_StreamReset(void* h, u64 flags) {
  (void)flags;
  return acm_stream_of(h) ? MMSYSERR_NOERROR_ : MMSYSERR_INVALHANDLE_;
}
u32 TRT_GUEST_ABI acm_StreamClose(void* h, u32) {
  acm_stream* s = acm_stream_of(h);
  if (!s) return MMSYSERR_INVALHANDLE_;  // double close is refused, not ignored
  s->magic = kAcmStreamDead;
  delete s;
  return MMSYSERR_NOERROR_;
}
u32 TRT_GUEST_ABI acm_StreamSize(void* h, u32 in_bytes, u32* out_bytes,
                                u32 flags) {
  acm_stream* s = acm_stream_of(h);
  if (!s) return MMSYSERR_INVALHANDLE_;
  if (!out_bytes) return MMSYSERR_INVALPARAM_;
  *out_bytes = (flags & kAcmSizeFromDest)
                   ? trt_acm_in_size(s->src, s->dst, in_bytes)
                   : trt_acm_out_size(s->src, s->dst, in_bytes);
  return *out_bytes ? MMSYSERR_NOERROR_ : (u32)ACMERR_NOTPOSSIBLE_;
}
u32 TRT_GUEST_ABI acm_FormatSuggest(void* had, const void* src, void* dst,
                                   u32 cb, u32 flags) {
  (void)had; (void)flags;
  trt_acm_fmt s;
  if (!src || !dst || cb < 18 || !trt_acm_parse(src, &s))
    return MMSYSERR_INVALPARAM_;
  // PCM at the source's channel count and rate: the shape the sink accepts.
  u8* p = (u8*)dst;
  memset(p, 0, cb);
  *(u16*)(p + 0) = TRT_ACM_TAG_PCM;
  *(u16*)(p + 2) = s.channels;
  *(u32*)(p + 4) = s.rate;
  *(u16*)(p + 14) = 16;
  *(u16*)(p + 12) = (u16)(s.channels * 2);
  *(u32*)(p + 8) = s.rate * s.channels * 2;
  return MMSYSERR_NOERROR_;
}
u32 TRT_GUEST_ABI acm_FormatTagEnum(void*, void* out, u32* size, u32) {
  // ACMFORMATTAGDETAILS: cbStruct(+0), dwFormatTagIndex(+4),
  // dwFormatTag(+8), cbFormatSize(+12), szFormatTag(+16...).
  // The list is exactly what the codec engine can decode; advertising more
  // would send a title into acmStreamOpen for a tag that must then fail.
  static const u16 k_tags[] = {TRT_ACM_TAG_PCM,     TRT_ACM_TAG_MS_ADPCM,
                               TRT_ACM_TAG_IMA_ADPCM, TRT_ACM_TAG_MULAW,
                               TRT_ACM_TAG_ALAW,    TRT_ACM_TAG_FLOAT};
  if (!out || !size || *size < 16) return MMSYSERR_ERROR_;
  u32* p = (u32*)out;
  const u32 idx = p[1];
  if (idx >= sizeof(k_tags) / sizeof(k_tags[0])) return MMSYSERR_ERROR_;
  p[2] = k_tags[idx];
  p[3] = 18;  // cbFormatSize of WAVEFORMATEX
  return MMSYSERR_NOERROR_;
}
u32 TRT_GUEST_ABI acm_FormatEnum(void*, void* out, u32* size, u32, u32) {
  // one PCM 44.1kHz stereo 16-bit format
  if (!out || !size || *size < 16) return MMSYSERR_ERROR_;
  u32* p = (u32*)out;
  u32 idx = p[1];
  if (idx == 0) {
    p[2] = 1;      // WAVE_FORMAT_PCM
    p[3] = 18;     // cbFormatSize
    return MMSYSERR_NOERROR_;
  }
  return MMSYSERR_ERROR_;
}
u32 TRT_GUEST_ABI acm_GetDriverHandle(void**, u32) {
  return MMSYSERR_ERROR_;  // no codecs installed
}
u32 TRT_GUEST_ABI acm_DriverEnum(void*, u32, u32) {
  return MMSYSERR_NOERROR_;  // zero drivers: engines stop probing
}
u32 TRT_GUEST_ABI acm_DriverID(void*, void**, u32) {
  return MMSYSERR_ERROR_;
}

// ===================== msvfw32 =====================
u32 TRT_GUEST_ABI msvfw_ICOpen(u32, u32, u32) {
  char line[80];
  snprintf(line, sizeof(line), "msvfw32: ICOpen (no AVI codecs)");
  log_line(line);
  return 0;  // NULL handle -> engine skips AVI logos
}
u32 TRT_GUEST_ABI msvfw_ICSendMessage(void*, u32, u32, u32) {
  return MMSYSERR_ERROR_;
}
u32 TRT_GUEST_ABI msvfw_ICInfo(u32, u32, void*) {
  return 0;  // no codec found
}
u32 TRT_GUEST_ABI msvfw_DrawDibOpen(void) {
  return 0;  // NULL -> engine falls back to D3D blits
}
u32 TRT_GUEST_ABI msvfw_AVISendMessage(void*, u32, u32, u32) {
  return MMSYSERR_ERROR_;
}
u32 TRT_GUEST_ABI msvfw_MCIWndRegisterClass(void) { return 0; }

// ===================== winspool.drv =====================
u32 TRT_GUEST_ABI wsp_OpenPrinterW(const void*, void** h, void*) {
  if (h) *h = nullptr;
  return 0;  // FALSE: no printer
}
u32 TRT_GUEST_ABI wsp_OpenPrinterA(const void*, void** h, void*) {
  if (h) *h = nullptr;
  return 0;
}
u32 TRT_GUEST_ABI wsp_ClosePrinterW(void*) { return 1; }
u32 TRT_GUEST_ABI wsp_EnumPrintersW(u32, const void*, u32, u8* buf, u32 buf_len,
                                    u32* needed, u32* returned) {
  if (needed) *needed = 0;
  if (returned) *returned = 0;
  (void)buf; (void)buf_len;
  char line[64];
  snprintf(line, sizeof(line), "winspool: EnumPrintersW (0 printers)");
  log_line(line);
  return 0;  // FALSE: no printers, engines skip print features
}
u32 TRT_GUEST_ABI wsp_EnumPrintersA(u32, const void*, u32, u8* buf, u32 buf_len,
                                    u32* needed, u32* returned) {
  return wsp_EnumPrintersW(0, nullptr, 0, buf, buf_len, needed, returned);
}
u32 TRT_GUEST_ABI wsp_GetDefaultPrinterW(void* buf, u32* size) {
  if (size) *size = 0;
  return 0;  // ERROR_FILE_NOT_FOUND by GetLastError semantics; FALSE
}
u32 TRT_GUEST_ABI wsp_GetPrinterW(void*, u32, u8*, u32, u32* needed) {
  if (needed) *needed = 0;
  return 0;
}

int trt_bridge_register_mmedia(void) {
  static const trt_named_fn k_acm[] = {
      {"acmGetVersion", (void*)&acm_GetVersion},
      {"acmStreamOpen", (void*)&acm_StreamOpen},
      {"acmStreamPrepareHeader", (void*)&acm_StreamPrepareHeader},
      {"acmStreamUnprepareHeader", (void*)&acm_StreamUnprepareHeader},
      {"acmStreamConvert", (void*)&acm_StreamConvert},
      {"acmStreamReset", (void*)&acm_StreamReset},
      {"acmStreamClose", (void*)&acm_StreamClose},
      {"acmStreamSize", (void*)&acm_StreamSize},
      {"acmFormatSuggest", (void*)&acm_FormatSuggest},
      {"acmFormatTagEnumW", (void*)&acm_FormatTagEnum},
      {"acmFormatTagEnumA", (void*)&acm_FormatTagEnum},
      {"acmFormatEnumW", (void*)&acm_FormatEnum},
      {"acmFormatEnumA", (void*)&acm_FormatEnum},
      {"acmGetDriverHandle", (void*)&acm_GetDriverHandle},
      {"acmDriverEnum", (void*)&acm_DriverEnum},
      {"acmDriverID", (void*)&acm_DriverID},
  };
  trt_module_register("msacm32.dll", TRT_ROUTE_BUILTIN, k_acm,
                      (int)(sizeof(k_acm) / sizeof(k_acm[0])));

  static const trt_named_fn k_vfw[] = {
      {"ICOpen", (void*)&msvfw_ICOpen},
      {"ICSendMessage", (void*)&msvfw_ICSendMessage},
      {"ICInfo", (void*)&msvfw_ICInfo},
      {"DrawDibOpen", (void*)&msvfw_DrawDibOpen},
      {"AVISendMessage", (void*)&msvfw_AVISendMessage},
      {"MCIWndRegisterClass", (void*)&msvfw_MCIWndRegisterClass},
  };
  trt_module_register("msvfw32.dll", TRT_ROUTE_BUILTIN, k_vfw,
                      (int)(sizeof(k_vfw) / sizeof(k_vfw[0])));

  static const trt_named_fn k_wsp[] = {
      {"OpenPrinterW", (void*)&wsp_OpenPrinterW},
      {"OpenPrinterA", (void*)&wsp_OpenPrinterA},
      {"ClosePrinterW", (void*)&wsp_ClosePrinterW},
      {"ClosePrinterA", (void*)&wsp_ClosePrinterW},
      {"EnumPrintersW", (void*)&wsp_EnumPrintersW},
      {"EnumPrintersA", (void*)&wsp_EnumPrintersA},
      {"GetDefaultPrinterW", (void*)&wsp_GetDefaultPrinterW},
      {"GetPrinterW", (void*)&wsp_GetPrinterW},
  };
  trt_module_register("winspool.drv", TRT_ROUTE_BUILTIN, k_wsp,
                      (int)(sizeof(k_wsp) / sizeof(k_wsp[0])));
  return 0;
}

}  // extern "C"
