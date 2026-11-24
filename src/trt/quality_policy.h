// tiramisu/src/trt/quality_policy.h
// What the `texture_quality` setting means, in one place.
//
// The setting (0=low, 1=medium, 2=high, 3=ultra) was previously a knob with no
// consumer. Anisotropic filtering is the honest lever for it on a console: it
// buys sharpness on angled surfaces at real bandwidth cost, and every guest
// already states the maximum it wants, so a tier can only ever lower that
// request - it can never make the image differ from what the title asked for
// beyond the detail the tier permits.
//
// The functions are pure and header-only so the mapping can be checked without
// a device; bridge_d3d12 applies them to the sampler records it mirrors.
#ifndef TIRAMISU_QUALITY_POLICY_H
#define TIRAMISU_QUALITY_POLICY_H

#include "trt.h"

// Highest anisotropy this tier allows. High and ultra are both 16, which is
// the hardware limit, so the default (2 = high) changes nothing at all.
static inline u32 trt_quality_aniso_cap(int quality) {
  switch (quality) {
    case 0: return 1;   // low: isotropic
    case 1: return 4;   // medium
    case 3: return 16;  // ultra
    case 2:
    default: return 16; // high (default) and out-of-range values are full quality
  }
}

// Clamp a guest's requested MaxAnisotropy to the tier. Never raises it: a title
// asking for 1 stays at 1 even on ultra.
static inline u32 trt_quality_clamp_aniso(int quality, u32 requested) {
  const u32 cap = trt_quality_aniso_cap(quality);
  if (requested == 0) return 1;      // D3D12 rejects 0; 1 = no anisotropy
  if (requested > 16u) return cap;   // invalid request: fall back to the tier
  return requested < cap ? requested : cap;
}

// True when the clamp actually lowered the guest's request (worth one log line
// per tier at startup, not one per sampler).
static inline bool trt_quality_aniso_clamped(int quality, u32 requested) {
  return requested > trt_quality_clamp_aniso(quality, requested);
}

#endif
