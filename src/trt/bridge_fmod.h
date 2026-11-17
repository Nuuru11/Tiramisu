// tiramisu/src/trt/bridge_fmod.h
// FMOD audio bridge — fmodex/fmod/fmodstudio. WAV/PCM real through the host
// mixer; compressed containers named-silent until decoders land.
#ifndef TIRAMISU_BRIDGE_FMOD_H
#define TIRAMISU_BRIDGE_FMOD_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register fmodex/fmod/fmodstudio module names as BUILTIN.
int trt_bridge_register_fmod(void);

// the core C surface (guest ABI == host ABI here: all handles are opaque
// pointers, all scalars pass in registers identically)
int TRT_GUEST_ABI fmod_System_Create(const void* mem, void** out);
int TRT_GUEST_ABI fmod_System_Release(void* sys);
int TRT_GUEST_ABI fmod_System_Init(void* sys, u32 maxchannels, u32 flags,
                                   void* extra);
int TRT_GUEST_ABI fmod_System_Close(void* sys);
int TRT_GUEST_ABI fmod_System_Update(void* sys);
int TRT_GUEST_ABI fmod_System_CreateSound(void* sys, const void* name_or_data,
                                          u32 mode, void* exinfo, void** out);
int TRT_GUEST_ABI fmod_System_CreateStream(void* sys, const void* name_or_data,
                                           u32 mode, void* exinfo, void** out);
int TRT_GUEST_ABI fmod_System_PlaySound(void* sys, void* channel_or_index,
                                        void* sound, int paused, void** out);
int TRT_GUEST_ABI fmod_System_SetOutput(void* sys, u32 output);
int TRT_GUEST_ABI fmod_System_SetSpeakerMode(void* sys, u32 mode);
int TRT_GUEST_ABI fmod_System_SetDSPBufferSize(void* sys, u32 len, u32 num);
int TRT_GUEST_ABI fmod_Sound_Release(void* snd);
int TRT_GUEST_ABI fmod_Sound_SetMode(void* snd, u32 mode);
int TRT_GUEST_ABI fmod_Sound_GetMode(void* snd, u32* mode);
int TRT_GUEST_ABI fmod_Sound_GetLength(void* snd, u32* len, u32 unit);
int TRT_GUEST_ABI fmod_Channel_Stop(void* ch);
int TRT_GUEST_ABI fmod_Channel_SetVolume(void* ch, float vol);
int TRT_GUEST_ABI fmod_Channel_GetVolume(void* ch, float* vol);
int TRT_GUEST_ABI fmod_Channel_SetPaused(void* ch, int paused);
int TRT_GUEST_ABI fmod_Channel_GetPaused(void* ch, int* paused);
int TRT_GUEST_ABI fmod_Channel_IsPlaying(void* ch, int* playing);

#ifdef __cplusplus
}
#endif

#endif  // TIRAMISU_BRIDGE_FMOD_H
