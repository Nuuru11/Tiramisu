// tiramisu/third_party/mono-bridges/mono_fmod_door.cpp
// SysV front door for FMOD + FMOD Studio P/Invokes from managed code
// (Celeste under embedded Mono + FNA). One .so serves both the "fmod" and
// "fmodstudio" dllmaps.
//
// The low-level core API forwards to src/trt/bridge_fmod.cpp (the same
// implementation the 64-bit native bridge uses). The Studio API forwards
// to src/trt/bridge_fmod_studio.cpp (the real bank/event engine). The
// bridge functions are TRT_GUEST_ABI (ms_abi on Linux); these wrappers
// are plain SysV and the compiler handles the ABI transition at the call.
//
// Functions the bridges do NOT implement get a fail-visible function from
// gen/gen_fmod_failvisible.inc (loud log + FMOD_ERR_UNSUPPORTED), never a
// silent stub that pretends success.
//
// Marker exports (the harness asserts these exist to prove the REAL .so
// is loaded — the old stubs export neither):
//   trt_mono_bridge_is_real() -> 1
//   trt_mono_bridge_name() -> "fmod-real"
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "../../src/trt/bridge_fmod.h"
#include "../../src/trt/bridge_fmod_studio.h"
#include "../../src/trt/trt.h"

// FMOD_RESULT values used directly by the door (see bridge_fmod_studio.h).
#define DOOR_FMOD_OK 0
#define DOOR_FMOD_ERR_INVALID_PARAM 28

