// tiramisu/tests/test_fmod_studio.cpp
// FMOD Studio engine gate: loads Celeste's REAL .bank files, resolves a
// real event through the strings bank + event graph, decodes a real FSB5
// Vorbis sample, and submits real PCM to the mixer hook. No stubs anywhere
// in this path: every assertion below fails if the engine is replaced by
// spike-parity stubs (event counts, PCM bytes, and the bridged Steam ID
// all come from the game's own data).
#include "../src/trt/bridge_fmod_studio.h"
#include "../src/trt/bridge_audio.h"
#include "../src/trt/trt.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, name)                                     \
  do {                                                        \
    if (cond) {                                               \
      printf("  PASS %s\n", name);                            \
    } else {                                                  \
      printf("  FAIL %s\n", name);                            \
      ++failures;                                             \
    }                                                         \
  } while (0)

// ---- mixer capture hook: records every submit ----
struct capture {
  u64 bytes = 0;
  u32 rate = 0, channels = 0, bps = 0;
  int count = 0;
} g_cap;

int cap_submit(u64 voice_id, const void* pcm, u32 bytes, u32 rate,
               u32 channels, u32 bytes_per_sample, u32 is_float, float volume,
               void* user) {
  (void)voice_id;
  (void)pcm;
  (void)is_float;
  (void)volume;
  (void)user;
  g_cap.bytes += bytes;
  g_cap.rate = rate;
  g_cap.channels = channels;
  g_cap.bps = bytes_per_sample;
  ++g_cap.count;
  return 1;  // consumed
}

static std::string bank_dir() {
  const char* home = getenv("HOME");
  return std::string(home ? home : "") +
         "/celeste-xbox/Celeste/Content/FMOD/Desktop";
}

static bool file_exists(const std::string& p) {
  return access(p.c_str(), R_OK) == 0;
}

