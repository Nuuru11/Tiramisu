// tiramisu/src/trt/bridge_fmod_studio.h
// FMOD Studio engine: real bank/event playback for the Mono door.
//
// Parses real FMOD Studio bank files (RIFF/FEV, format v0x67 as shipped by
// Celeste) and FSB5 sample banks, decodes FSB Vorbis to PCM through the
// vendored stb_vorbis, and plays it through trt_audio_submit_voice (the
// host mixer). No stubs: every function either does the real thing or
// returns a failure FMOD_RESULT with a named log line.
//
// Handles are opaque pointers to engine objects; the door layer
// (third_party/mono-bridges) owns the P/Invoke signatures and translates
// them to these calls. This file is plain C++ (SysV); the guest ABI is
// the door's problem, not the engine's.
#ifndef TIRAMISU_BRIDGE_FMOD_STUDIO_H
#define TIRAMISU_BRIDGE_FMOD_STUDIO_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- FMOD_RESULT (FMOD Studio 2.02 public enum, fmod_common.h) ----
// These MUST match the SDK: the managed wrapper compares against them
// (notably ERR_EVENT_NOTFOUND). The core bridge's old invented values
// (INVALID_HANDLE=5 etc.) were wrong and are fixed to match.
#define TRT_FMOD_OK 0
#define TRT_FMOD_ERR_BADCOMMAND 1
#define TRT_FMOD_ERR_CHANNEL_ALLOC 2
#define TRT_FMOD_ERR_CHANNEL_STOLEN 3
#define TRT_FMOD_ERR_DMA 4
#define TRT_FMOD_ERR_DSP_CONNECTION 5
#define TRT_FMOD_ERR_DSP_FORMAT 6
#define TRT_FMOD_ERR_DSP_NOTFOUND 7
#define TRT_FMOD_ERR_DSP_RUNNING 8
#define TRT_FMOD_ERR_DSP_TOOMANY 9
#define TRT_FMOD_ERR_FILE_BAD 10
#define TRT_FMOD_ERR_FILE_COULDNOTSEEK 11
#define TRT_FMOD_ERR_FILE_DISKEJECTED 12
#define TRT_FMOD_ERR_FILE_EOF 13
#define TRT_FMOD_ERR_FILE_ENDOFDATA 14
#define TRT_FMOD_ERR_FILE_NOTFOUND 15
#define TRT_FMOD_ERR_FORMAT 16
#define TRT_FMOD_ERR_HEADER_MISMATCH 17
#define TRT_FMOD_ERR_HTTP 18
#define TRT_FMOD_ERR_HTTP_ACCESS 19
#define TRT_FMOD_ERR_HTTP_PROXY_AUTH 20
#define TRT_FMOD_ERR_HTTP_SERVER_ERROR 21
#define TRT_FMOD_ERR_HTTP_TIMEOUT 22
#define TRT_FMOD_ERR_INITIALIZATION 23
#define TRT_FMOD_ERR_INITIALIZED 24
#define TRT_FMOD_ERR_INTERNAL 25
#define TRT_FMOD_ERR_INVALID_FLOAT 26
#define TRT_FMOD_ERR_INVALID_HANDLE 27
#define TRT_FMOD_ERR_INVALID_PARAM 28
#define TRT_FMOD_ERR_INVALID_POSITION 29
#define TRT_FMOD_ERR_INVALID_SPEAKER 30
#define TRT_FMOD_ERR_INVALID_SYNCPOINT 31
#define TRT_FMOD_ERR_INVALID_THREAD 32
#define TRT_FMOD_ERR_INVALID_VECTOR 33
#define TRT_FMOD_ERR_MAXAUDIBLE 34
#define TRT_FMOD_ERR_MEMORY 35
#define TRT_FMOD_ERR_MEMORY_CANTPOINT 36
#define TRT_FMOD_ERR_NEEDS3D 37
#define TRT_FMOD_ERR_NEEDSHARDWARE 38
#define TRT_FMOD_ERR_NET_CONNECT 39
#define TRT_FMOD_ERR_NET_SOCKET_ERROR 40
#define TRT_FMOD_ERR_NET_URL 41
#define TRT_FMOD_ERR_NET_WOULD_BLOCK 42
#define TRT_FMOD_ERR_NOTREADY 43
#define TRT_FMOD_ERR_OUTPUT_DRIVERCALL 44
#define TRT_FMOD_ERR_OUTPUT_FORMAT 45
#define TRT_FMOD_ERR_OUTPUT_INIT 46
#define TRT_FMOD_ERR_OUTPUT_NOSOFTWARE 47
#define TRT_FMOD_ERR_PAN 48
#define TRT_FMOD_ERR_PLUGIN 49
#define TRT_FMOD_ERR_PLUGIN_MISSING 50
#define TRT_FMOD_ERR_PLUGIN_RESOURCE 51
#define TRT_FMOD_ERR_PLUGIN_VERSION 52
#define TRT_FMOD_ERR_RECORD 53
#define TRT_FMOD_ERR_REVERB_CHANNELGROUP 54
#define TRT_FMOD_ERR_REVERB_INSTANCE 55
#define TRT_FMOD_ERR_SUBSOUNDS 56
#define TRT_FMOD_ERR_SUBSOUND_ALLOCATED 57
#define TRT_FMOD_ERR_SUBSOUND_CANTMOVE 58
#define TRT_FMOD_ERR_TAGNOTFOUND 59
#define TRT_FMOD_ERR_TOOMANYCHANNELS 60
#define TRT_FMOD_ERR_TRUNCATED 61
#define TRT_FMOD_ERR_UNIMPLEMENTED 62
#define TRT_FMOD_ERR_UNINITIALIZED 63
#define TRT_FMOD_ERR_UNSUPPORTED 64
#define TRT_FMOD_ERR_VERSION 65
#define TRT_FMOD_ERR_EVENT_ALREADY_LOADED 66
#define TRT_FMOD_ERR_EVENT_LIVEUPDATE_BUSY 67
#define TRT_FMOD_ERR_EVENT_LIVEUPDATE_MISMATCH 68
#define TRT_FMOD_ERR_EVENT_LIVEUPDATE_TIMEOUT 69
#define TRT_FMOD_ERR_EVENT_NOTFOUND 70
#define TRT_FMOD_ERR_STUDIO_UNINITIALIZED 71
#define TRT_FMOD_ERR_STUDIO_NOT_LOADED 72
#define TRT_FMOD_ERR_INVALID_STRING 73
#define TRT_FMOD_ERR_ALREADY_LOCKED 74
#define TRT_FMOD_ERR_NOT_LOCKED 75
#define TRT_FMOD_ERR_RECORD_DISCONNECTED 76
#define TRT_FMOD_ERR_TOOMANYSAMPLES 77

