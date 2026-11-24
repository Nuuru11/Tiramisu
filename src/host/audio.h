// tiramisu/src/host/audio.h
// Real audio output on console (audio.cpp).
#ifndef TIRAMISU_HOST_AUDIO_H
#define TIRAMISU_HOST_AUDIO_H

#include <cstdint>

namespace longan {

void audio_init();      // create the XAudio2 sink, publish hooks to the bridge
void audio_status(char* out, unsigned cap);
void audio_play_pcm_chunk(uint64_t voice_id, const void* pcm, uint32_t bytes,
                          uint32_t rate, uint32_t channels, uint32_t bps,
                          float volume = 1.0f);
void audio_stop_voice(uint64_t voice_id);
void audio_shutdown();

} // namespace longan

#endif
