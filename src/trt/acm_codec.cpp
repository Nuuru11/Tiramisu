// tiramisu/src/trt/acm_codec.cpp
// Real software decoders for the codecs msacm32 advertises:
//   PCM (8/16/24/32), IEEE float, MS ADPCM, IMA/DVI ADPCM, A-law, u-law.
// Decode produces interleaved s16 at the source rate; the converter then does
// channel mapping, linear resampling and destination packing. Every stage is
// deterministic and has no host dependency, so it is verifiable offline.
#include "acm_codec.h"

#include <string.h>

#include <vector>

namespace {

u16 rd16(const u8* p) { return (u16)((u32)p[0] | ((u32)p[1] << 8)); }
u32 rd32(const u8* p) {
  return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
i16 clamp16(i32 v) {
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return (i16)v;
}

bool is_compressed(u16 tag) {
  return tag == TRT_ACM_TAG_MS_ADPCM || tag == TRT_ACM_TAG_IMA_ADPCM;
}

// ---- IMA / DVI ADPCM (WAVE_FORMAT_IMA_ADPCM) ------------------------------
const int kImaStep[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,
    21,    23,    25,    28,    31,    34,    37,    41,    45,    50,    55,
    60,    66,    73,    80,    88,    97,    107,   118,   130,   143,   157,
    173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,
    494,   544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,
    1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,  3660,
    4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};
const int kImaIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8,
                           -1, -1, -1, -1, 2, 4, 6, 8};

void ima_nibble(i32* pred, i32* index, int nib, std::vector<i16>* out) {
  const i32 step = kImaStep[*index];
  i32 diff = step >> 3;
  if (nib & 1) diff += step >> 2;
  if (nib & 2) diff += step >> 1;
  if (nib & 4) diff += step;
  *pred += (nib & 8) ? -diff : diff;
  if (*pred > 32767) *pred = 32767;
  if (*pred < -32768) *pred = -32768;
  *index += kImaIndex[nib & 0xF];
  if (*index < 0) *index = 0;
  if (*index > 88) *index = 88;
  out->push_back((i16)*pred);
}

bool decode_ima(const trt_acm_fmt& f, const u8* p, u32 bytes,
                std::vector<i16>& out) {
  const u32 ch = f.channels ? f.channels : 1;
  const u32 ba = f.block_align;
  if (ch == 0 || ch > 8 || ba < 4 * ch) return false;
  u32 off = 0;
  while (off + ba <= bytes) {
    const u8* blk = p + off;
    off += ba;
    i32 pred[8], index[8];
    for (u32 c = 0; c < ch; ++c) {
      pred[c] = (i16)rd16(blk + c * 4);
      index[c] = blk[c * 4 + 2];
      if (index[c] > 88) index[c] = 88;
    }
    for (u32 c = 0; c < ch; ++c) out.push_back((i16)pred[c]);
    // 4 uncompressed bytes per channel per group, interleaved by channel.
    u32 pos = 4 * ch;
    while (pos + 4 * ch <= ba) {
      for (u32 c = 0; c < ch; ++c) {
        for (u32 k = 0; k < 4; ++k) {
          const u8 b = blk[pos++];
          ima_nibble(&pred[c], &index[c], (b >> 4) & 0xF, &out);
          ima_nibble(&pred[c], &index[c], b & 0xF, &out);
        }
      }
    }
  }
  return true;
}

// ---- Microsoft ADPCM (WAVE_FORMAT_ADPCM) ----------------------------------
const i32 kMsAdapt[16] = {230, 230, 230, 230, 307, 409, 512, 614,
                          768, 614, 512, 409, 307, 230, 230, 230};
const i32 kMsCoeff1[7] = {256, 512, 0, 192, 240, 460, 392};
const i32 kMsCoeff2[7] = {0, -256, 0, 64, 0, -208, -232};

i16 ms_nibble(i32* s1, i32* s2, i32* delta, int nib, u32 predictor) {
  i32 v = (*s1 * kMsCoeff1[predictor] + *s2 * kMsCoeff2[predictor]) / 256;
  const int sn = (nib & 0x8) ? nib - 16 : nib;
  v += sn * *delta;
  const i16 sample = clamp16(v);
  *delta = (kMsAdapt[nib & 0xF] * *delta) / 256;
  if (*delta < 16) *delta = 16;
  *s2 = *s1;
  *s1 = sample;
  return sample;
}

bool decode_ms_adpcm(const trt_acm_fmt& f, const u8* p, u32 bytes,
                     std::vector<i16>& out) {
  const u32 ch = f.channels ? f.channels : 1;
  const u32 ba = f.block_align;
  if (ch == 0 || ch > 2) return false;
  const u32 header = 7 * ch;  // bPredictor + iDelta[ch] + Samp1[ch] + Samp2[ch]
  if (ba <= header) return false;
  u32 off = 0;
  while (off + ba <= bytes) {
    const u8* blk = p + off;
    off += ba;
    u32 predictor = blk[0];
    if (predictor > 6) predictor = 0;
    i32 delta[2], s1[2], s2[2];
    u32 q = 1;
    for (u32 c = 0; c < ch; ++c) { delta[c] = (i16)rd16(blk + q); q += 2; }
    for (u32 c = 0; c < ch; ++c) { s1[c] = (i16)rd16(blk + q); q += 2; }
    for (u32 c = 0; c < ch; ++c) { s2[c] = (i16)rd16(blk + q); q += 2; }
    for (u32 c = 0; c < ch; ++c)
      if (delta[c] < 16) delta[c] = 16;
    // The first two frames of the block are stored uncompressed.
    for (u32 c = 0; c < ch; ++c) out.push_back((i16)s1[c]);
    for (u32 c = 0; c < ch; ++c) out.push_back((i16)s2[c]);
    for (u32 pos = header; pos < ba; ++pos) {
      const u8 b = blk[pos];
      if (ch == 2) {
        out.push_back(ms_nibble(&s1[0], &s2[0], &delta[0], (b >> 4) & 0xF,
                                predictor));
        out.push_back(ms_nibble(&s1[1], &s2[1], &delta[1], b & 0xF, predictor));
      } else {
        out.push_back(
            ms_nibble(&s1[0], &s2[0], &delta[0], (b >> 4) & 0xF, predictor));
        out.push_back(ms_nibble(&s1[0], &s2[0], &delta[0], b & 0xF, predictor));
      }
    }
  }
  return true;
}

// ---- G.711 companding -----------------------------------------------------
i16 mulaw_decode(u8 b) {
  const u32 u = (~b) & 0xFF;
  i32 t = (i32)(((u & 0x0F) << 3) + 0x84);
  t <<= (u & 0x70) >> 4;
  return (i16)((u & 0x80) ? (0x84 - t) : (t - 0x84));
}

i16 alaw_decode(u8 b) {
  const u32 a = (u32)b ^ 0x55;
  i32 t = (i32)((a & 0x0F) << 4);
  const u32 seg = (a & 0x70) >> 4;
  if (seg == 0) {
    t += 8;
  } else if (seg == 1) {
    t += 0x108;
  } else {
    t += 0x108;
    t <<= seg - 1;
  }
  return (i16)((a & 0x80) ? t : -t);
}

// ---- decoding front end ---------------------------------------------------
bool decode_all(const trt_acm_fmt& f, const u8* p, u32 bytes,
                std::vector<i16>& out) {
  const u32 ch = f.channels ? f.channels : 1;
  switch (f.tag) {
    case TRT_ACM_TAG_PCM: {
      if (f.bits == 8) {
        for (u32 i = 0; i < bytes; ++i)
          out.push_back((i16)(((i32)p[i] - 128) << 8));
        return true;
      }
      if (f.bits == 16) {
        const u32 n = bytes / 2;
        for (u32 i = 0; i < n; ++i) out.push_back((i16)rd16(p + i * 2));
        return true;
      }
      if (f.bits == 24) {
        const u32 n = bytes / 3;
        for (u32 i = 0; i < n; ++i) {
          const u8* q = p + (size_t)i * 3;
          i32 v = (i32)((u32)q[0] | ((u32)q[1] << 8) | ((u32)q[2] << 16));
          if (v & 0x800000) v -= 0x1000000;
          out.push_back((i16)(v >> 8));
        }
        return true;
      }
      if (f.bits == 32) {
        const u32 n = bytes / 4;
        for (u32 i = 0; i < n; ++i)
          out.push_back((i16)((i32)rd32(p + i * 4) >> 16));
        return true;
      }
      return false;
    }
    case TRT_ACM_TAG_FLOAT: {
      if (f.bits != 32) return false;
      const u32 n = bytes / 4;
      for (u32 i = 0; i < n; ++i) {
        float v;
        const u32 raw = rd32(p + i * 4);
        memcpy(&v, &raw, 4);
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        out.push_back((i16)(i32)(v * 32767.0f));
      }
      return true;
    }
    case TRT_ACM_TAG_MULAW:
      for (u32 i = 0; i < bytes; ++i) out.push_back(mulaw_decode(p[i]));
      return true;
    case TRT_ACM_TAG_ALAW:
      for (u32 i = 0; i < bytes; ++i) out.push_back(alaw_decode(p[i]));
      return true;
    case TRT_ACM_TAG_IMA_ADPCM:
      return decode_ima(f, p, bytes, out);
    case TRT_ACM_TAG_MS_ADPCM:
      return decode_ms_adpcm(f, p, bytes, out);
    default:
      (void)ch;
      return false;
  }
}

// ---- channel mapping ------------------------------------------------------
bool channel_map(const std::vector<i16>& in, u32 sc, u32 dc,
                 std::vector<i16>& out) {
  if (sc == dc) {
    out = in;
    return true;
  }
  if (!sc || !dc) return false;
  const size_t frames = in.size() / sc;
  out.clear();
  out.reserve(frames * dc);
  if (sc == 2 && dc == 1) {
    for (size_t i = 0; i < frames; ++i)
      out.push_back((i16)(((i32)in[i * 2] + in[i * 2 + 1]) / 2));
    return true;
  }
  if (sc == 1 && dc == 2) {
    for (size_t i = 0; i < frames; ++i) {
      out.push_back(in[i]);
      out.push_back(in[i]);
    }
    return true;
  }
  if (sc == 1) {  // mono fills every destination channel
    for (size_t i = 0; i < frames; ++i)
      for (u32 c = 0; c < dc; ++c) out.push_back(in[i]);
    return true;
  }
  if (dc == 1) {  // downmix everything to mono
    for (size_t i = 0; i < frames; ++i) {
      i32 sum = 0;
      for (u32 c = 0; c < sc; ++c) sum += in[i * sc + c];
      out.push_back((i16)(sum / (i32)sc));
    }
    return true;
  }
  return false;  // 5.1/7.1 channel remaps are not guessed
}

// ---- linear resampling ----------------------------------------------------
std::vector<i16> resample(const std::vector<i16>& in, u32 ch, u32 sr, u32 dr) {
  if (sr == dr || !sr || !dr) return in;
  const size_t frames = in.size() / ch;
  if (!frames) return {};
  const size_t out_frames = (size_t)(((u64)frames * dr) / sr);
  std::vector<i16> out(out_frames * ch);
  for (size_t j = 0; j < out_frames; ++j) {
    const u64 num = (u64)j * sr;
    const size_t i0 = (size_t)(num / dr);
    const double frac = (double)(num % dr) / (double)dr;
    const size_t i1 = i0 + 1 < frames ? i0 + 1 : frames - 1;
    for (u32 c = 0; c < ch; ++c) {
      const double a = (double)in[i0 * ch + c];
      const double b = (double)in[i1 * ch + c];
      out[j * ch + c] = (i16)(a + (b - a) * frac);
    }
  }
  return out;
}

bool pack(const std::vector<i16>& in, u16 bits, u8* out, u32 cap,
          u32* written) {
  const size_t n = in.size();
  const size_t need = n * (bits / 8);
  if (need > cap) return false;
  if (bits == 8) {
    for (size_t i = 0; i < n; ++i) out[i] = (u8)((in[i] >> 8) + 128);
  } else if (bits == 16) {
    for (size_t i = 0; i < n; ++i) {
      out[i * 2] = (u8)(in[i] & 0xFF);
      out[i * 2 + 1] = (u8)((u16)in[i] >> 8);
    }
  } else if (bits == 24) {
    if (need > cap) return false;
    for (size_t i = 0; i < n; ++i) {
      const i32 v = (i32)in[i] << 8;
      out[i * 3] = (u8)v;
      out[i * 3 + 1] = (u8)(v >> 8);
      out[i * 3 + 2] = (u8)(v >> 16);
    }
  } else if (bits == 32) {
    for (size_t i = 0; i < n; ++i) {
      const i32 v = (i32)in[i] << 16;
      out[i * 4] = (u8)v;
      out[i * 4 + 1] = (u8)(v >> 8);
      out[i * 4 + 2] = (u8)(v >> 16);
      out[i * 4 + 3] = (u8)(v >> 24);
    }
  } else {
    return false;
  }
  *written = (u32)need;
  return true;
}

u32 derive_samples_per_block(const trt_acm_fmt& f) {
  const u32 ch = f.channels ? f.channels : 1;
  const u32 ba = f.block_align;
  if (f.tag == TRT_ACM_TAG_MS_ADPCM) {
    if (ba <= 7 * ch) return 0;
    return (u32)(((ba - 7 * ch) * 2) / ch) + 2;
  }
  if (f.tag == TRT_ACM_TAG_IMA_ADPCM) {
    if (ba < 4 * ch) return 0;
    return (u32)(((ba - 4 * ch) * 2) / ch) + 1;
  }
  return 1;  // PCM/float: one frame per block_align bytes
}

u32 frames_for(const trt_acm_fmt& f, u32 bytes) {
  const u32 ch = f.channels ? f.channels : 1;
  if (!is_compressed(f.tag)) {
    const u32 ba = f.block_align ? f.block_align : (u32)(ch * (f.bits / 8 ? f.bits / 8 : 1));
    return ba ? bytes / ba : 0;
  }
  if (!f.block_align) return 0;
  const u32 spb = f.samples_per_block ? f.samples_per_block
                                      : derive_samples_per_block(f);
  return (bytes / f.block_align) * spb;
}

u32 bytes_for_frames(const trt_acm_fmt& f, u32 frames) {
  if (!is_compressed(f.tag)) {
    const u32 ch = f.channels ? f.channels : 1;
    const u32 ba = f.block_align ? f.block_align : (u32)(ch * (f.bits / 8 ? f.bits / 8 : 1));
    return frames * ba;
  }
  const u32 spb = f.samples_per_block ? f.samples_per_block
                                      : derive_samples_per_block(f);
  if (!spb || !f.block_align) return 0;
  const u32 blocks = (frames + spb - 1) / spb;
  return blocks * f.block_align;
}

}  // namespace