// ---- FMOD Studio enums (2.02 public values) ----
#define TRT_STUDIO_LOADING_UNLOADING 0
#define TRT_STUDIO_LOADING_UNLOADED 1
#define TRT_STUDIO_LOADING_LOADING 2
#define TRT_STUDIO_LOADING_LOADED 3
#define TRT_STUDIO_LOADING_ERROR 4

#define TRT_STUDIO_PLAYBACK_PLAYING 0
#define TRT_STUDIO_PLAYBACK_SUSTAINING 1
#define TRT_STUDIO_PLAYBACK_STOPPED 2
#define TRT_STUDIO_PLAYBACK_STARTING 3
#define TRT_STUDIO_PLAYBACK_STOPPING 4

#define TRT_STUDIO_STOP_ALLOWFADEOUT 0
#define TRT_STUDIO_STOP_IMMEDIATE 1

#define TRT_STUDIO_LOAD_BANK_NORMAL 0
#define TRT_STUDIO_LOAD_BANK_NONBLOCKING 1
#define TRT_STUDIO_LOAD_BANK_DECOMPRESS_SAMPLES 2
#define TRT_STUDIO_LOAD_BANK_UNENCRYPTED 4

// 3D attributes: 12 floats (position, velocity, forward, up), as in
// FMOD_STUDIO_3D_ATTRIBUTES.
typedef struct trt_fmod_3d_attrs {
  float position[3];
  float velocity[3];
  float forward[3];
  float up[3];
} trt_fmod_3d_attrs;

