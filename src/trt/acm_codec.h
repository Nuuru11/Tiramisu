// tiramisu/src/trt/acm_codec.h
// The decoding half of msacm32.
//
// Before this file existed, every acmStreamOpen that was not PCM->PCM answered
// ACMERR_NOTPOSSIBLE, and the ACM surface was classified whole-module
// named-silent. Games with IMA/MS-ADPCM or u-law/A-law WAV assets (very common
// in the Xbox 360 era, and still in indie titles) got silence with no way to
// know why.
//
// This is a real decoder: it turns compressed source bytes into the PCM the
// console sink and the guest both understand, and it performs the PCM format
// conversion (bits / channels / sample rate) that acmStreamSize and
// acmStreamConvert are documented to do. It decodes whole blocks only, which is
// exactly what the ACMSTREAMHEADER contract requires.
//
// It is deliberately self-contained (no FFmpeg, no host codec DLLs): the
// codecs are small and fully specified, and a deterministic software decoder is
// testable offline against hand-computed vectors.
#ifndef TIRAMISU_ACM_CODEC_H
#define TIRAMISU_ACM_CODEC_H

#include "trt.h"

// WAVE_FORMAT_* tags this engine decodes or accepts as a destination.
constexpr u16 TRT_ACM_TAG_PCM = 0x0001;
constexpr u16 TRT_ACM_TAG_MS_ADPCM = 0x0002;
constexpr u16 TRT_ACM_TAG_FLOAT = 0x0003;
constexpr u16 TRT_ACM_TAG_ALAW = 0x0006;
constexpr u16 TRT_ACM_TAG_MULAW = 0x0007;
constexpr u16 TRT_ACM_TAG_IMA_ADPCM = 0x0011;

struct trt_acm_fmt {
  u16 tag = 0;
  u16 channels = 0;
  u32 rate = 0;
  u32 avg_bytes = 0;
  u16 block_align = 0;
  u16 bits = 0;
  u16 extra_size = 0;
  u16 samples_per_block = 0;  // compressed only; derived when the header omits it
};

// Parse a WAVEFORMATEX (+ trailing codec data). false = structurally unusable.
bool trt_acm_parse(const void* wfx, trt_acm_fmt* out);

// Can we decode this tag?
bool trt_acm_supported(u16 tag);
// Can this format be an ACM destination? (PCM and IEEE float only)
bool trt_acm_dst_supported(const trt_acm_fmt& f);

// Source bytes -> destination bytes (feeds acmStreamSize with SIZE_FROM_SOURCE).
// 0 = the conversion is impossible.
u32 trt_acm_out_size(const trt_acm_fmt& src, const trt_acm_fmt& dst, u32 in_bytes);
// Destination bytes -> source bytes (feeds acmStreamSize with SIZE_FROM_DEST).
u32 trt_acm_in_size(const trt_acm_fmt& src, const trt_acm_fmt& dst, u32 out_bytes);

// Decode + channel-convert + resample + repack. Consumes whole blocks of the
// source. Returns 0 on success, -1 when the format pair cannot be converted,
// and writes how many source bytes were consumed and destination bytes written.
int trt_acm_convert(const trt_acm_fmt& src, const trt_acm_fmt& dst,
                    const void* in, u32 in_bytes, void* out, u32 out_cap,
                    u32* used_in, u32* used_out);

#endif  // TIRAMISU_ACM_CODEC_H