bool trt_acm_parse(const void* wfx, trt_acm_fmt* out) {
  if (!wfx || !out) return false;
  const u8* p = (const u8*)wfx;
  trt_acm_fmt f;
  f.tag = rd16(p + 0);
  f.channels = rd16(p + 2);
  f.rate = rd32(p + 4);
  f.avg_bytes = rd32(p + 8);
  f.block_align = rd16(p + 12);
  f.bits = rd16(p + 14);
  f.extra_size = rd16(p + 16);
  if (f.extra_size >= 2) f.samples_per_block = rd16(p + 18);
  if (!f.channels || f.channels > 8 || !f.rate) return false;
  if (is_compressed(f.tag)) {
    if (!f.samples_per_block) f.samples_per_block = (u16)derive_samples_per_block(f);
    if (!f.samples_per_block || !f.block_align) return false;
  } else if (!f.block_align) {
    const u32 unit = f.bits / 8;
    if (!unit) return false;
    f.block_align = (u16)(f.channels * unit);
  }
  *out = f;
  return true;
}

bool trt_acm_supported(u16 tag) {
  switch (tag) {
    case TRT_ACM_TAG_PCM:
    case TRT_ACM_TAG_MS_ADPCM:
    case TRT_ACM_TAG_FLOAT:
    case TRT_ACM_TAG_ALAW:
    case TRT_ACM_TAG_MULAW:
    case TRT_ACM_TAG_IMA_ADPCM:
      return true;
    default:
      return false;
  }
}

