// tiramisu/src/trt/bridge_audio.cpp
// XAudio2 for the guest.
//
// Games output sound through XAudio2: they create the engine, a mastering
// voice, source voices per sound, submit buffers and poll GetState. Without
// this bridge a game either crashes at startup or runs muted.
//
// The COM objects use the SDK's EXACT vtable layouts:
//   IXAudio2             3 + 8  slots
//   IXAudio2SourceVoice  3 + 30 slots (22..32 are the source-only ones)
// On the console the host publishes a real audio sink through
// trt_audio_set_hooks; the offline harness counts bytes.
#include "bridge_audio.h"
#include "trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr int E_NOTIMPL_ = 0x80004001;
constexpr int S_OK_ = 0;

// ---- the host audio sink (the console fills this in) ----
struct audio_hooks {
  // a PCM buffer was submitted (pcm bytes, sample rate, channels, bytes/sample)
  int (*submit)(const void* pcm, u32 bytes, u32 rate, u32 channels,
                u32 bytes_per_sample, void* user);
  void* user;
};
audio_hooks g_hooks = {nullptr, nullptr};

} // namespace

extern "C" void trt_audio_set_hooks(int (*submit)(const void*, u32, u32, u32,
                                                  u32, void*),
                                    void* user) {
  g_hooks.submit = submit;
  g_hooks.user = user;
}

// getter for sibling bridges (openal drives the same sink)
extern "C" void trt_audio_get_hooks(void (**submit)(const void*, u32, u32, u32,
                                                    u32, void*),
                                    void** user) {
  if (submit) *submit = (void (*)(const void*, u32, u32, u32, u32, void*))g_hooks.submit;
  if (user) *user = g_hooks.user;
}

