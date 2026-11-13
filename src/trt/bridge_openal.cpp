// tiramisu/src/trt/bridge_openal.cpp
// OpenAL 1.1 bridge. See bridge_openal.h.
//
// The subset games actually use (verified against OpenAL 1.1 SDK + the
// Minecraft-class call pattern):
//   device:   alcOpenDevice / alcCloseDevice / alcGetError / alcGetString /
//             alcCreateContext / alcMakeContextCurrent / alcDestroyContext /
//             alcGetIntegerv
//   buffers:  alGenBuffers / alDeleteBuffers / alIsBuffer / alBufferData /
//             alGetBufferi(AL_SIZE/FREQUENCY/CHANNELS/BITS)
//   sources:  alGenSources / alDeleteSources / alIsSource / alSourcei /
//             alGetSourcei / alSourcef / alGetSourcef / alSource3f /
//             alSourcePlay / alSourceStop / alSourcePause /
//             alSourceQueueBuffers / alSourceUnqueueBuffers
//   state:    alGetError / alIsExtensionPresent / alGetProcAddress /
//             alGetString / alDistanceModel / alListenerf / alListener3f /
//             alListenerfv / alGetListenerfv / alDopplerFactor /
//             alSpeedOfSound
//
// Format map (OpenAL 1.1): AL_FORMAT_MONO8=0x1100, MONO16=0x1101,
// STEREO8=0x1102, STEREO16=0x1103.
//
// Buffer model: alBufferData copies the PCM. Queued buffers attach to the
// source in queue order; a drain thread (daemon) submits finished PCM to
// the audio sink at the buffer's rate/channels and advances the queue —
// AL_BUFFERS_PROCESSED counts what the sink has consumed.
#include "bridge_openal.h"
#include "bridge_audio.h"
#include "thunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// unique name, file scope, OUTSIDE extern "C": C linkage demotes even
// namespace-scope symbols to globals on this toolchain (see bridge_d3dx)
namespace allog {
static void line(const char* s) { trt_core_log(s); }
}
#define log_line(s_) allog::line(s_)