extern "C" {

// ---- markers: prove the real bridge is loaded ----
int trt_mono_bridge_is_real(void) { return 1; }
const char* trt_mono_bridge_name(void) { return "fmod-real"; }

// ---- PARAMETER_DESCRIPTION_INTERNAL (40 bytes: 36 bytes of fields +
// 4 bytes of tail padding to 8-byte alignment, matching the C#
// Sequential layout on 64-bit: name pointer, 8-byte parameter id,
// min/max/default, type, flags). The name pointer refers to bank-owned
// memory, valid while the bank is loaded.
struct param_desc_internal {
  const char* name;
  u32 id_data1;
  u32 id_data2;
  float minimum;
  float maximum;
  float default_value;
  int type;
  int flags;
};
static_assert(sizeof(param_desc_internal) == 40,
              "PARAMETER_DESCRIPTION_INTERNAL must be 40 bytes");
static_assert(offsetof(param_desc_internal, name) == 0, "");
static_assert(offsetof(param_desc_internal, id_data1) == 8, "");
static_assert(offsetof(param_desc_internal, id_data2) == 12, "");
static_assert(offsetof(param_desc_internal, minimum) == 16, "");
static_assert(offsetof(param_desc_internal, maximum) == 20, "");
static_assert(offsetof(param_desc_internal, default_value) == 24, "");
static_assert(offsetof(param_desc_internal, type) == 28, "");
static_assert(offsetof(param_desc_internal, flags) == 32, "");

void fill_param_desc_internal(param_desc_internal* out,
                              const trt_fmod_param_desc* in) {
  out->name = in->name;
  out->id_data1 = in->id_data1;
  out->id_data2 = in->id_data2;
  out->minimum = in->minimum;
  out->maximum = in->maximum;
  out->default_value = in->default_value;
  out->type = in->type;
  out->flags = in->flags;
}

// ---- ChannelGroup dual dispatch ----
// The FMOD C# wrapper binds the shared ChannelControl functions
// (Stop/SetVolume/GetVolume/SetPaused/GetPaused/IsPlaying) through the
// FMOD_ChannelGroup_* native names for BOTH Channel and ChannelGroup
// handles. The bridge validates handle types, so the door tries the
// channel function first; INVALID_HANDLE means "a group handle" (or
// garbage — the wrapper only passes handles it got from the bridge).
// Group volume/pause state lives in a door-side map: the bridge's group
// functions are stateless no-ops, and no channel is ever added to a
// group, so group stop/pause affects nothing downstream.
struct group_state {
  float volume = 1.0f;
  int paused = 0;
};
std::mutex g_group_mu;
std::unordered_map<void*, group_state> g_group_state;

group_state* group_state_for(void* h) {
  std::lock_guard<std::mutex> lk(g_group_mu);
  return &g_group_state[h];  // default-constructed on first use
}

// ---- core FMOD doors (bridge_fmod.cpp) ----

int FMOD_System_Create(void** out) {
  return fmod_System_Create(nullptr, out);
}
int FMOD_System_Release(void* sys) { return fmod_System_Release(sys); }
int FMOD_System_Init(void* sys, int maxchannels, unsigned int flags,
                     void* extra) {
  return fmod_System_Init(sys, (u32)maxchannels, flags, extra);
}
int FMOD_System_Close(void* sys) { return fmod_System_Close(sys); }
int FMOD_System_Update(void* sys) { return fmod_System_Update(sys); }
int FMOD_System_CreateSound(void* sys, const unsigned char* name,
                            unsigned int mode, void* exinfo, void** out) {
  return fmod_System_CreateSound(sys, name, mode, exinfo, out);
}
int FMOD_System_CreateStream(void* sys, const unsigned char* name,
                             unsigned int mode, void* exinfo, void** out) {
  return fmod_System_CreateStream(sys, name, mode, exinfo, out);
}
int FMOD_System_PlaySound(void* sys, void* sound, void* group, int paused,
                          void** out) {
  return fmod_System_PlaySound(sys, sound, group, paused, out);
}
int FMOD_System_GetMasterChannelGroup(void* sys, void** out) {
  return fmod_System_GetMasterChannelGroup(sys, out);
}
int FMOD_System_SetOutput(void* sys, unsigned int output) {
  return fmod_System_SetOutput(sys, output);
}
int FMOD_System_SetDSPBufferSize(void* sys, unsigned int len, int num) {
  return fmod_System_SetDSPBufferSize(sys, len, (u32)num);
}
// One host output device exists (the mixer); the format is the mixer's
// real format (48 kHz stereo), and the version is the bank format the
// Studio engine implements (FMOD 2.02.09).
int FMOD_System_GetNumDrivers(void* sys, int* numdrivers) {
  (void)sys;
  if (!numdrivers) return DOOR_FMOD_ERR_INVALID_PARAM;
  *numdrivers = 1;
  return DOOR_FMOD_OK;
}
int FMOD_System_GetSoftwareFormat(void* sys, int* samplerate, int* speakermode,
                                  int* numrawspeakers) {
  (void)sys;
  if (samplerate) *samplerate = 48000;
  if (speakermode) *speakermode = 3;  // FMOD_SPEAKERMODE_STEREO
  if (numrawspeakers) *numrawspeakers = 2;
  return DOOR_FMOD_OK;
}
int FMOD_System_GetVersion(void* sys, unsigned int* version) {
  (void)sys;
  if (!version) return DOOR_FMOD_ERR_INVALID_PARAM;
  *version = 0x00020209u;  // FMOD 2.02.09
  return DOOR_FMOD_OK;
}
int FMOD_Sound_Release(void* snd) { return fmod_Sound_Release(snd); }
int FMOD_Sound_SetMode(void* snd, unsigned int mode) {
  return fmod_Sound_SetMode(snd, mode);
}
int FMOD_Sound_GetMode(void* snd, unsigned int* mode) {
  return fmod_Sound_GetMode(snd, mode);
}
int FMOD_Sound_GetLength(void* snd, unsigned int* len, unsigned int unit) {
  return fmod_Sound_GetLength(snd, len, unit);
}
int FMOD_Channel_SetFrequency(void* ch, float freq) {
  return fmod_Channel_SetFrequency(ch, freq);
}
int FMOD_Channel_SetPosition(void* ch, unsigned int pos, unsigned int unit) {
  return fmod_Channel_SetPosition(ch, pos, unit);
}
int FMOD_ChannelGroup_Stop(void* h) {
  int rc = fmod_Channel_Stop(h);
  if (rc == DOOR_FMOD_OK) return rc;
  // Not a channel: a group. The bridge tracks no group membership, so
  // there is nothing to stop; accept.
  return DOOR_FMOD_OK;
}
int FMOD_ChannelGroup_SetVolume(void* h, float vol) {
  int rc = fmod_Channel_SetVolume(h, vol);
  if (rc == DOOR_FMOD_OK) return rc;
  group_state_for(h)->volume = vol;
  return DOOR_FMOD_OK;
}
int FMOD_ChannelGroup_GetVolume(void* h, float* vol) {
  if (!vol) return DOOR_FMOD_ERR_INVALID_PARAM;
  int rc = fmod_Channel_GetVolume(h, vol);
  if (rc == DOOR_FMOD_OK) return rc;
  *vol = group_state_for(h)->volume;
  return DOOR_FMOD_OK;
}
int FMOD_ChannelGroup_SetPaused(void* h, int paused) {
  int rc = fmod_Channel_SetPaused(h, paused);
  if (rc == DOOR_FMOD_OK) return rc;
  group_state_for(h)->paused = paused ? 1 : 0;
  return DOOR_FMOD_OK;
}
int FMOD_ChannelGroup_GetPaused(void* h, int* paused) {
  if (!paused) return DOOR_FMOD_ERR_INVALID_PARAM;
  int rc = fmod_Channel_GetPaused(h, paused);
  if (rc == DOOR_FMOD_OK) return rc;
  *paused = group_state_for(h)->paused;
  return DOOR_FMOD_OK;
}
int FMOD_ChannelGroup_IsPlaying(void* h, int* playing) {
  if (!playing) return DOOR_FMOD_ERR_INVALID_PARAM;
  int rc = fmod_Channel_IsPlaying(h, playing);
  if (rc == DOOR_FMOD_OK) return rc;
  // A group: no channel is ever added to a group in the bridge, so no
  // member channel is playing.
  *playing = 0;
  return DOOR_FMOD_OK;
}
int FMOD_Memory_Initialize(void* pool, int size, void* alloc, void* realloc,
                           void* free_cb, unsigned int memtype) {
  // The bridge uses the host heap and ignores the callbacks, but pass
  // them through in the documented struct anyway.
  struct mem_funcs {
    void* alloc;
    void* realloc;
    void* free_cb;
  };
  static mem_funcs funcs;
  funcs.alloc = alloc;
  funcs.realloc = realloc;
  funcs.free_cb = free_cb;
  return fmod_Memory_Initialize(pool, (u32)size, &funcs, memtype);
}
int FMOD_Debug_Initialize(unsigned int flags, int mode, void* callback,
                          const char* filename) {
  (void)callback;  // the bridge has no debug callback sink
  // NOTE: the bridge takes (flags, filename, mode) — reordered vs the
  // managed (flags, mode, callback, filename) signature.
  return fmod_Debug_Initialize(flags, filename, mode);
}

// ---- Studio doors: special cases (hand-written) ----

// FMOD_Studio_ParseID: "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" (braces
// optional) into a 16-byte GUID. System.Guid layout: u32 LE, u16 LE,
// u16 LE, 8 bytes as-is — the same as FMOD_GUID.
int FMOD_Studio_ParseID(const unsigned char* str, unsigned char* guid_out) {
  if (!str || !guid_out) return DOOR_FMOD_ERR_INVALID_PARAM;
  const char* s = (const char*)str;
  if (*s == '{') ++s;
  auto hexval = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  // 8 hex (u32 LE) - 4 hex (u16 LE) - 4 hex (u16 LE) - 4 hex - 12 hex.
  u32 d1 = 0;
  for (int i = 0; i < 8; ++i) {
    int v = hexval(s[i]);
    if (v < 0) return DOOR_FMOD_ERR_INVALID_PARAM;
    d1 = (d1 << 4) | (u32)v;
  }
  if (s[8] != '-') return DOOR_FMOD_ERR_INVALID_PARAM;
  u32 d2 = 0, d3 = 0;
  for (int i = 0; i < 4; ++i) {
    int v = hexval(s[9 + i]);
    if (v < 0) return DOOR_FMOD_ERR_INVALID_PARAM;
    d2 = (d2 << 4) | (u32)v;
  }
  if (s[13] != '-') return DOOR_FMOD_ERR_INVALID_PARAM;
  for (int i = 0; i < 4; ++i) {
    int v = hexval(s[14 + i]);
    if (v < 0) return DOOR_FMOD_ERR_INVALID_PARAM;
    d3 = (d3 << 4) | (u32)v;
  }
  if (s[18] != '-') return DOOR_FMOD_ERR_INVALID_PARAM;
  u8 d4[8];
  for (int i = 0; i < 4; ++i) {
    int v = hexval(s[19 + i]);
    if (v < 0) return DOOR_FMOD_ERR_INVALID_PARAM;
    if ((i & 1) == 0) d4[i / 2] = (u8)(v << 4);
    else d4[i / 2] |= (u8)v;
  }
  if (s[23] != '-') return DOOR_FMOD_ERR_INVALID_PARAM;
  for (int i = 0; i < 12; ++i) {
    int v = hexval(s[24 + i]);
    if (v < 0) return DOOR_FMOD_ERR_INVALID_PARAM;
    if ((i & 1) == 0) d4[2 + i / 2] = (u8)(v << 4);
    else d4[2 + i / 2] |= (u8)v;
  }
  memcpy(guid_out, &d1, 4);
  u16 d2v = (u16)d2, d3v = (u16)d3;
  memcpy(guid_out + 4, &d2v, 2);
  memcpy(guid_out + 6, &d3v, 2);
  memcpy(guid_out + 8, d4, 8);
  return DOOR_FMOD_OK;
}

// Batch parameter set: loop over the by-index setter.
int FMOD_Studio_EventInstance_SetParameterValuesByIndices(void* inst,
                                                          const int* indices,
                                                          const float* values,
                                                          int count) {
  if (!indices || !values || count < 0)
    return DOOR_FMOD_ERR_INVALID_PARAM;
  for (int i = 0; i < count; ++i) {
    int rc = trt_fmod_studio_instance_set_parameter_value_by_index(
        inst, indices[i], values[i]);
    if (rc != DOOR_FMOD_OK) return rc;
  }
  return DOOR_FMOD_OK;
}

// ---- Studio doors: mechanical wrappers (generated) ----
#include "gen/gen_studio_doors.inc"

// ---- fail-visible: everything the bridges do not implement ----
#include "gen/gen_fmod_failvisible.inc"

}  // extern "C"