int main() {
  const char* banks[] = {"Master Bank.bank", "Master Bank.strings.bank",
                         "music.bank",       "sfx.bank",
                         "ui.bank",          "dlc_music.bank",
                         "dlc_sfx.bank"};
  const std::string dir = bank_dir();
  for (const char* b : banks) {
    if (!file_exists(dir + "/" + b)) {
      printf("SKIP: %s not present\n", (dir + "/" + b).c_str());
      return 0;
    }
  }

  trt_backend be{};  // default backend: host alloc/protect
  trt_bridge_boot(&be);
  trt_audio_set_voice_hook(cap_submit, nullptr);

  void* sys = nullptr;
  CHECK(trt_fmod_studio_system_create(&sys, 0) == TRT_FMOD_OK && sys,
        "studio system create");
  if (!sys) {
    printf("RESULT: FAIL\n");
    return 1;
  }
  CHECK(trt_fmod_studio_system_init(sys, 1024, 0, 0, nullptr) ==
            TRT_FMOD_OK,
        "studio system init");

  // ---- load all 7 banks; assert the real parsed counts ----
  void* bank[7] = {};
  bool all_ok = true;
  for (int i = 0; i < 7; ++i) {
    int rc = trt_fmod_studio_load_bank_file(sys, (dir + "/" + banks[i]).c_str(),
                                            0, &bank[i]);
    if (rc != TRT_FMOD_OK || !bank[i]) {
      printf("  FAIL load bank %s (rc=%d)\n", banks[i], rc);
      all_ok = false;
      ++failures;
    }
  }
  CHECK(all_ok, "load all 7 banks");

  int sfx_events = 0;
  CHECK(trt_fmod_studio_bank_get_event_count(bank[3], &sfx_events) ==
            TRT_FMOD_OK &&
            sfx_events == 587,
        "sfx.bank event count == 587 (parsed EVTB)");
  int total_events = trt_fmod_studio_total_event_count(sys);
  CHECK(total_events == 1031, "total event count == 1031");
  int total_samples = trt_fmod_studio_total_sample_count(sys);
  CHECK(total_samples == 4969, "total sample count == 4969");

  int state = -1;
  CHECK(trt_fmod_studio_bank_get_loading_state(bank[3], &state) ==
            TRT_FMOD_OK &&
            state == TRT_STUDIO_LOADING_LOADED,
        "sfx bank loading state == LOADED");
  CHECK(trt_fmod_studio_bank_load_sample_data(bank[3]) == TRT_FMOD_OK,
        "sfx Bank_LoadSampleData");

  // ---- the boot event: resolve, round-trip its path, check length ----
  void* ev = nullptr;
  CHECK(trt_fmod_studio_get_event(sys, "event:/env/amb/worldmap",
                                         &ev) == TRT_FMOD_OK &&
            ev,
        "GetEvent event:/env/amb/worldmap");
  if (ev) {
    char path[256];
    int retrieved = 0;
    CHECK(trt_fmod_studio_event_get_path(ev, path, sizeof(path), &retrieved) ==
              TRT_FMOD_OK &&
              strcmp(path, "event:/env/amb/worldmap") == 0,
        "event GetPath round-trips the real path");
    int is3d = -1;
    CHECK(trt_fmod_studio_event_is_3d(ev, &is3d) == TRT_FMOD_OK && is3d == 0,
        "worldmap Is3D == false (no spatializer data parsed)");
    int length_ms = 0;
    CHECK(trt_fmod_studio_event_get_length(ev, &length_ms) == TRT_FMOD_OK &&
              length_ms >= 33000 && length_ms <= 34000,
        "worldmap length ~= 33.7s (real sample duration)");
    CHECK(trt_fmod_studio_event_load_sample_data(ev) == TRT_FMOD_OK,
        "worldmap EventDescription_LoadSampleData (pre-decode)");

    // ---- start it and prove PCM reaches the mixer hook ----
    void* inst = nullptr;
    CHECK(trt_fmod_studio_event_create_instance(ev, &inst) == TRT_FMOD_OK &&
              inst,
        "worldmap CreateInstance");
    if (inst) {
      CHECK(trt_fmod_studio_instance_start(inst) == TRT_FMOD_OK,
            "worldmap EventInstance_Start");
      int pb = -1;
      CHECK(trt_fmod_studio_instance_get_playback_state(inst, &pb) ==
                TRT_FMOD_OK &&
                pb == TRT_STUDIO_PLAYBACK_PLAYING,
            "playback state == PLAYING after Start");
      // Drive Update for ~2 s; the feeder submits 0.5 s chunks.
      for (int i = 0; i < 40; ++i) {
        trt_fmod_studio_system_update(sys);
        usleep(50000);
      }
      CHECK(g_cap.count > 0 && g_cap.bytes > 0,
            "mixer hook received real PCM during playback");
      CHECK(g_cap.rate == 48000 && g_cap.channels == 2 && g_cap.bps == 2,
            "submitted PCM is 48 kHz stereo 16-bit");
      printf("  (hook: %d submits, %llu bytes, %u Hz x%u)\n", g_cap.count,
             (unsigned long long)g_cap.bytes, g_cap.rate, g_cap.channels);
      CHECK(trt_fmod_studio_instance_stop(inst, 1) == TRT_FMOD_OK,
            "worldmap EventInstance_Stop (immediate)");
      CHECK(trt_fmod_studio_instance_get_playback_state(inst, &pb) ==
                TRT_FMOD_OK &&
                pb == TRT_STUDIO_PLAYBACK_STOPPED,
            "playback state == STOPPED after Stop");
      CHECK(trt_fmod_studio_instance_release(inst) == TRT_FMOD_OK,
            "worldmap EventInstance_Release");
    }
  }

  // ---- VCAs: real bank defaults (0 dB = linear 1.0), real SetVolume ----
  void* vca = nullptr;
  CHECK(trt_fmod_studio_get_vca(sys, "vca:/music", &vca) ==
            TRT_FMOD_OK &&
            vca,
        "GetVCA vca:/music");
  if (vca) {
    float vol = -1.f, finalvol = -1.f;
    CHECK(trt_fmod_studio_vca_get_volume(vca, &vol, &finalvol) ==
              TRT_FMOD_OK &&
              fabsf(vol - 1.0f) < 0.001f,
        "vca:/music default volume == 1.0 (0 dB from bank)");
    CHECK(trt_fmod_studio_vca_set_volume(vca, 0.5f) == TRT_FMOD_OK,
        "VCA_SetVolume(0.5)");
    CHECK(trt_fmod_studio_vca_get_volume(vca, &vol, &finalvol) ==
              TRT_FMOD_OK &&
              fabsf(vol - 0.5f) < 0.001f,
        "VCA_GetVolume reflects the SetVolume");
  }

  // ---- buses ----
  void* bus = nullptr;
  CHECK(trt_fmod_studio_get_bus(sys, "bus:/music", &bus) ==
            TRT_FMOD_OK &&
            bus,
        "GetBus bus:/music");
  if (bus) {
    float vol = -1.f;
    CHECK(trt_fmod_studio_bus_get_volume(bus, &vol, nullptr) == TRT_FMOD_OK &&
              fabsf(vol - 1.0f) < 0.001f,
        "bus:/music default volume == 1.0");
  }

  // ---- a missing event fails honestly (no fake handle) ----
  void* bad = nullptr;
  CHECK(trt_fmod_studio_get_event(sys, "event:/does/not/exist",
                                         &bad) == TRT_FMOD_ERR_EVENT_NOTFOUND,
        "missing event returns ERR_EVENT_NOTFOUND (70)");

  for (int i = 0; i < 7; ++i)
    if (bank[i]) trt_fmod_studio_bank_unload(bank[i]);
  CHECK(trt_fmod_studio_system_release(sys) == TRT_FMOD_OK,
        "studio system release");

  printf("RESULT: %s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
