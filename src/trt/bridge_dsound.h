// tiramisu/src/trt/bridge_dsound.h
// DirectSound for the guest (M6 coverage batch).
//
// Nearly every pre-2015 PC game plays sound through DirectSound: lock a
// secondary buffer, write PCM, unlock, play. UWP has no dsound.dll, so
// this bridge implements the COM surface (SDK-exact vtables) and feeds
// the decoded PCM into the same console audio sink XAudio2 uses
// (trt_audio_set_hooks from bridge_audio.h) — at Unlock time, which is
// the DirectSound analog of XAudio2's SubmitSourceBuffer.
#ifndef TIRAMISU_BRIDGE_DSOUND_H
#define TIRAMISU_BRIDGE_DSOUND_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

int trt_bridge_register_dsound(void);

#ifdef __cplusplus
}
#endif

#endif