bool trt_acm_dst_supported(const trt_acm_fmt& f) {
  if (f.tag != TRT_ACM_TAG_PCM && f.tag != TRT_ACM_TAG_FLOAT) return false;
  return f.bits == 8 || f.bits == 16 || f.bits == 24 || f.bits == 32;
}

u32 trt_acm_out_size(const trt_acm_fmt& src, const trt_acm_fmt& dst,
                     u32 in_bytes) {
  if (!trt_acm_dst_supported(dst) || !trt_acm_supported(src.tag)) return 0;
  const u32 frames = frames_for(src, in_bytes);
  if (!frames) return 0;
  const u32 out_frames = (u32)(((u64)frames * dst.rate) / src.rate);
  return out_frames * dst.block_align;
}

u32 trt_acm_in_size(const trt_acm_fmt& src, const trt_acm_fmt& dst,
                    u32 out_bytes) {
  if (!trt_acm_dst_supported(dst) || !trt_acm_supported(src.tag)) return 0;
  if (!dst.block_align) return 0;
  const u32 out_frames = out_bytes / dst.block_align;
  if (!out_frames) return 0;
  // Round UP the source frame count: a destination frame may straddle two
  // source frames when downsampling. An exact multiple must not gain a block.
  const u64 num = (u64)out_frames * src.rate;
  const u64 frames = (num + dst.rate - 1) / dst.rate;
  return bytes_for_frames(src, (u32)frames);
}