namespace {

// ---- objects ----
enum voice_kind { VK_MASTER = 1, VK_SOURCE = 2, VK_SUBMIX = 3 };

struct xaudio_voice {
  const void** vtbl;
  u32 refcount;
  u32 kind;              // voice_kind
  u32 started;
  u32 stopped;
  u32 buffers_queued;
  u64 samples_played;
  // the source format (WAVEFORMATEX)
  u16 format_tag;
  u16 channels;
  u32 sample_rate;
  u16 bits_per_sample;
  u32 bytes_total;
};

struct xaudio_engine {
  const void** vtbl;
  u32 refcount;
  u32 started;
};

// the voices (fixed pool: a game opens dozens, 64 is plenty for now)
xaudio_voice g_voices[64];
int g_voice_count = 0;
xaudio_engine g_engine_obj;

// forward decls for the vtables
extern const void* g_xaudio2_vtbl[11];
extern const void* g_srcvoice_vtbl[33];

// ---- IUnknown on every object ----
u32 TRT_GUEST_ABI xq_AddRef(void* self) {
  u32* r = (u32*)self;
  return ++r[1];
}
u32 TRT_GUEST_ABI xq_Release(void* self) {
  u32* r = (u32*)self;
  return r[1] ? --r[1] : 0;
}
int TRT_GUEST_ABI xq_QueryInterface(void* self, const u8* riid, void** out) {
  (void)riid;
  if (out) *out = self;
  return S_OK_;
}

// ---- IXAudio2 (slots 3..10) ----
int TRT_GUEST_ABI xe_RegisterForCallbacks(void*, void*) { return S_OK_; }
void TRT_GUEST_ABI xe_UnregisterForCallbacks(void*, void*) {}
int TRT_GUEST_ABI xe_CreateSourceVoice(void* self, const void* format,
                                       void** out_voice, u32 flags,
                                       float max_ratio, void* callback,
                                       const void* sends, u32 opset);
int TRT_GUEST_ABI xe_CreateSubmixVoice(void* self, void** out_voice,
                                       u32 channels, u32 rate, u32 flags,
                                       u32 processor, const void* sends,
                                       u32 opset) {
  (void)self; (void)processor; (void)sends; (void)opset;
  if (!out_voice || g_voice_count >= 64) return E_NOTIMPL_;
  xaudio_voice* v = &g_voices[g_voice_count++];
  memset(v, 0, sizeof(*v));
  v->vtbl = g_srcvoice_vtbl;
  v->refcount = 1;
  v->kind = VK_SUBMIX;
  v->channels = (u16)channels;
  v->sample_rate = rate;
  *out_voice = v;
  return S_OK_;
}
int TRT_GUEST_ABI xe_CreateMasteringVoice(void* self, void** out_voice,
                                          u32 channels, u32 rate, u32 flags,
                                          const u16* device_id,
                                          const void* effects,
                                          u8 category, u8 pipeline) {
  (void)self; (void)flags; (void)device_id; (void)effects;
  (void)category; (void)pipeline;
  if (!out_voice || g_voice_count >= 64) return E_NOTIMPL_;
  xaudio_voice* v = &g_voices[g_voice_count++];
  memset(v, 0, sizeof(*v));
  v->vtbl = g_srcvoice_vtbl;
  v->refcount = 1;
  v->kind = VK_MASTER;
  v->channels = (u16)(channels ? channels : 2);
  v->sample_rate = rate ? rate : 48000;
  *out_voice = v;
  return S_OK_;
}
int TRT_GUEST_ABI xe_StartEngine(void* self) {
  ((xaudio_engine*)self)->started = 1;
  return S_OK_;
}
void TRT_GUEST_ABI xe_StopEngine(void* self) {
  ((xaudio_engine*)self)->started = 0;
}
int TRT_GUEST_ABI xe_CommitChanges(void*, u32) { return S_OK_; }

const void* g_xaudio2_vtbl[11] = {
    (const void*)&xq_QueryInterface,
    (const void*)&xq_AddRef,
    (const void*)&xq_Release,
    (const void*)&xe_RegisterForCallbacks,
    (const void*)&xe_UnregisterForCallbacks,
    (const void*)&xe_CreateSourceVoice,
    (const void*)&xe_CreateSubmixVoice,
    (const void*)&xe_CreateMasteringVoice,
    (const void*)&xe_StartEngine,
    (const void*)&xe_StopEngine,
    (const void*)&xe_CommitChanges,
};

// ---- IXAudio2Voice base (slots 3..21 shared by every voice) ----
int TRT_GUEST_ABI xv_GetVoiceDetails(void* self, void* details) {
  (void)self;
  if (!details) return E_NOTIMPL_;
  u32* d = (u32*)details;
  d[0] = 20;  // SizeOf
  d[1] = 1;   // InputChannels
  d[2] = 48000;
  d[3] = 64;   // CreationFlags
  d[4] = 0;    // Flags
  return S_OK_;
}
int TRT_GUEST_ABI xv_SetOutputVoices(void*, const void*) { return S_OK_; }
int TRT_GUEST_ABI xv_SetEffectChain(void*, const void*) { return S_OK_; }
int TRT_GUEST_ABI xv_EnableEffect(void*, u32, u32, u32) { return S_OK_; }
int TRT_GUEST_ABI xv_DisableEffect(void*, u32, u32, u32) { return S_OK_; }
void TRT_GUEST_ABI xv_GetEffectState(void*, u32, u32* enabled) {
  if (enabled) *enabled = 0;
}
int TRT_GUEST_ABI xv_SetEffectParameters(void*, u32, const void*, u32, u32) {
  return S_OK_;
}
int TRT_GUEST_ABI xv_GetEffectParameters(void*, u32, void*, u32) {
  return E_NOTIMPL_;
}
int TRT_GUEST_ABI xv_SetFilterParameters(void*, const void*, u32) {
  return S_OK_;
}
void TRT_GUEST_ABI xv_GetFilterParameters(void*, void*) {}
int TRT_GUEST_ABI xv_SetOutputFilterParameters(void*, void*, const void*, u32) {
  return S_OK_;
}
void TRT_GUEST_ABI xv_GetOutputFilterParameters(void*, void*, void*) {}
int TRT_GUEST_ABI xv_SetVolume(void* self, float volume, u32 opset) {
  (void)self; (void)opset; (void)volume;
  return S_OK_;
}
void TRT_GUEST_ABI xv_GetVolume(void* self, float* out) {
  (void)self;
  if (out) *out = 1.0f;
}
int TRT_GUEST_ABI xv_SetChannelVolumes(void*, u32, const float*, u32) {
  return S_OK_;
}
void TRT_GUEST_ABI xv_GetChannelVolumes(void*, u32, float* out) {
  if (out) out[0] = 1.0f;
}
int TRT_GUEST_ABI xv_SetOutputMatrix(void*, void*, u32, u32, const float*, u32) {
  return S_OK_;
}
void TRT_GUEST_ABI xv_GetOutputMatrix(void*, void*, u32, u32, float* out) {
  if (out) out[0] = 1.0f;
}
void TRT_GUEST_ABI xv_DestroyVoice(void*) {}

// ---- IXAudio2SourceVoice (slots 22..32) ----
int TRT_GUEST_ABI xv_Start(void* self, u32 flags, u32 opset) {
  (void)flags; (void)opset;
  ((xaudio_voice*)self)->started = 1;
  ((xaudio_voice*)self)->stopped = 0;
  return S_OK_;
}
int TRT_GUEST_ABI xv_Stop(void* self, u32 flags, u32 opset) {
  (void)flags; (void)opset;
  ((xaudio_voice*)self)->stopped = 1;
  return S_OK_;
}
// XAUDIO2_BUFFER: Flags, AudioBytes, pAudioData, PlayBegin, PlayLength,
//                LoopBegin, LoopLength, LoopCount, pContext
struct x2_buffer {
  u32 flags;
  u32 audio_bytes;
  const void* audio_data;
  u32 play_begin;
  u32 play_length;
  u32 loop_begin;
  u32 loop_length;
  u32 loop_count;
  void* context;
};
int TRT_GUEST_ABI xv_SubmitSourceBuffer(void* self, const x2_buffer* buf,
                                        const void* wma) {
  (void)wma;
  xaudio_voice* v = (xaudio_voice*)self;
  if (!buf) return E_NOTIMPL_;
  ++v->buffers_queued;
  v->bytes_total += buf->audio_bytes;
  // hand the PCM to the host's sink (the console plays it)
  if (g_hooks.submit && buf->audio_data) {
    g_hooks.submit(buf->audio_data, buf->audio_bytes, v->sample_rate,
                   v->channels, v->bits_per_sample / 8, g_hooks.user);
  }
  return S_OK_;
}
int TRT_GUEST_ABI xv_FlushSourceBuffers(void* self) {
  ((xaudio_voice*)self)->buffers_queued = 0;
  return S_OK_;
}
int TRT_GUEST_ABI xv_Discontinuity(void*) { return S_OK_; }
int TRT_GUEST_ABI xv_ExitLoop(void*, u32) { return S_OK_; }
// XAUDIO2_VOICE_STATE: pCurrentBufferContext, BuffersQueued, SamplesPlayed
void TRT_GUEST_ABI xv_GetState(void* self, void* state, u32 flags) {
  (void)flags;
  xaudio_voice* v = (xaudio_voice*)self;
  if (!state) return;
  void** s = (void**)state;
  s[0] = nullptr;                    // pCurrentBufferContext
  u32* q = (u32*)state;
  q[2] = v->buffers_queued;          // BuffersQueued (offset 8)
  u64* sp = (u64*)state;
  sp[2] = v->samples_played;         // SamplesPlayed (offset 16)
}
int TRT_GUEST_ABI xv_SetFrequencyRatio(void*, float, u32) { return S_OK_; }
void TRT_GUEST_ABI xv_GetFrequencyRatio(void*, float* out) {
  if (out) *out = 1.0f;
}
int TRT_GUEST_ABI xv_SetSourceSampleRate(void* self, u32 rate) {
  ((xaudio_voice*)self)->sample_rate = rate;
  return S_OK_;
}
int TRT_GUEST_ABI xv_GetChannelMask(void* self, u32* mask) {
  (void)self;
  if (mask) *mask = 0x3;  // FRONT_LEFT | FRONT_RIGHT
  return S_OK_;
}

const void* g_srcvoice_vtbl[33] = {
    (const void*)&xq_QueryInterface,
    (const void*)&xq_AddRef,
    (const void*)&xq_Release,
    (const void*)&xv_GetVoiceDetails,
    (const void*)&xv_SetOutputVoices,
    (const void*)&xv_SetEffectChain,
    (const void*)&xv_EnableEffect,
    (const void*)&xv_DisableEffect,
    (const void*)&xv_GetEffectState,
    (const void*)&xv_SetEffectParameters,
    (const void*)&xv_GetEffectParameters,
    (const void*)&xv_SetFilterParameters,
    (const void*)&xv_GetFilterParameters,
    (const void*)&xv_SetOutputFilterParameters,
    (const void*)&xv_GetOutputFilterParameters,
    (const void*)&xv_SetVolume,
    (const void*)&xv_GetVolume,
    (const void*)&xv_SetChannelVolumes,
    (const void*)&xv_GetChannelVolumes,
    (const void*)&xv_SetOutputMatrix,
    (const void*)&xv_GetOutputMatrix,
    (const void*)&xv_DestroyVoice,
    (const void*)&xv_Start,
    (const void*)&xv_Stop,
    (const void*)&xv_SubmitSourceBuffer,
    (const void*)&xv_FlushSourceBuffers,
    (const void*)&xv_Discontinuity,
    (const void*)&xv_ExitLoop,
    (const void*)&xv_GetState,
    (const void*)&xv_SetFrequencyRatio,
    (const void*)&xv_GetFrequencyRatio,
    (const void*)&xv_SetSourceSampleRate,
    (const void*)&xv_GetChannelMask,
};

int TRT_GUEST_ABI xe_CreateSourceVoice(void* self, const void* format,
                                       void** out_voice, u32 flags,
                                       float max_ratio, void* callback,
                                       const void* sends, u32 opset) {
  (void)self; (void)flags; (void)max_ratio; (void)callback; (void)sends;
  (void)opset;
  if (!out_voice || g_voice_count >= 64) return E_NOTIMPL_;
  xaudio_voice* v = &g_voices[g_voice_count++];
  memset(v, 0, sizeof(*v));
  v->vtbl = g_srcvoice_vtbl;
  v->refcount = 1;
  v->kind = VK_SOURCE;
  if (format) {
    // WAVEFORMATEX: tag, channels, rate, avg, align, bits, cbSize
    const u16* w = (const u16*)format;
    v->format_tag = w[0];
    v->channels = w[1];
    v->sample_rate = *(const u32*)(const void*)(w + 2);
    v->bits_per_sample = w[5];
  }
  *out_voice = v;
  return S_OK_;
}

} // namespace