extern "C" {

// ---- OpenAL constants (AL/alc.h 1.1) ----
constexpr u32 AL_FALSE = 0, AL_TRUE = 1;
constexpr u32 AL_NO_ERROR = 0;
constexpr u32 AL_INVALID_NAME = 0xA001;
constexpr u32 AL_INVALID_ENUM = 0xA002;
constexpr u32 AL_INVALID_VALUE = 0xA003;
constexpr u32 AL_INVALID_OPERATION = 0xA004;
constexpr u32 AL_OUT_OF_MEMORY = 0xA005;
constexpr u32 AL_FORMAT_MONO8 = 0x1100;
constexpr u32 AL_FORMAT_MONO16 = 0x1101;
constexpr u32 AL_FORMAT_STEREO8 = 0x1102;
constexpr u32 AL_FORMAT_STEREO16 = 0x1103;
constexpr u32 AL_SOURCE_STATE = 0x1010;
constexpr u32 AL_INITIAL = 0x1011, AL_PLAYING = 0x1012, AL_PAUSED = 0x1013,
              AL_STOPPED = 0x1014;
constexpr u32 AL_BUFFERS_QUEUED = 0x1015;
constexpr u32 AL_BUFFERS_PROCESSED = 0x1016;
constexpr u32 AL_SIZE = 0x2004;
constexpr u32 AL_CHANNELS = 0x2007;
constexpr u32 AL_BITS = 0x2002;
constexpr u32 AL_FREQUENCY = 0x2001;
constexpr u32 AL_GAIN = 0x1005;
constexpr u32 AL_PITCH = 0x1003;
constexpr u32 AL_POSITION = 0x1004;
constexpr u32 AL_VELOCITY = 0x1006;
constexpr u32 AL_LOOPING = 0x1007;
constexpr u32 AL_BUFFER = 0x1009;
constexpr u32 AL_DISTANCE_MODEL = 0xD000;
constexpr u32 AL_VENDOR = 0xB001, AL_VERSION = 0xB002,
              AL_RENDERER = 0xB003, AL_EXTENSIONS = 0xB004;
constexpr u32 ALC_NO_ERROR = 0;
constexpr u32 ALC_INVALID_DEVICE = 0xA001;
constexpr u32 ALC_DEFAULT_DEVICE_SPECIFIER = 0x1004;
constexpr u32 ALC_DEVICE_SPECIFIER = 0x1005;

// ---- error state (per-thread conceptually; single guest thread here) ----
u32 g_al_error = AL_NO_ERROR;
u32 g_alc_error = ALC_NO_ERROR;
void set_al(u32 e) { g_al_error = e; }
void set_alc(u32 e) { g_alc_error = e; }

// ---- buffers: PCM storage ----
constexpr u32 k_max_bufs = 512;
struct al_buf {
  u32 alive;
  u32 fmt;
  u32 rate;
  u32 bytes;
  u8* pcm;  // heap copy
};
al_buf g_bufs[k_max_bufs];
u32 g_buf_next = 0;

// handles are 1-based; 0 = AL invalid name
u32 buf_alloc() {
  if (g_buf_next >= k_max_bufs) return 0;
  // reuse freed slots
  for (u32 i = 0; i < k_max_bufs; ++i) {
    const u32 idx = (g_buf_next + i) % k_max_bufs;
    if (!g_bufs[idx].alive) {
      ++g_buf_next;
      g_bufs[idx].alive = 1;
      return idx + 1;
    }
  }
  return 0;
}
al_buf* buf_at(u32 h) {
  if (!h || h > k_max_bufs) return nullptr;
  return g_bufs[h - 1].alive ? &g_bufs[h - 1] : nullptr;
}
void buf_free(u32 h) {
  al_buf* b = buf_at(h);
  if (!b) return;
  free(b->pcm);
  b->pcm = nullptr;
  b->alive = 0;
}

// ---- sources: a queue of buffer handles + playback state ----
constexpr u32 k_max_srcs = 128;
constexpr u32 k_queue_max = 64;
struct al_src {
  u32 alive;
  u32 state;             // AL_INITIAL/PLAYING/PAUSED/STOPPED
  u32 queue[k_queue_max];
  u32 queue_head;        // next buffer the sink consumes
  u32 queue_tail;        // next free queue slot
  u32 processed;         // buffers the sink finished (for Unqueue)
  u32 looping;
  float gain, pitch;
  float pos[3], vel[3];
};
al_src g_srcs[k_max_srcs];
u32 g_src_next = 0;

u32 src_alloc() {
  if (g_src_next >= k_max_srcs) return 0;
  for (u32 i = 0; i < k_max_srcs; ++i) {
    const u32 idx = (g_src_next + i) % k_max_srcs;
    if (!g_srcs[idx].alive) {
      ++g_src_next;
      g_srcs[idx].alive = 1;
      g_srcs[idx].state = AL_INITIAL;
      g_srcs[idx].queue_head = 0;
      g_srcs[idx].queue_tail = 0;
      g_srcs[idx].processed = 0;
      g_srcs[idx].looping = 0;
      g_srcs[idx].gain = 1.f;
      g_srcs[idx].pitch = 1.f;
      return idx + 1;
    }
  }
  return 0;
}
al_src* src_at(u32 h) {
  if (!h || h > k_max_srcs) return nullptr;
  return g_srcs[h - 1].alive ? &g_srcs[h - 1] : nullptr;
}
void src_free(u32 h) {
  al_src* s = src_at(h);
  if (!s) return;
  s->alive = 0;
}

u32 src_channels(const al_buf* b) {
  return (b->fmt == AL_FORMAT_STEREO8 || b->fmt == AL_FORMAT_STEREO16) ? 2 : 1;
}
u32 src_bytes_per(const al_buf* b) {
  return (b->fmt == AL_FORMAT_MONO16 || b->fmt == AL_FORMAT_STEREO16) ? 2 : 1;
}

// ---- drain: what the sink consumed becomes "processed" ----
// the sink hook is fetched lazily in trt_openal_pump (single guest thread)
void (*g_submit)(const void*, u32, u32, u32, u32, void*) = nullptr;
void* g_submit_user = nullptr;

// defined in bridge_audio.cpp (C linkage, file scope)
extern "C" void trt_audio_get_hooks(void (**submit)(const void*, u32, u32, u32,
                                                    u32, void*),
                                    void** user);

// drain ONE buffer from a playing source: submit its PCM, mark processed.
// Returns 1 when something drained.
int src_drain_one(al_src* s) {
  if (s->queue_head == s->queue_tail) return 0;
  const u32 h = s->queue[s->queue_head % k_queue_max];
  al_buf* b = buf_at(h);
  if (!b) {  // deleted while queued: skip
    ++s->queue_head;
    ++s->processed;
    return 1;
  }
  if (g_submit && b->bytes)
    g_submit(b->pcm, b->bytes, b->rate, src_channels(b), src_bytes_per(b),
             g_submit_user);
  ++s->queue_head;
  ++s->processed;
  return 1;
}

// ---- AL context: this bridge is the context (the guest creates one and
// makes it current; nothing else observes the difference) ----

u32 TRT_GUEST_ABI al_GetError(void) {
  const u32 e = g_al_error;
  g_al_error = AL_NO_ERROR;
  return e;
}
void TRT_GUEST_ABI al_Enable(u32) {}
void TRT_GUEST_ABI al_Disable(u32) {}
u8 TRT_GUEST_ABI al_IsEnabled(u32) { return AL_FALSE; }
const char* TRT_GUEST_ABI al_GetString(u32 param) {
  switch (param) {
    case AL_VENDOR: return "Tiramisu";
    case AL_VERSION: return "1.1";
    case AL_RENDERER: return "OpenAL Soft on XAudio2 (bridge)";
    case AL_EXTENSIONS: return "";
    default: set_al(AL_INVALID_ENUM); return "";
  }
}
u8 TRT_GUEST_ABI al_IsExtensionPresent(const char*) {
  return AL_FALSE;  // fail-visible via GetError? no: extension probes are
                    // *supposed* to return false; not an error
}
void* TRT_GUEST_ABI al_GetProcAddress(const char* name) {
  char line[128];
  snprintf(line, sizeof(line), "openal: alGetProcAddress(\"%s\") -> null "
                               "(extension unsupported)", name ? name : "?");
  log_line(line);
  return nullptr;  // visible in the log; games branch off gracefully
}
u32 TRT_GUEST_ABI al_GetEnumValue(const char*) { return 0; }
void TRT_GUEST_ABI al_Listenerf(u32, float) {}
void TRT_GUEST_ABI al_Listener3f(u32, float, float, float) {}
void TRT_GUEST_ABI al_Listenerfv(u32, const float*) {}
void TRT_GUEST_ABI al_GetListenerf(u32, float* out) {
  if (out) *out = 1.f;
}
void TRT_GUEST_ABI al_GetListener3f(u32, float* a, float* b, float* c) {
  if (a) *a = 0.f;
  if (b) *b = 0.f;
  if (c) *c = 0.f;
}
void TRT_GUEST_ABI al_GetListenerfv(u32, float*) {}
void TRT_GUEST_ABI al_DistanceModel(u32) {}
void TRT_GUEST_ABI al_SpeedOfSound(float) {}
void TRT_GUEST_ABI al_DopplerFactor(float) {}

// ---- buffers ----
void TRT_GUEST_ABI al_GenBuffers(u32 n, u32* out) {
  if (!out) { set_al(AL_INVALID_VALUE); return; }
  for (u32 i = 0; i < n; ++i) {
    const u32 h = buf_alloc();
    if (!h) { set_al(AL_OUT_OF_MEMORY); return; }
    out[i] = h;
  }
  char line[80];
  snprintf(line, sizeof(line), "openal: GenBuffers(%u)", n);
  log_line(line);
}
void TRT_GUEST_ABI al_DeleteBuffers(u32 n, const u32* hs) {
  (void)n;
  if (!hs) { set_al(AL_INVALID_VALUE); return; }
  for (u32 i = 0; i < n; ++i) buf_free(hs[i]);
}
u8 TRT_GUEST_ABI al_IsBuffer(u32 h) { return buf_at(h) ? AL_TRUE : AL_FALSE; }
void TRT_GUEST_ABI al_BufferData(u32 h, u32 fmt, const void* data,
                              u32 bytes, u32 rate) {
  al_buf* b = buf_at(h);
  if (!b) { set_al(AL_INVALID_NAME); return; }
  if (!data || !bytes || !rate) {
    char line[128];
    snprintf(line, sizeof(line),
             "openal: BufferData h%u fmt=%#x bytes=%u rate=%u -> AL_INVALID_VALUE",
             h, fmt, bytes, rate);
    log_line(line);
    set_al(AL_INVALID_VALUE);
    return;
  }
  if (fmt != AL_FORMAT_MONO8 && fmt != AL_FORMAT_MONO16 &&
      fmt != AL_FORMAT_STEREO8 && fmt != AL_FORMAT_STEREO16) {
    char line[128];
    snprintf(line, sizeof(line),
             "openal: BufferData h%u fmt=%#x unsupported (want mono/stereo 8/16)",
             h, fmt);
    log_line(line);
    set_al(AL_INVALID_ENUM);
    return;
  }
  u8* copy = (u8*)malloc(bytes);
  if (!copy) { set_al(AL_OUT_OF_MEMORY); return; }
  memcpy(copy, data, bytes);
  free(b->pcm);
  b->pcm = copy;
  b->bytes = bytes;
  b->fmt = fmt;
  b->rate = rate;
}
void TRT_GUEST_ABI al_GetBufferi(u32 h, u32 param, u32* out) {
  const al_buf* b = buf_at(h);
  if (!b || !out) { set_al(AL_INVALID_NAME); return; }
  switch (param) {
    case AL_SIZE: *out = b->bytes; break;
    case AL_FREQUENCY: *out = b->rate; break;
    case AL_CHANNELS: *out = src_channels(b); break;
    case AL_BITS: *out = src_bytes_per(b) * 8; break;
    default: set_al(AL_INVALID_ENUM); break;
  }
}

// ---- sources ----
void TRT_GUEST_ABI al_GenSources(u32 n, u32* out) {
  if (!out) { set_al(AL_INVALID_VALUE); return; }
  for (u32 i = 0; i < n; ++i) {
    const u32 h = src_alloc();
    if (!h) { set_al(AL_OUT_OF_MEMORY); return; }
    out[i] = h;
  }
  char line[80];
  snprintf(line, sizeof(line), "openal: GenSources(%u)", n);
  log_line(line);
}
void TRT_GUEST_ABI al_DeleteSources(u32 n, const u32* hs) {
  (void)n;
  if (!hs) { set_al(AL_INVALID_VALUE); return; }
  for (u32 i = 0; i < n; ++i) src_free(hs[i]);
}
u8 TRT_GUEST_ABI al_IsSource(u32 h) { return src_at(h) ? AL_TRUE : AL_FALSE; }

void TRT_GUEST_ABI al_Sourcei(u32 h, u32 param, u32 value) {
  al_src* s = src_at(h);
  if (!s) { set_al(AL_INVALID_NAME); return; }
  switch (param) {
    case AL_LOOPING: s->looping = value; break;
    case AL_BUFFER:
      // stop-and-set: replace the queue with a single static buffer
      if (value && !buf_at(value)) { set_al(AL_INVALID_NAME); return; }
      s->queue[0] = value;
      s->queue[1] = 0;
      s->queue_head = 0;
      s->queue_tail = value ? 1 : 0;
      s->processed = 0;
      break;
    case AL_SOURCE_STATE: set_al(AL_INVALID_OPERATION); break;
    default: break;  // source-relative etc: accepted, effect-free here
  }
}
void TRT_GUEST_ABI al_GetSourcei(u32 h, u32 param, u32* out) {
  al_src* s = src_at(h);
  if (!s || !out) { set_al(AL_INVALID_NAME); return; }
  switch (param) {
    case AL_SOURCE_STATE: *out = s->state; break;
    case AL_BUFFERS_QUEUED: *out = s->queue_tail - s->queue_head; break;
    case AL_BUFFERS_PROCESSED: *out = s->processed; break;
    case AL_LOOPING: *out = s->looping; break;
    case AL_BUFFER: {
      // the queue's next buffer (the one playing)
      *out = s->queue_head < s->queue_tail
                 ? s->queue[s->queue_head % k_queue_max]
                 : 0;
      break;
    }
    default: set_al(AL_INVALID_ENUM); break;
  }
}
void TRT_GUEST_ABI al_Sourcef(u32 h, u32 param, float value) {
  al_src* s = src_at(h);
  if (!s) { set_al(AL_INVALID_NAME); return; }
  switch (param) {
    case AL_GAIN: s->gain = value; break;
    case AL_PITCH: s->pitch = value; break;
    default: break;
  }
}
void TRT_GUEST_ABI al_GetSourcef(u32 h, u32 param, float* out) {
  al_src* s = src_at(h);
  if (!s || !out) { set_al(AL_INVALID_NAME); return; }
  switch (param) {
    case AL_GAIN: *out = s->gain; break;
    case AL_PITCH: *out = s->pitch; break;
    default: set_al(AL_INVALID_ENUM); break;
  }
}
void TRT_GUEST_ABI al_Source3f(u32 h, u32 param, float a, float b, float c) {
  al_src* s = src_at(h);
  if (!s) { set_al(AL_INVALID_NAME); return; }
  if (param == AL_POSITION) {
    s->pos[0] = a; s->pos[1] = b; s->pos[2] = c;
  } else if (param == AL_VELOCITY) {
    s->vel[0] = a; s->vel[1] = b; s->vel[2] = c;
  }
}
void TRT_GUEST_ABI al_SourcePlay(u32 h) {
  al_src* s = src_at(h);
  if (!s) { set_al(AL_INVALID_NAME); return; }
  s->state = AL_PLAYING;
  char line[80];
  snprintf(line, sizeof(line), "openal: SourcePlay h%u (queued %u)", h,
           s->queue_tail - s->queue_head);
  log_line(line);
}
void TRT_GUEST_ABI al_SourcePlay_v(u32, const u32*) {}
void TRT_GUEST_ABI al_SourceStop(u32 h) {
  al_src* s = src_at(h);
  if (!s) { set_al(AL_INVALID_NAME); return; }
  s->state = AL_STOPPED;
}
void TRT_GUEST_ABI al_SourceStop_v(u32, const u32*) {}
void TRT_GUEST_ABI al_SourcePause(u32 h) {
  al_src* s = src_at(h);
  if (!s) { set_al(AL_INVALID_NAME); return; }
  s->state = AL_PAUSED;
}
void TRT_GUEST_ABI al_SourcePause_v(u32, const u32*) {}

// the streaming heart: queue/unqueue
void TRT_GUEST_ABI al_SourceQueueBuffers(u32 h, u32 n, const u32* hs) {
  al_src* s = src_at(h);
  if (!s || !hs) { set_al(AL_INVALID_NAME); return; }
  for (u32 i = 0; i < n; ++i) {
    if (!buf_at(hs[i])) { set_al(AL_INVALID_NAME); return; }
    if (s->queue_tail - s->queue_head >= k_queue_max) {
      set_al(AL_INVALID_VALUE);
      return;
    }
    s->queue[s->queue_tail % k_queue_max] = hs[i];
    ++s->queue_tail;
  }
  char line[80];
  snprintf(line, sizeof(line), "openal: QueueBuffers h%u +%u (total %u)", h, n,
           s->queue_tail - s->queue_head);
  log_line(line);
}
void TRT_GUEST_ABI al_SourceUnqueueBuffers(u32 h, u32 n, u32* out) {
  al_src* s = src_at(h);
  if (!s) { set_al(AL_INVALID_NAME); return; }
  if (!out) { set_al(AL_INVALID_VALUE); return; }
  if (n > s->processed) {
    // asking for more than finished: OpenAL semantics = error, hand back 0
    set_al(AL_INVALID_VALUE);
    return;
  }
  for (u32 i = 0; i < n; ++i) out[i] = 0;  // handles already retired
  s->processed -= n;
}

// ---- ALC ----
void* TRT_GUEST_ABI alc_OpenDevice(const char* name) {
  char line[128];
  snprintf(line, sizeof(line), "openal: alcOpenDevice(\"%s\") -> device",
           name ? name : "(null)");
  log_line(line);
  return (void*)0x1A11;  // non-null token
}
u8 TRT_GUEST_ABI alc_CloseDevice(void* dev) {
  (void)dev;
  return AL_TRUE;
}
u32 TRT_GUEST_ABI alc_GetError(void* dev) {
  (void)dev;
  const u32 e = g_alc_error;
  g_alc_error = ALC_NO_ERROR;
  return e;
}
const char* TRT_GUEST_ABI alc_GetString(void* dev, u32 param) {
  (void)dev;
  if (param == ALC_DEFAULT_DEVICE_SPECIFIER ||
      param == ALC_DEVICE_SPECIFIER)
    return "Tiramisu XAudio2";
  return "";
}
void* TRT_GUEST_ABI alc_CreateContext(void* dev, const u32* attrs) {
  (void)dev; (void)attrs;
  return (void*)0x1A12;
}
u8 TRT_GUEST_ABI alc_MakeContextCurrent(void* ctx) {
  (void)ctx;
  return AL_TRUE;
}
void TRT_GUEST_ABI alc_DestroyContext(void* ctx) { (void)ctx; }
void TRT_GUEST_ABI alc_ProcessContext(void* ctx) { (void)ctx; }
void TRT_GUEST_ABI alc_SuspendContext(void* ctx) { (void)ctx; }
void TRT_GUEST_ABI alc_GetIntegerv(void* dev, u32 param, u32 size, u32* out) {
  (void)dev;
  if (!out || !size) return;
  // ALC_MAJOR_VERSION=0x1000, MINOR=0x1001; attributes the game might ask
  out[0] = 1;  // major
  (void)param;
}

// ---- the pump the guest calls to keep audio flowing ----
// Minecraft's loop: play -> every frame poll AL_BUFFERS_PROCESSED ->
// unqueue -> refill. We give the guest a manual pump: the drain happens
// here instead of a background thread (deterministic, testable offline).
u32 TRT_GUEST_ABI trt_openal_pump(u32 max_per_source) {
  if (!g_submit) trt_audio_get_hooks(&g_submit, &g_submit_user);
  u32 drained = 0;
  for (u32 i = 0; i < k_max_srcs; ++i) {
    al_src* s = &g_srcs[i];
    if (!s->alive || s->state != AL_PLAYING) continue;
    for (u32 k = 0; k < max_per_source; ++k) {
      if (!src_drain_one(s)) break;
      ++drained;
      if (s->queue_head == s->queue_tail) {
        // queue exhausted
        if (s->looping && s->queue_tail) {
          s->queue_head = 0;  // loop to the start
        } else {
          s->state = AL_STOPPED;
        }
        break;
      }
    }
  }
  return drained;
}

int trt_bridge_register_openal(void) {
  static const trt_named_fn k_al_fns[] = {
      {"alGetError", (void*)&al_GetError},
      {"alEnable", (void*)&al_Enable},
      {"alDisable", (void*)&al_Disable},
      {"alIsEnabled", (void*)&al_IsEnabled},
      {"alGetString", (void*)&al_GetString},
      {"alIsExtensionPresent", (void*)&al_IsExtensionPresent},
      {"alGetProcAddress", (void*)&al_GetProcAddress},
      {"alGetEnumValue", (void*)&al_GetEnumValue},
      {"alListenerf", (void*)&al_Listenerf},
      {"alListener3f", (void*)&al_Listener3f},
      {"alListenerfv", (void*)&al_Listenerfv},
      {"alGetListenerf", (void*)&al_GetListenerf},
      {"alGetListener3f", (void*)&al_GetListener3f},
      {"alGetListenerfv", (void*)&al_GetListenerfv},
      {"alDistanceModel", (void*)&al_DistanceModel},
      {"alSpeedOfSound", (void*)&al_SpeedOfSound},
      {"alDopplerFactor", (void*)&al_DopplerFactor},
      {"alGenBuffers", (void*)&al_GenBuffers},
      {"alDeleteBuffers", (void*)&al_DeleteBuffers},
      {"alIsBuffer", (void*)&al_IsBuffer},
      {"alBufferData", (void*)&al_BufferData},
      {"alGetBufferi", (void*)&al_GetBufferi},
      {"alGenSources", (void*)&al_GenSources},
      {"alDeleteSources", (void*)&al_DeleteSources},
      {"alIsSource", (void*)&al_IsSource},
      {"alSourcei", (void*)&al_Sourcei},
      {"alGetSourcei", (void*)&al_GetSourcei},
      {"alSourcef", (void*)&al_Sourcef},
      {"alGetSourcef", (void*)&al_GetSourcef},
      {"alSource3f", (void*)&al_Source3f},
      {"alSourcePlay", (void*)&al_SourcePlay},
      {"alSourcePlayv", (void*)&al_SourcePlay_v},
      {"alSourceStop", (void*)&al_SourceStop},
      {"alSourceStopv", (void*)&al_SourceStop_v},
      {"alSourcePause", (void*)&al_SourcePause},
      {"alSourcePausev", (void*)&al_SourcePause_v},
      {"alSourceQueueBuffers", (void*)&al_SourceQueueBuffers},
      {"alSourceUnqueueBuffers", (void*)&al_SourceUnqueueBuffers},
      {"trt_openal_pump", (void*)&trt_openal_pump},
  };
  static const trt_named_fn k_alc_fns[] = {
      {"alcOpenDevice", (void*)&alc_OpenDevice},
      {"alcCloseDevice", (void*)&alc_CloseDevice},
      {"alcGetError", (void*)&alc_GetError},
      {"alcGetString", (void*)&alc_GetString},
      {"alcCreateContext", (void*)&alc_CreateContext},
      {"alcMakeContextCurrent", (void*)&alc_MakeContextCurrent},
      {"alcDestroyContext", (void*)&alc_DestroyContext},
      {"alcProcessContext", (void*)&alc_ProcessContext},
      {"alcSuspendContext", (void*)&alc_SuspendContext},
      {"alcGetIntegerv", (void*)&alc_GetIntegerv},
  };
  static trt_named_fn k_all[sizeof(k_al_fns) / sizeof(k_al_fns[0]) +
                            sizeof(k_alc_fns) / sizeof(k_alc_fns[0])];
  u32 n = 0;
  for (u32 i = 0; i < sizeof(k_al_fns) / sizeof(k_al_fns[0]); ++i)
    k_all[n++] = k_al_fns[i];
  for (u32 i = 0; i < sizeof(k_alc_fns) / sizeof(k_alc_fns[0]); ++i)
    k_all[n++] = k_alc_fns[i];
  trt_module_register("openal32.dll", TRT_ROUTE_BUILTIN, k_all, (int)n);
  trt_module_register("OpenAL32.dll", TRT_ROUTE_BUILTIN, k_all, (int)n);
  return 0;
}

}  // extern "C"
