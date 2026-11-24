// tiramisu/src/host/ui_sfx.h
// Subtle PSP / PS5-style procedural UI navigation sound effects synthesized on
// the fly into 48 kHz stereo 16-bit PCM and mixed through XAudio2 (audio.h).
#ifndef TIRAMISU_HOST_UI_SFX_H
#define TIRAMISU_HOST_UI_SFX_H

#include "audio.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace longan {

namespace ui_sfx_detail {

constexpr uint32_t k_rate = 48000u;
constexpr double k_tau = 6.28318530717958647692;

inline uint64_t next_voice_id() {
  static std::atomic<uint64_t> seq{0x5549534658000000ULL};
  // Rotate across 6 short-lived UI SFX voice slots so rapid D-pad scrolling
  // overlaps naturally without piling up voices.
  return 0x5549534658000000ULL + (seq.fetch_add(1) % 6u);
}

inline void emit_tone(double dur_sec, double f0, double f1, double f2_ratio,
                      double attack_sec, double decay_exp, float vol) {
  const uint32_t frames =
      std::max(64u, static_cast<uint32_t>(dur_sec * (double)k_rate));
  std::vector<int16_t> pcm((size_t)frames * 2u);
  double phase1 = 0.0;
  double phase2 = 0.0;
  for (uint32_t i = 0; i < frames; ++i) {
    const double t = (double)i / (double)k_rate;
    const double u = (double)i / (double)frames;
    const double freq = f0 + (f1 - f0) * (1.0 - (1.0 - u) * (1.0 - u));
    phase1 += k_tau * freq / (double)k_rate;
    phase2 += k_tau * (freq * f2_ratio) / (double)k_rate;

    const double atk = (attack_sec > 0.0 && t < attack_sec) ? (t / attack_sec) : 1.0;
    const double rel = std::exp(-decay_exp * u) * (1.0 - u * u * u);
    const double env = atk * rel;
    const double sig = (0.76 * std::sin(phase1) + 0.24 * std::sin(phase2)) * env;
    const int16_t s = static_cast<int16_t>(
        std::clamp(sig * 32767.0, -32767.0, 32767.0));
    pcm[(size_t)i * 2u + 0u] = s;
    pcm[(size_t)i * 2u + 1u] = s;
  }
  audio_play_pcm_chunk(next_voice_id(), pcm.data(),
                       static_cast<uint32_t>(pcm.size() * sizeof(int16_t)),
                       k_rate, 2u, 2u, vol);
}

inline void emit_chord(double dur_sec, double fa, double fb, double fc,
                       float vol) {
  const uint32_t frames =
      std::max(64u, static_cast<uint32_t>(dur_sec * (double)k_rate));
  std::vector<int16_t> pcm((size_t)frames * 2u);
  for (uint32_t i = 0; i < frames; ++i) {
    const double t = (double)i / (double)k_rate;
    const double u = (double)i / (double)frames;
    const double atk = std::min(1.0, t / 0.012);
    const double env = atk * std::exp(-4.2 * u) * (1.0 - u * u);
    const double s_l = (0.45 * std::sin(k_tau * fa * t) +
                        0.33 * std::sin(k_tau * fb * t) +
                        0.22 * std::sin(k_tau * fc * t)) *
                       env;
    const double s_r = (0.45 * std::sin(k_tau * (fa * 1.0015) * t) +
                        0.33 * std::sin(k_tau * (fb * 0.9985) * t) +
                        0.22 * std::sin(k_tau * fc * t)) *
                       env;
    pcm[(size_t)i * 2u + 0u] = static_cast<int16_t>(
        std::clamp(s_l * 32767.0, -32767.0, 32767.0));
    pcm[(size_t)i * 2u + 1u] = static_cast<int16_t>(
        std::clamp(s_r * 32767.0, -32767.0, 32767.0));
  }
  audio_play_pcm_chunk(next_voice_id(), pcm.data(),
                       static_cast<uint32_t>(pcm.size() * sizeof(int16_t)),
                       k_rate, 2u, 2u, vol);
}

} // namespace ui_sfx_detail

// Crisp 24ms PSP-style focus tick
inline void ui_sfx_move() {
  ui_sfx_detail::emit_tone(0.026, 920.0, 1340.0, 2.0, 0.002, 7.5, 0.14f);
}

// Smooth 55ms airy glass tab-switch chime
inline void ui_sfx_tab() {
  ui_sfx_detail::emit_tone(0.058, 540.0, 810.0, 1.5, 0.005, 5.2, 0.16f);
}

// Warm PlayStation-style confirm chime
inline void ui_sfx_confirm() {
  ui_sfx_detail::emit_chord(0.130, 587.33, 880.0, 1174.66, 0.18f);
}

// Soft descending muted back tone
inline void ui_sfx_back() {
  ui_sfx_detail::emit_tone(0.062, 520.0, 340.0, 1.5, 0.003, 5.8, 0.14f);
}

// Cinematic game-launch power chord
inline void ui_sfx_launch() {
  ui_sfx_detail::emit_chord(0.280, 523.25, 783.99, 1046.50, 0.22f);
}

// Subtle low confirmation tone for delete/uninstall
inline void ui_sfx_delete() {
  ui_sfx_detail::emit_tone(0.110, 330.0, 220.0, 1.5, 0.004, 5.0, 0.16f);
}

} // namespace longan

#endif // TIRAMISU_HOST_UI_SFX_H
