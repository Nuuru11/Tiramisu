// tiramisu/src/host/audio.h
// Real audio output on console (audio.cpp).
#ifndef TIRAMISU_HOST_AUDIO_H
#define TIRAMISU_HOST_AUDIO_H

namespace longan {

void audio_init();      // create the XAudio2 sink, publish hooks to the bridge
void audio_status(char* out, unsigned cap);
void audio_shutdown();

} // namespace longan

#endif