// Parameter description (mirrors FMOD_STUDIO_PARAMETER_DESCRIPTION as the
// C# wrapper's PARAMETER_DESCRIPTION_INTERNAL sees it: name pointer,
// 8-byte id, min/max/default, type, flags).
typedef struct trt_fmod_param_desc {
  const char* name;  // owned by the bank; valid while the bank is loaded
  u32 id_data1;
  u32 id_data2;
  float minimum;
  float maximum;
  float default_value;
  int type;   // FMOD_STUDIO_PARAMETER_TYPE
  int flags;  // FMOD_STUDIO_PARAMETER_FLAGS
} trt_fmod_param_desc;

// Event callback (FMOD_STUDIO_EVENT_CALLBACK): type bitmask, instance,
// parameters pointer. Stored per instance/description; invoked from the
// engine's update thread context (the game thread's Update call).
typedef int (*trt_fmod_event_callback)(int type, void* instance, void* parameters);

// ---- system ----
int trt_fmod_studio_system_create(void** out, u32 header_version);
int trt_fmod_studio_system_is_valid(void* sys);
int trt_fmod_studio_system_init(void* sys, int maxchannels, u32 studio_flags,
                                u32 core_flags, void* extra);
int trt_fmod_studio_system_release(void* sys);
int trt_fmod_studio_system_update(void* sys);
int trt_fmod_studio_system_get_low_level(void* sys, void** out);
int trt_fmod_studio_system_get_num_listeners(void* sys, int* out);
int trt_fmod_studio_system_set_num_listeners(void* sys, int count);
int trt_fmod_studio_system_get_listener_attrs(void* sys, int index,
                                              trt_fmod_3d_attrs* out);
int trt_fmod_studio_system_set_listener_attrs(void* sys, int index,
                                              const trt_fmod_3d_attrs* attrs);
int trt_fmod_studio_system_unload_all(void* sys);
int trt_fmod_studio_system_flush_commands(void* sys);
int trt_fmod_studio_system_flush_sample_loading(void* sys);
int trt_fmod_studio_system_get_bank_count(void* sys, int* out);
int trt_fmod_studio_system_get_bank_list(void* sys, void** out, int capacity,
                                         int* count);
int trt_fmod_studio_system_get_user_data(void* sys, void** out);
int trt_fmod_studio_system_set_user_data(void* sys, void* data);

// ---- banks ----
int trt_fmod_studio_load_bank_file(void* sys, const char* path, u32 flags,
                                   void** out);
int trt_fmod_studio_bank_is_valid(void* bank);
int trt_fmod_studio_bank_unload(void* bank);
int trt_fmod_studio_bank_load_sample_data(void* bank);
int trt_fmod_studio_bank_unload_sample_data(void* bank);
int trt_fmod_studio_bank_get_loading_state(void* bank, int* out);
int trt_fmod_studio_bank_get_sample_loading_state(void* bank, int* out);
int trt_fmod_studio_bank_get_id(void* bank, u8 out_guid[16]);
int trt_fmod_studio_bank_get_path(void* bank, char* out, int size,
                                  int* retrieved);
int trt_fmod_studio_bank_get_event_count(void* bank, int* out);
int trt_fmod_studio_bank_get_event_list(void* bank, void** out, int capacity,
                                        int* count);
int trt_fmod_studio_bank_get_bus_count(void* bank, int* out);
int trt_fmod_studio_bank_get_bus_list(void* bank, void** out, int capacity,
                                      int* count);
int trt_fmod_studio_bank_get_vca_count(void* bank, int* out);
int trt_fmod_studio_bank_get_vca_list(void* bank, void** out, int capacity,
                                      int* count);
int trt_fmod_studio_bank_get_string_count(void* bank, int* out);
int trt_fmod_studio_bank_get_string_info(void* bank, int index,
                                         u8 out_guid[16], char* path,
                                         int path_size, int* retrieved);
int trt_fmod_studio_bank_get_user_data(void* bank, void** out);
int trt_fmod_studio_bank_set_user_data(void* bank, void* data);

// ---- events (lookup + description) ----
int trt_fmod_studio_get_event(void* sys, const char* path, void** out);
int trt_fmod_studio_get_event_by_id(void* sys, const u8 guid[16], void** out);
int trt_fmod_studio_event_get_id(void* desc, u8 out_guid[16]);
int trt_fmod_studio_event_get_path(void* desc, char* out, int size,
                                   int* retrieved);
