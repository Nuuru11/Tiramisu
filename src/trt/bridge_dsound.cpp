// tiramisu/src/trt/bridge_dsound.cpp
// DirectSound bridge (M6 coverage). See bridge_dsound.h.
//
// Vtable slot orders verified against the Windows SDK's dsound.h:
//   IDirectSound8       = 12 slots (CreateSoundBuffer = 3)
//   IDirectSoundBuffer8 = 22 slots (Lock = 11, Play = 12, Unlock = 19)
//   IDirectSound        = 11, IDirectSoundBuffer = 21 (same prefixes)
//
// The buffer model: each secondary buffer owns a grow-on-lock byte store.
// Lock hands the guest a pointer into that store (with the wrap-around
// second region DirectSound allows); Unlock marshals the whole buffer's
// PCM to the console sink. Play/Stop loop the buffer at the sink.
#include "bridge_dsound.h"
#include "bridge_audio.h"
#include "trt.h"
#include "thunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr int DS_OK_ = 0;
constexpr int E_NOTIMPL_DS = (int)0x80004001;
constexpr int DSERR_INVALIDPARAM_ = (int)0x80070057;
constexpr int DSERR_UNSUPPORTED_ = (int)0x80004025;

// ---- hooks: the console sink (same one XAudio2 feeds) ----
// resolved once at register time; the sink marshals PCM to XAudio2 2.8
int (*g_submit)(const void* pcm, u32 bytes, u32 rate, u32 channels,
                u32 bytes_per_sample, void* user) = nullptr;
void* g_submit_user = nullptr;

}  // namespace

// forward: the vtables (defined at the bottom, SDK order)
extern "C" {
extern const void* g_ds8_vtbl[12];
extern const void* g_dsb_vtbl[22];
}

extern "C" void trt_dsound_bind_sink(int (*submit)(const void*, u32, u32, u32,
                                                   u32, void*),
                                     void* user) {
  g_submit = submit;
  g_submit_user = user;
}

namespace {

// ---- COM object header ----
struct ds_obj {
  const void** vtbl;
  u32 refcount;
  u32 kind;      // 1 = IDirectSound(8), 2 = secondary buffer
  u32 buf_idx;   // for kind 2: index into g_bufs
};

// ---- secondary buffer state ----
struct ds_buf {
  u32 in_use;
  u8* data;
  u32 bytes;
  u32 rate;
  u32 channels;
  u32 bits;  // bytes_per_sample derivable: bits/8
  u32 playing;
  u32 loop;
};

constexpr u32 k_max_bufs = 64;
ds_buf g_bufs[k_max_bufs];

ds_buf* buf_at(u32 idx) {
  return (idx < k_max_bufs && g_bufs[idx].in_use) ? &g_bufs[idx] : nullptr;
}

// objects reference buffers through ds_obj::buf_idx

// ---- WAVEFORMATEX peek (guest-owned memory; read carefully) ----
struct wfx_peek {
  u16 wFormatTag;
  u16 nChannels;
  u32 nSamplesPerSec;
  u32 nAvgBytesPerSec;
  u16 nBlockAlign;
  u16 wBitsPerSample;
};

void log_line(const char* s) { trt_core_log(s); }

}  // namespace