extern "C" {

// XAudio2Create(out_engine, flags, processor)
int TRT_GUEST_ABI trt_XAudio2Create(void** out_engine, u32 flags, u32 processor) {
  (void)flags; (void)processor;
  if (!out_engine) return E_NOTIMPL_;
  memset(&g_engine_obj, 0, sizeof(g_engine_obj));
  g_engine_obj.vtbl = g_xaudio2_vtbl;
  g_engine_obj.refcount = 1;
  g_voice_count = 0;
  *out_engine = &g_engine_obj;
  return 0;
}

// ---- test hooks: the guest proves its PCM reached the sink ----
u32 TRT_GUEST_ABI trt_audio_note_submit(u32 bytes) {
  if (g_hooks.submit) {
    g_hooks.submit(nullptr, bytes, 48000, 2, 2, g_hooks.user);
  }
  return bytes;
}
void TRT_GUEST_ABI trt_audio_note_start(void) {}

int trt_bridge_register_audio(void) {
  static const trt_named_fn k_fns[] = {
      {"XAudio2Create", (void*)&trt_XAudio2Create},
      {"trt_audio_note_submit", (void*)&trt_audio_note_submit},
      {"trt_audio_note_start", (void*)&trt_audio_note_start},
      // some engines link against the 2.8 CLSID path too
      {"XAudio2CreateWithVersionInfo", (void*)&trt_XAudio2Create},
  };
  trt_module_register("xaudio2_9.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  trt_module_register("xaudio2_8.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  trt_module_register("xaudio2.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

} // extern "C"