int trt_fmod_studio_event_get_length(void* desc, int* out_ms);
int trt_fmod_studio_event_is_3d(void* desc, int* out);
int trt_fmod_studio_event_is_snapshot(void* desc, int* out);
int trt_fmod_studio_event_is_oneshot(void* desc, int* out);
int trt_fmod_studio_event_is_stream(void* desc, int* out);
int trt_fmod_studio_event_has_cue(void* desc, int* out);
int trt_fmod_studio_event_get_min_distance(void* desc, float* out);
int trt_fmod_studio_event_get_max_distance(void* desc, float* out);
int trt_fmod_studio_event_get_sound_size(void* desc, float* out);
int trt_fmod_studio_event_get_parameter_count(void* desc, int* out);
int trt_fmod_studio_event_get_parameter_by_index(void* desc, int index,
                                                 trt_fmod_param_desc* out);
int trt_fmod_studio_event_get_parameter(void* desc, const char* name,
                                        trt_fmod_param_desc* out);
int trt_fmod_studio_event_create_instance(void* desc, void** out);
int trt_fmod_studio_event_get_instance_count(void* desc, int* out);
int trt_fmod_studio_event_get_instance_list(void* desc, void** out,
                                            int capacity, int* count);
int trt_fmod_studio_event_load_sample_data(void* desc);
int trt_fmod_studio_event_unload_sample_data(void* desc);
int trt_fmod_studio_event_get_sample_loading_state(void* desc, int* out);
int trt_fmod_studio_event_release_all_instances(void* desc);
int trt_fmod_studio_event_set_callback(void* desc,
                                       trt_fmod_event_callback cb, int mask);
int trt_fmod_studio_event_get_user_data(void* desc, void** out);
int trt_fmod_studio_event_set_user_data(void* desc, void* data);
int trt_fmod_studio_event_is_valid(void* desc);

// ---- event instances ----
int trt_fmod_studio_instance_start(void* inst);
int trt_fmod_studio_instance_stop(void* inst, int mode);
int trt_fmod_studio_instance_release(void* inst);
int trt_fmod_studio_instance_get_playback_state(void* inst, int* out);
int trt_fmod_studio_instance_get_paused(void* inst, int* out);
int trt_fmod_studio_instance_set_paused(void* inst, int paused);
int trt_fmod_studio_instance_get_volume(void* inst, float* volume,
                                        float* final_volume);
int trt_fmod_studio_instance_set_volume(void* inst, float volume);
int trt_fmod_studio_instance_get_pitch(void* inst, float* pitch,
                                       float* final_pitch);
int trt_fmod_studio_instance_set_pitch(void* inst, float pitch);
int trt_fmod_studio_instance_get_3d_attrs(void* inst,
                                          trt_fmod_3d_attrs* out);
int trt_fmod_studio_instance_set_3d_attrs(void* inst,
                                          const trt_fmod_3d_attrs* attrs);
int trt_fmod_studio_instance_get_listener_mask(void* inst, u32* out);
int trt_fmod_studio_instance_set_listener_mask(void* inst, u32 mask);
int trt_fmod_studio_instance_get_property(void* inst, int property,
                                          float* out);
int trt_fmod_studio_instance_set_property(void* inst, int property,
                                          float value);
int trt_fmod_studio_instance_get_reverb_level(void* inst, int index,
                                              float* out);
int trt_fmod_studio_instance_set_reverb_level(void* inst, int index,
                                              float level);
int trt_fmod_studio_instance_get_timeline_position(void* inst, int* out_ms);
int trt_fmod_studio_instance_set_timeline_position(void* inst, int pos_ms);
int trt_fmod_studio_instance_get_channel_group(void* inst, void** out);
int trt_fmod_studio_instance_is_virtual(void* inst, int* out);
int trt_fmod_studio_instance_trigger_cue(void* inst);
int trt_fmod_studio_instance_set_callback(void* inst,
                                          trt_fmod_event_callback cb,
                                          int mask);
