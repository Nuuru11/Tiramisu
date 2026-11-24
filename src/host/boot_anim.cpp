// tiramisu/src/host/boot_anim.cpp
// Self-contained HD boot animation player for Xbox Series X|S UWP.
#include "boot_anim.h"
#include "audio.h"
#include "host_log.h"
#include "vendor/stb_image.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace longan {

namespace {

constexpr uint64_t k_boot_voice_id = 0x544952414D495355ULL;

struct anim_state {
  bool active = false;
  uint32_t fps = 24;
  uint32_t frame_count = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t audio_rate = 0;
  uint32_t audio_channels = 0;
  uint32_t audio_bytes = 0;

  std::vector<uint8_t> raw;
  std::vector<uint32_t> frame_off;
  std::vector<uint32_t> frame_len;
  uint32_t audio_off = 0;
  uint32_t audio_sent = 0;

  ULONGLONG start_ms = 0;
  int64_t decoded_idx = -1;
  std::vector<uint8_t> bgra;
  unsigned dec_w = 0;
  unsigned dec_h = 0;
  uint64_t frame_ver = 0;
};

std::mutex g_mu;
anim_state g_anim;

uint32_t rd_u32(const uint8_t* p) {
  uint32_t v = 0;
  memcpy(&v, p, 4);
  return v;
}

void release_locked() {
  g_anim.active = false;
  g_anim.raw.clear();
  g_anim.raw.shrink_to_fit();
  g_anim.frame_off.clear();
  g_anim.frame_off.shrink_to_fit();
  g_anim.frame_len.clear();
  g_anim.frame_len.shrink_to_fit();
  g_anim.bgra.clear();
  g_anim.bgra.shrink_to_fit();
  g_anim.decoded_idx = -1;
}

bool decode_frame_locked(uint32_t idx) {
  if (idx >= g_anim.frame_count) return false;
  const uint32_t off = g_anim.frame_off[idx];
  const uint32_t len = g_anim.frame_len[idx];
  if (off + len > g_anim.raw.size() || len < 16) return false;

  int w = 0, h = 0, ch = 0;
  unsigned char* rgba =
      stbi_load_from_memory(g_anim.raw.data() + off, (int)len, &w, &h, &ch, 4);
  if (!rgba || w <= 0 || h <= 0) {
    if (rgba) stbi_image_free(rgba);
    return false;
  }
  const size_t n = (size_t)w * (size_t)h;
  g_anim.bgra.resize(n * 4);
  uint8_t* dst = g_anim.bgra.data();
  for (size_t i = 0; i < n; ++i) {
    dst[i * 4 + 0] = rgba[i * 4 + 2];
    dst[i * 4 + 1] = rgba[i * 4 + 1];
    dst[i * 4 + 2] = rgba[i * 4 + 0];
    dst[i * 4 + 3] = 255;
  }
  stbi_image_free(rgba);
  g_anim.dec_w = (unsigned)w;
  g_anim.dec_h = (unsigned)h;
  g_anim.decoded_idx = (int64_t)idx;
  g_anim.frame_ver++;
  return true;
}

void pump_audio_locked(double elapsed_sec) {
  if (g_anim.audio_bytes == 0 || g_anim.audio_rate == 0 ||
      g_anim.audio_channels == 0)
    return;
  if (g_anim.audio_sent >= g_anim.audio_bytes) return;
  const uint32_t frame_bytes = g_anim.audio_channels * 2u;
  // Keep ~350ms buffered ahead of playback cursor inside XAudio2 FIFO
  const double target_sec = elapsed_sec + 0.35;
  uint32_t want = (uint32_t)(target_sec * (double)g_anim.audio_rate) * frame_bytes;
  if (want > g_anim.audio_bytes) want = g_anim.audio_bytes;
  if (want <= g_anim.audio_sent) return;

  uint32_t chunk = want - g_anim.audio_sent;
  // Cap per-call submission to 0.5s so XAudio2's 2s FIFO never drops
  const uint32_t max_chunk = (g_anim.audio_rate / 2u) * frame_bytes;
  if (chunk > max_chunk) chunk = max_chunk;
  chunk = (chunk / frame_bytes) * frame_bytes;
  if (chunk == 0) return;

  const uint32_t src_off = g_anim.audio_off + g_anim.audio_sent;
  if (src_off + chunk > g_anim.raw.size()) return;
  audio_play_pcm_chunk(k_boot_voice_id, g_anim.raw.data() + src_off, chunk,
                       g_anim.audio_rate, g_anim.audio_channels, 2u, 0.92f);
  g_anim.audio_sent += chunk;
}

} // namespace

