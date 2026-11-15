// tiramisu/src/trt/bridge_openal.h
// OpenAL 1.1 for the guest (bridge_openal.cpp).
//
// OpenAL is the audio API of the Minecraft generation: alGenBuffers ->
// alBufferData (PCM in) -> alGenSources -> alSourceQueueBuffers ->
// alSourcePlay -> poll AL_BUFFERS_PROCESSED -> refill. This bridge keeps
// the guest's PCM and drains it through the same trt_audio sink hooks
// DirectSound and XAudio2 use — one console output path.
#ifndef TIRAMISU_BRIDGE_OPENAL_H
#define TIRAMISU_BRIDGE_OPENAL_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register openal32.dll as BUILTIN.
int trt_bridge_register_openal(void);
u32 TRT_GUEST_ABI trt_openal_pump(u32 max_per_source);

#ifdef __cplusplus
}
#endif

#endif