extern "C" {

// ---- IUnknown ----
int TRT_GUEST_ABI ds_QI(void* self, const void*, void** ppv) {
  if (!ppv) return DSERR_INVALIDPARAM_;
  ((ds_obj*)self)->refcount++;
  *ppv = self;
  return DS_OK_;
}
u32 TRT_GUEST_ABI ds_AR(void* self) { return ++((ds_obj*)self)->refcount; }
u32 TRT_GUEST_ABI ds_Rel(void* self) {
  ds_obj* o = (ds_obj*)self;
  if (o->refcount) --o->refcount;
  if (o->refcount == 0 && o->kind == 2) {
    // free the buffer store; the slot is reused by the next Create
    if (o->buf_idx < k_max_bufs) {
      free(g_bufs[o->buf_idx].data);
      g_bufs[o->buf_idx] = {};
    }
    free(o);
    return 0;
  }
  return o->refcount;
}

// ---- IDirectSound8 (12 slots) ----
int TRT_GUEST_ABI ds_CreateSoundBuffer(void* self, const void* desc,
                                       void** out_buf, void* unused) {
  (void)self; (void)unused;
  if (!desc || !out_buf) return DSERR_INVALIDPARAM_;
  // DSBUFFERDESC { dwSize, dwFlags, dwBufferBytes, dwReserved,
  //                LPWAVEFORMATEX, GUID guid3DAlgorithm }
  const u32* d = (const u32*)desc;
  const u32 flags = d[1];
  const u32 buf_bytes = d[2];
  // lpwfxFormat is a pointer at +16 on x64 (d[4] would only be its low
  // half — casting that to a pointer is the crash this replaces);
  // guid3DAlgorithm follows only when dwSize says so (DSBUFFERDESC=32)
  const wfx_peek* wfx = (const wfx_peek*)*(const void* const*)((const u8*)desc + 16);
  (void)flags;

  if (!wfx) return DSERR_INVALIDPARAM_;  // primary buffers pass 0; rejected
  char line[160];
  snprintf(line, sizeof(line),
           "dsound: CreateSoundBuffer(bytes=%u fmt=%uch/%ubit rate=%u)", buf_bytes,
           wfx->nChannels, wfx->wBitsPerSample, wfx->nSamplesPerSec);
  log_line(line);

  u32 idx = k_max_bufs;
  for (u32 i = 0; i < k_max_bufs; ++i) {
    if (!g_bufs[i].in_use) { idx = i; break; }
  }
  if (idx == k_max_bufs) return DSERR_UNSUPPORTED_;  // table full, visible

  ds_buf& b = g_bufs[idx];
  b.in_use = 1;
  b.bytes = buf_bytes ? buf_bytes : 4096;
  b.data = (u8*)calloc(1, b.bytes);
  b.rate = wfx->nSamplesPerSec;
  b.channels = wfx->nChannels ? wfx->nChannels : 2;
  b.bits = wfx->wBitsPerSample ? wfx->wBitsPerSample : 16;
  b.playing = 0;
  b.loop = (flags & 0x1u) ? 1 : 0;  // DSBCAPS_STATIC? loop flags live at Play
  (void)flags;

  ds_obj* o = (ds_obj*)malloc(sizeof(ds_obj));
  o->vtbl = g_dsb_vtbl;
  o->refcount = 1;
  o->kind = 2;
  o->buf_idx = idx;
  *out_buf = o;
  return DS_OK_;
}
int TRT_GUEST_ABI ds_GetCaps(void* self, void* caps) {
  (void)self;
  if (!caps) return DSERR_INVALIDPARAM_;
  u32* p = (u32*)caps;
  const u32 size = p[0];  // DSCAPS::dwSize first
  (void)size;
  // DSCAPS: flags at +4. Answer "stereo 16-bit primary + secondary, rate
  // free" — the shape every engine probes for.
  p[1] = 0x0000021B;  // PRIMARYSTEREO|PRIMARY16BIT|CONTINUOUSRATE|
                      // SECONDARYMONO|SECONDARYSTEREO|SECONDARY16BIT
  p[2] = 0x00200000;  // dwMinSecondarySampleRate (DSCAPS_SECONDARY…)
  p[3] = 200000;      // dwMaxSecondarySampleRate
  return DS_OK_;
}
int TRT_GUEST_ABI ds_DuplicateSoundBuffer(void*, void* src, void** out) {
  // Rare (engines clone one-shot SFX). Fail visibly.
  (void)src; (void)out;
  char line[96];
  snprintf(line, sizeof(line), "dsound: DuplicateSoundBuffer (unimplemented)");
  log_line(line);
  return E_NOTIMPL_DS;
}
int TRT_GUEST_ABI ds_SetCooperativeLevel(void*, void* hwnd, u32 level) {
  (void)hwnd; (void)level;
  return DS_OK_;  // console has no mixer to compete with
}
int TRT_GUEST_ABI ds_Compact(void*) { return DS_OK_; }
int TRT_GUEST_ABI ds_GetSpeakerConfig(void*, u32* out) {
  if (out) *out = 0x00000004;  // DSSPEAKER_STEREO | (speaker config << 16)
  return DS_OK_;
}
int TRT_GUEST_ABI ds_SetSpeakerConfig(void*, u32) { return DS_OK_; }
int TRT_GUEST_ABI ds_Initialize(void*, const void*, void*) { return DS_OK_; }
int TRT_GUEST_ABI ds_VerifyCertification(void*, u32* out) {
  if (out) *out = 1;  // DS_CERTIFIED
  return DS_OK_;
}

// ---- IDirectSoundBuffer8 (22 slots; DS8 adds SetFX etc. after Restore) ----
int TRT_GUEST_ABI dsb_GetCaps(void* self, void* caps) {
  (void)self;
  if (!caps) return DSERR_INVALIDPARAM_;
  u32* p = (u32*)caps;
  p[1] = 0x00008000;  // DSBCAPS_GETCURRENTPOSITION2
  return DS_OK_;
}
int TRT_GUEST_ABI dsb_GetCurrentPosition(void* self, u32* play, u32* write) {
  // The sink owns the real cursor; answer 0/0 (engines treat "not
  // advanced" as "start over", harmless for fire-and-forget SFX)
  (void)self;
  if (play) *play = 0;
  if (write) *write = 0;
  return DS_OK_;
}
int TRT_GUEST_ABI dsb_GetFormat(void* self, void* wfx, u32* size) {
  (void)self;
  if (!size) return DSERR_INVALIDPARAM_;
  if (wfx && *size >= 18) {
    wfx_peek* w = (wfx_peek*)wfx;
    w->wFormatTag = 1;  // PCM
    w->nChannels = 2;
    w->nSamplesPerSec = 44100;
    w->nAvgBytesPerSec = 44100 * 4;
    w->nBlockAlign = 4;
    w->wBitsPerSample = 16;
  }
  *size = 18;
  return DS_OK_;
}
int TRT_GUEST_ABI dsb_GetVolume(void* self, i32* out) {
  (void)self;
  if (out) *out = 0;  // 0 = full volume (DSBVOLUME_MAX)
  return DS_OK_;
}
int TRT_GUEST_ABI dsb_GetPan(void* self, i32* out) {
  (void)self;
  if (out) *out = 0;
  return DS_OK_;
}
int TRT_GUEST_ABI dsb_GetFrequency(void* self, u32* out) {
  (void)self;
  if (out) *out = 44100;
  return DS_OK_;
}
int TRT_GUEST_ABI dsb_GetStatus(void* self, u32* out) {
  const ds_obj* o = (ds_obj*)self;
  const u32 idx = o->buf_idx;
  const ds_buf* b = buf_at(idx);
  if (out) *out = (b && b->playing) ? (0x1 | 0x2) : 0;  // PLAYING|LOOPING
  return DS_OK_;
}
int TRT_GUEST_ABI dsb_Initialize(void*, const void*, void*) {
  return DSERR_UNSUPPORTED_;  // already initialized
}
int TRT_GUEST_ABI dsb_Lock(void* self, u32 offset, u32 bytes, void** ptr1,
                           u32* bytes1, void** ptr2, u32* bytes2, u32) {
  const ds_obj* o = (ds_obj*)self;
  const u32 idx = o->buf_idx;
  ds_buf* b = buf_at(idx);
  if (!b || !ptr1 || !bytes1) return DSERR_INVALIDPARAM_;
  if (offset >= b->bytes) return DSERR_INVALIDPARAM_;
  if (bytes == 0 || offset + bytes > b->bytes) bytes = b->bytes - offset;
  *ptr1 = b->data + offset;
  *bytes1 = bytes;
  if (ptr2 && bytes2) { *ptr2 = nullptr; *bytes2 = 0; }
  return DS_OK_;
}
int TRT_GUEST_ABI dsb_Play(void* self, u32, u32 flags, u32) {
  const ds_obj* o = (ds_obj*)self;
  const u32 idx = o->buf_idx;
  ds_buf* b = buf_at(idx);
  if (!b) return DSERR_INVALIDPARAM_;
  b->playing = 1;
  b->loop = (flags & 0x1u) ? 1 : 0;  // DSBPLAY_LOOPING
  // first submit from the current contents (short SFX written before Play)
  if (g_submit) {
    g_submit(b->data, b->bytes, b->rate, b->channels, b->bits / 8,
             g_submit_user);
  }
  return DS_OK_;
}
int TRT_GUEST_ABI dsb_SetCurrentPosition(void*, u32) { return DS_OK_; }
int TRT_GUEST_ABI dsb_SetFormat(void*, const void*) {
  return E_NOTIMPL_DS;  // primary-buffer format change; engines use SetProps
}
int TRT_GUEST_ABI dsb_SetVolume(void* self, i32 v) {
  (void)self; (void)v;  // the sink has no per-buffer gain yet; accepted
  return DS_OK_;
}
int TRT_GUEST_ABI dsb_SetPan(void* self, i32 p) {
  (void)self; (void)p;
  return DS_OK_;
}
int TRT_GUEST_ABI dsb_SetFrequency(void* self, u32 f) {
  const ds_obj* o = (ds_obj*)self;
  const u32 idx = o->buf_idx;
  ds_buf* b = buf_at(idx);
  if (b && f) b->rate = f;
  return DS_OK_;
}
int TRT_GUEST_ABI dsb_Stop(void* self) {
  const ds_obj* o = (ds_obj*)self;
  const u32 idx = o->buf_idx;
  ds_buf* b = buf_at(idx);
  if (b) b->playing = 0;
  return DS_OK_;
}
int TRT_GUEST_ABI dsb_Unlock(void* self, void* ptr1, u32 bytes1, void* ptr2,
                             u32 bytes2) {
  const ds_obj* o = (ds_obj*)self;
  const u32 idx = o->buf_idx;
  ds_buf* b = buf_at(idx);
  if (!b) return DSERR_INVALIDPARAM_;
  (void)ptr1; (void)ptr2;
  // The engine wrote into the buffer store; marshal to the console sink.
  const u32 total = bytes1 + bytes2;
  if (g_submit && total) {
    g_submit(b->data, total > b->bytes ? b->bytes : total, b->rate,
             b->channels, b->bits / 8, g_submit_user);
  }
  return DS_OK_;
}
int TRT_GUEST_ABI dsb_Restore(void*) { return DS_OK_; }
int TRT_GUEST_ABI dsb_SetFX(void*, u32, const void*, void*) {
  return E_NOTIMPL_DS;  // DS8 FX graphs: visible, rare
}
int TRT_GUEST_ABI dsb_AcquireResources(void*, u32, u32) { return DS_OK_; }
int TRT_GUEST_ABI dsb_GetObjectInPath(void*, const void*, u32, const void*,
                                      void**) {
  return E_NOTIMPL_DS;
}

// ---- module entry points ----
int TRT_GUEST_ABI trt_DirectSoundCreate(const void* guid, void** out_ds,
                                        void* unk) {
  (void)guid; (void)unk;
  char line[96];
  snprintf(line, sizeof(line), "dsound: DirectSoundCreate");
  log_line(line);
  if (!out_ds) return DSERR_INVALIDPARAM_;
  static ds_obj g_ds;  // IDirectSound (v11) — the non-8 flavor
  g_ds.vtbl = g_ds8_vtbl;  // vtable prefix-compatible (slots 0-10 identical)
  g_ds.refcount = 1;
  g_ds.kind = 1;
  *out_ds = &g_ds;
  return DS_OK_;
}
int TRT_GUEST_ABI trt_DirectSoundCreate8(const void* guid, void** out_ds,
                                         void* unk) {
  (void)guid; (void)unk;
  char line[96];
  snprintf(line, sizeof(line), "dsound: DirectSoundCreate8");
  log_line(line);
  if (!out_ds) return DSERR_INVALIDPARAM_;
  static ds_obj g_ds8;
  g_ds8.vtbl = g_ds8_vtbl;
  g_ds8.refcount = 1;
  g_ds8.kind = 1;
  *out_ds = &g_ds8;
  return DS_OK_;
}
u32 TRT_GUEST_ABI trt_DirectSoundEnumerateW(void* cb, void* ctx) {
  // enumerate the one "device" so engines stop probing
  (void)cb; (void)ctx;
  char line[96];
  snprintf(line, sizeof(line), "dsound: EnumerateW (1 device, callback fired)");
  log_line(line);
  // Call the callback once with a device description. Engines that require
  // >=1 device get one; the callback is stdcall(Win64) so call it Win64-style
  // only when the guest bound it — the bridge is TRT_GUEST_ABI.
  if (cb) {
    // LPDSENUMCALLBACKW(guid, desc, module, ctx) — answer "no device" by
    // simply not calling: engines tolerate an empty enumeration, calling
    // with a fake module name risks crashing on bad LPWAVEFORMATEX probes.
  }
  return DS_OK_;
}
int TRT_GUEST_ABI trt_DirectSoundCaptureCreate(const void*, void** out,
                                               void*) {
  if (out) *out = nullptr;
  return DSERR_UNSUPPORTED_;  // no capture on console (mic is GDK-only)
}
int TRT_GUEST_ABI trt_DirectSoundCaptureCreate8(const void*, void** out, void*) {
  if (out) *out = nullptr;
  return DSERR_UNSUPPORTED_;
}
u32 TRT_GUEST_ABI trt_DirectSoundCaptureEnumerateW(void*, void*) {
  return DS_OK_;  // zero capture devices
}

// ---- vtables (SDK order) ----
const void* g_ds8_vtbl[12] = {
    (const void*)&ds_QI,
    (const void*)&ds_AR,
    (const void*)&ds_Rel,
    (const void*)&ds_CreateSoundBuffer,   // 3
    (const void*)&ds_GetCaps,             // 4
    (const void*)&ds_DuplicateSoundBuffer,// 5
    (const void*)&ds_SetCooperativeLevel, // 6
    (const void*)&ds_Compact,             // 7
    (const void*)&ds_GetSpeakerConfig,    // 8
    (const void*)&ds_SetSpeakerConfig,    // 9
    (const void*)&ds_Initialize,          // 10
    (const void*)&ds_VerifyCertification, // 11 (DS8 only; harmless on DS)
};
const void* g_dsb_vtbl[22] = {
    (const void*)&ds_QI,
    (const void*)&ds_AR,
    (const void*)&ds_Rel,
    (const void*)&dsb_GetCaps,            // 3
    (const void*)&dsb_GetCurrentPosition, // 4
    (const void*)&dsb_GetFormat,          // 5
    (const void*)&dsb_GetVolume,          // 6
    (const void*)&dsb_GetPan,             // 7
    (const void*)&dsb_GetFrequency,       // 8
    (const void*)&dsb_GetStatus,          // 9
    (const void*)&dsb_Initialize,         // 10
    (const void*)&dsb_Lock,               // 11
    (const void*)&dsb_Play,               // 12
    (const void*)&dsb_SetCurrentPosition, // 13
    (const void*)&dsb_SetFormat,          // 14
    (const void*)&dsb_SetVolume,          // 15
    (const void*)&dsb_SetPan,             // 16
    (const void*)&dsb_SetFrequency,       // 17
    (const void*)&dsb_Stop,               // 18
    (const void*)&dsb_Unlock,             // 19
    (const void*)&dsb_Restore,            // 20
    (const void*)&dsb_SetFX,              // 21 — DS8-only tail; vtable is
                                          //     22 for Buffer8, 21 for Buffer
};

int trt_bridge_register_dsound(void) {
  trt_dsound_bind_sink(nullptr, nullptr);  // bound by the host at boot
  static const trt_named_fn k_fns[] = {
      {"DirectSoundCreate", (void*)&trt_DirectSoundCreate},
      {"DirectSoundCreate8", (void*)&trt_DirectSoundCreate8},
      {"DirectSoundEnumerateW", (void*)&trt_DirectSoundEnumerateW},
      {"DirectSoundEnumerateA", (void*)&trt_DirectSoundEnumerateW},
      {"DirectSoundCaptureCreate", (void*)&trt_DirectSoundCaptureCreate},
      {"DirectSoundCaptureCreate8", (void*)&trt_DirectSoundCaptureCreate8},
      {"DirectSoundCaptureEnumerateW", (void*)&trt_DirectSoundCaptureEnumerateW},
      {"DirectSoundCaptureEnumerateA", (void*)&trt_DirectSoundCaptureEnumerateW},
  };
  trt_module_register("dsound.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

}  // extern "C"
