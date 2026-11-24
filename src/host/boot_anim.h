// tiramisu/src/host/boot_anim.h
// Native HD boot animation player (decodes packaged boot_anim.tanim JPEG frames
// + streams original stereo PCM audio through XAudio2 with smooth crossfade).
#ifndef TIRAMISU_HOST_BOOT_ANIM_H
#define TIRAMISU_HOST_BOOT_ANIM_H

#include <cstdint>
#include <string>
#include <vector>

namespace longan {

// Load and begin playing the startup boot animation from path (non-blocking).
bool boot_anim_start(const std::string& tanim_path);

// True while the boot animation (including its final fade-out) is active.
bool boot_anim_is_active();

// Skip/stop the boot animation immediately and release its memory.
void boot_anim_skip();

// Advance clock, stream audio slice, and return the current premultiplied BGRA
// frame when active. out_opacity fades 1.0 -> 0.0 during the final 0.45s.
bool boot_anim_get_frame(std::vector<uint8_t>* out_bgra, unsigned* out_w,
                         unsigned* out_h, uint64_t* out_frame_id,
                         float* out_opacity);

} // namespace longan

#endif // TIRAMISU_HOST_BOOT_ANIM_H
