// tiramisu/src/trt/bridge_audio.h
// XAudio2 for the guest (bridge_audio.cpp).
#ifndef TIRAMISU_BRIDGE_AUDIO_H
#define TIRAMISU_BRIDGE_AUDIO_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

int trt_bridge_register_audio(void);

// the console host publishes its real audio sink here
void trt_audio_set_hooks(int (*submit)(const void* pcm, u32 bytes, u32 rate,
                                       u32 channels, u32 bytes_per_sample,
                                       void* user),
                         void* user);

#ifdef __cplusplus
}
#endif

#endif