int trt_fmod_studio_instance_get_description(void* inst, void** out);
int trt_fmod_studio_instance_get_user_data(void* inst, void** out);
int trt_fmod_studio_instance_set_user_data(void* inst, void* data);
int trt_fmod_studio_instance_is_valid(void* inst);
// parameters (by name, as Celeste calls them)
int trt_fmod_studio_instance_set_parameter_value(void* inst, const char* name,
                                                 float value);
int trt_fmod_studio_instance_get_parameter_value(void* inst, const char* name,
                                                 float* value,
                                                 float* final_value);
int trt_fmod_studio_instance_set_parameter_value_by_index(void* inst,
                                                          int index,
                                                          float value);
int trt_fmod_studio_instance_get_parameter_value_by_index(void* inst,
                                                          int index,
                                                          float* value,
                                                          float* final_value);
int trt_fmod_studio_instance_get_parameter(void* inst, const char* name,
                                           void** out);
int trt_fmod_studio_instance_get_parameter_by_index(void* inst, int index,
                                                    void** out);
int trt_fmod_studio_instance_get_parameter_count(void* inst, int* out);
int trt_fmod_studio_parameter_get_value(void* param, float* out);
int trt_fmod_studio_parameter_set_value(void* param, float value);
int trt_fmod_studio_parameter_get_description(void* param,
                                              trt_fmod_param_desc* out);
int trt_fmod_studio_parameter_is_valid(void* param);

// ---- buses / VCAs ----
int trt_fmod_studio_get_bus(void* sys, const char* path, void** out);
int trt_fmod_studio_get_bus_by_id(void* sys, const u8 guid[16], void** out);
int trt_fmod_studio_bus_get_id(void* bus, u8 out_guid[16]);
int trt_fmod_studio_bus_get_path(void* bus, char* out, int size,
                                 int* retrieved);
int trt_fmod_studio_bus_get_volume(void* bus, float* volume,
                                   float* final_volume);
int trt_fmod_studio_bus_set_volume(void* bus, float volume);
int trt_fmod_studio_bus_get_paused(void* bus, int* out);
int trt_fmod_studio_bus_set_paused(void* bus, int paused);
int trt_fmod_studio_bus_get_mute(void* bus, int* out);
int trt_fmod_studio_bus_set_mute(void* bus, int mute);
int trt_fmod_studio_bus_stop_all_events(void* bus, int mode);
int trt_fmod_studio_bus_get_channel_group(void* bus, void** out);
int trt_fmod_studio_bus_is_valid(void* bus);
int trt_fmod_studio_get_vca(void* sys, const char* path, void** out);
int trt_fmod_studio_get_vca_by_id(void* sys, const u8 guid[16], void** out);
int trt_fmod_studio_vca_get_id(void* vca, u8 out_guid[16]);
int trt_fmod_studio_vca_get_path(void* vca, char* out, int size,
                                 int* retrieved);
int trt_fmod_studio_vca_get_volume(void* vca, float* volume,
                                   float* final_volume);
int trt_fmod_studio_vca_set_volume(void* vca, float volume);
int trt_fmod_studio_vca_is_valid(void* vca);

// ---- GUID / path lookup ----
int trt_fmod_studio_lookup_id(void* sys, const char* path, u8 out_guid[16]);
int trt_fmod_studio_lookup_path(void* sys, const u8 guid[16], char* out,
                                int size, int* retrieved);

// ---- test introspection (NOT part of the FMOD surface; the harness and
// unit tests use these to prove the real engine is in use) ----
int trt_fmod_studio_total_event_count(void* sys);
int trt_fmod_studio_total_sample_count(void* sys);
u64 trt_fmod_studio_total_pcm_bytes_submitted(void* sys);
int trt_fmod_studio_bank_sample_count(void* bank);
// Handle-free: parsed events across ALL live Studio systems. The
// harness test uses this — the game's system handle lives inside
// managed code and the test cannot know it.
int trt_fmod_studio_global_event_count();
// Current PCM decode cache size in bytes (g_cache_bytes). Memory
// instrumentation for the Celeste OOM investigation (oom-analysis.md):
// the Mono watchdog (patch 06) logs this on every tick, and bank_load
// (patch 07) logs it after each bank load.
u64 trt_fmod_studio_pcm_cache_bytes();

#ifdef __cplusplus
}
#endif

#endif  // TIRAMISU_BRIDGE_FMOD_STUDIO_H
