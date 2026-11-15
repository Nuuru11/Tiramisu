// tiramisu/src/trt/bridge_wasapi.h
// G4 — WASAPI COM audio stack (MMDevAPI + AudioClient -> XAudio2 host sink)
// and AVRT multimedia thread characteristics (avrt.dll).
//
// Commercial engines (Unity FMOD, Godot 4 WASAPI, Wwise, SDL2 WASAPI)
// acquire audio via:
//   CoCreateInstance(CLSID_MMDeviceEnumerator)
//     -> IMMDeviceEnumerator::GetDefaultAudioEndpoint(eRender, ...)
//     -> IMMDevice::Activate(IID_IAudioClient, ...)
//     -> IAudioClient::GetMixFormat / IsFormatSupported / Initialize
//     -> IAudioClient::GetService(IID_IAudioRenderClient, ...)
//     -> IAudioRenderClient::GetBuffer / ReleaseBuffer
//
// ReleaseBuffer converts IEEE 32-bit float (or 16/24/32-bit PCM) into 16-bit
// signed PCM and submits it directly to the host XAudio2 sink via
// trt_audio_get_hooks() (bridge_audio.h).
#ifndef TIRAMISU_BRIDGE_WASAPI_H
#define TIRAMISU_BRIDGE_WASAPI_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers CLSID_MMDeviceEnumerator in bridge_ole and registers avrt.dll.
int trt_bridge_register_wasapi(void);

// Direct factory entry for CoCreateInstance(CLSID_MMDeviceEnumerator).
int trt_wasapi_create_enumerator(const void* iid, void** out, void* user);

#ifdef __cplusplus
}
#endif

#endif