int trt_acm_convert(const trt_acm_fmt& src, const trt_acm_fmt& dst,
                    const void* in, u32 in_bytes, void* out, u32 out_cap,
                    u32* used_in, u32* used_out) {
  if (used_in) *used_in = 0;
  if (used_out) *used_out = 0;
  if (!in || !out) return -1;
  if (!trt_acm_dst_supported(dst) || !trt_acm_supported(src.tag)) return -1;
  if (!dst.block_align || !dst.channels) return -1;

  u32 consumed = in_bytes;
  if (is_compressed(src.tag)) {
    if (!src.block_align) return -1;
    consumed = (in_bytes / src.block_align) * src.block_align;  // whole blocks
  }
  if (!consumed) return -1;

  std::vector<i16> samples;
  samples.reserve(((size_t)consumed / (src.block_align ? src.block_align : 1) + 1) *
                  src.channels * 4);
  if (!decode_all(src, (const u8*)in, consumed, samples)) return -1;
  if (samples.empty()) return -1;
  if (samples.size() % src.channels) samples.resize(samples.size() - samples.size() % src.channels);
  if (samples.empty()) return -1;

  std::vector<i16> chmap;
  if (!channel_map(samples, src.channels, dst.channels, chmap)) return -1;
  std::vector<i16> res = resample(chmap, dst.channels, src.rate, dst.rate);

  // Truncate to what the destination buffer can physically hold.
  const u32 frame_bytes = (u32)dst.channels * (dst.bits / 8);
  if (!frame_bytes) return -1;
  u32 frames = (u32)(res.size() / dst.channels);
  const u32 max_frames = out_cap / frame_bytes;
  if (frames > max_frames) {
    frames = max_frames;
    res.resize((size_t)frames * dst.channels);
  }
  u32 written = 0;
  if (!pack(res, dst.bits, (u8*)out, out_cap, &written)) return -1;
  if (used_in) *used_in = consumed;
  if (used_out) *used_out = written;
  return 0;
}