bool boot_anim_start(const std::string& tanim_path) {
  FILE* f = fopen(tanim_path.c_str(), "rb");
  if (!f) {
    log_line(("boot_anim: not found at " + tanim_path).c_str());
    return false;
  }
  fseek(f, 0, SEEK_END);
  const long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz < 64 || sz > 64 * 1024 * 1024) {
    fclose(f);
    log_line("boot_anim: invalid file size");
    return false;
  }
  std::vector<uint8_t> buf((size_t)sz);
  const size_t got = fread(buf.data(), 1, (size_t)sz, f);
  fclose(f);
  if (got < 32 || memcmp(buf.data(), "TANM", 4) != 0) {
    log_line("boot_anim: bad magic");
    return false;
  }

  const uint32_t fps = std::max(1u, rd_u32(buf.data() + 4));
  const uint32_t nframes = rd_u32(buf.data() + 8);
  const uint32_t w = rd_u32(buf.data() + 12);
  const uint32_t h = rd_u32(buf.data() + 16);
  const uint32_t arate = rd_u32(buf.data() + 20);
  const uint32_t ach = rd_u32(buf.data() + 24);
  const uint32_t abytes = rd_u32(buf.data() + 28);
  const size_t table_end = 32u + (size_t)nframes * 4u;
  if (nframes == 0 || nframes > 2400 || table_end > got) {
    log_line("boot_anim: invalid header");
    return false;
  }

  std::vector<uint32_t> foff(nframes), flen(nframes);
  size_t cur = table_end;
  for (uint32_t i = 0; i < nframes; ++i) {
    const uint32_t l = rd_u32(buf.data() + 32u + i * 4u);
    if (cur + l > got) {
      log_line("boot_anim: truncated frame table");
      return false;
    }
    foff[i] = (uint32_t)cur;
    flen[i] = l;
    cur += l;
  }
  const uint32_t aoff = (uint32_t)cur;
  const uint32_t clamped_abytes =
      (cur + abytes <= got) ? abytes : (uint32_t)(got - cur);

  std::lock_guard<std::mutex> lk(g_mu);
  g_anim.active = true;
  g_anim.fps = fps;
  g_anim.frame_count = nframes;
  g_anim.width = w;
  g_anim.height = h;
  g_anim.audio_rate = arate;
  g_anim.audio_channels = ach;
  g_anim.audio_bytes = clamped_abytes;
  g_anim.raw = std::move(buf);
  g_anim.frame_off = std::move(foff);
  g_anim.frame_len = std::move(flen);
  g_anim.audio_off = aoff;
  g_anim.audio_sent = 0;
  g_anim.start_ms = GetTickCount64();
  g_anim.decoded_idx = -1;
  g_anim.frame_ver = 0;
  decode_frame_locked(0);
  pump_audio_locked(0.0);

  char msg[160];
  snprintf(msg, sizeof(msg),
           "boot_anim: started %ux%u @ %u fps (%u frames, audio=%u bytes)",
           w, h, fps, nframes, clamped_abytes);
  log_line(msg);
  return true;
}

bool boot_anim_is_active() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_anim.active;
}

void boot_anim_skip() {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_anim.active) return;
    release_locked();
  }
  audio_stop_voice(k_boot_voice_id);
  log_line("boot_anim: skipped by user");
}

bool boot_anim_get_frame(std::vector<uint8_t>* out_bgra, unsigned* out_w,
                         unsigned* out_h, uint64_t* out_frame_id,
                         float* out_opacity) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_anim.active) return false;

  const double elapsed_sec =
      (double)(GetTickCount64() - g_anim.start_ms) * 0.001;
  const double total_sec = (double)g_anim.frame_count / (double)g_anim.fps;
  const double fade_sec = 0.45;
  if (elapsed_sec >= total_sec + 0.05) {
    release_locked();
    return false;
  }

  pump_audio_locked(elapsed_sec);

  uint32_t idx = (uint32_t)(elapsed_sec * (double)g_anim.fps);
  if (idx >= g_anim.frame_count) idx = g_anim.frame_count - 1;
  if ((int64_t)idx != g_anim.decoded_idx) {
    decode_frame_locked(idx);
  }
  if (g_anim.bgra.empty()) return false;

  float op = 1.0f;
  const double rem = total_sec - elapsed_sec;
  if (rem < fade_sec) {
    op = (float)std::clamp(rem / fade_sec, 0.0, 1.0);
  }

  if (out_opacity) *out_opacity = op;
  if (out_w) *out_w = g_anim.dec_w;
  if (out_h) *out_h = g_anim.dec_h;
  if (out_frame_id) *out_frame_id = g_anim.frame_ver;
  if (out_bgra) *out_bgra = g_anim.bgra;
  return true;
}

} // namespace longan
