// tiramisu/tests/test_mono_doors.cpp
// Door-level test for the REAL mono dllmap .so files:
//   libfmod-real.so, libcsteamworks-real.so
// dlopens both and calls the flat door functions the way Mono's P/Invoke
// binder would. Asserts the REAL implementations are in use (markers,
// real bank event counts, real bridged Steam ID) and that unimplemented
// functions fail VISIBLE (FMOD_ERR_UNSUPPORTED / false), never silent
// success.
//
// Usage: test_mono_doors <libfmod-real.so> <libcsteamworks-real.so> <bank-dir>
// The bank dir is the game's FMOD/Desktop dir (real .bank files).
#include <dlfcn.h>

#include <cstdio>
#include <cstring>
#include <unistd.h>

static int g_fail = 0;
#define CHECK(c, ...)                                            \
  do {                                                           \
    if (!(c)) {                                                  \
      ++g_fail;                                                  \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);                \
      printf(__VA_ARGS__);                                       \
      printf("\n");                                              \
    } else {                                                     \
      printf("PASS ");                                           \
      printf(__VA_ARGS__);                                       \
      printf("\n");                                              \
    }                                                            \
  } while (0)

static void* must_sym(void* h, const char* name) {
  dlerror();
  void* p = dlsym(h, name);
  const char* err = dlerror();
  if (!p || err) {
    ++g_fail;
    printf("FAIL dlsym %s: %s\n", name, err ? err : "NULL");
    return nullptr;
  }
  return p;
}

// ---- voice-hook capture (installed into the FMOD .so) ----
struct cap {
  int submits = 0;
  unsigned long long bytes = 0;
  int rate = 0, channels = 0, bps = 0;
};
static cap g_cap;
static int cap_submit(unsigned long long voice, const void* pcm, unsigned bytes,
                      unsigned rate, unsigned channels, unsigned bps,
                      unsigned is_float, float volume, void* user) {
  (void)voice;
  (void)pcm;
  (void)is_float;
  (void)volume;
  (void)user;
  ++g_cap.submits;
  g_cap.bytes += bytes;
  g_cap.rate = (int)rate;
  g_cap.channels = (int)channels;
  g_cap.bps = (int)bps;
  return 1;
}

// FMOD_RESULT values the test asserts on (real SDK 2.02).
#define FMOD_OK 0
#define FMOD_ERR_UNSUPPORTED 64
#define FMOD_ERR_EVENT_NOTFOUND 70

// Studio enum values (real SDK 2.02).
#define LOADING_LOADED 3
#define PLAYBACK_PLAYING 0
#define PLAYBACK_STOPPED 2

// Controller struct returns (SysV ABI: 2-byte / 16-byte / 40-byte).
struct ctrl_digital {
  unsigned char bState;
  unsigned char bActive;
};
struct ctrl_analog {
  int eMode;
  float x, y;
  unsigned char bActive;
  unsigned char _pad[3];
};
struct ctrl_motion {
  float v[10];
};

int main(int argc, char** argv) {
  if (argc < 4) {
    printf("usage: %s <libfmod-real.so> <libcsteamworks-real.so> <bank-dir>\n",
           argv[0]);
    return 2;
  }
  const char* fmod_so = argv[1];
  const char* steam_so = argv[2];
  const char* bank_dir = argv[3];

  void* hf = dlopen(fmod_so, RTLD_NOW | RTLD_LOCAL);
  if (!hf) {
    printf("FAIL dlopen fmod: %s\n", dlerror());
    return 1;
  }
  void* hs = dlopen(steam_so, RTLD_NOW | RTLD_LOCAL);
  if (!hs) {
    printf("FAIL dlopen steam: %s\n", dlerror());
    return 1;
  }

  // ============ markers: prove the REAL .so files are loaded ============
  auto fmod_is_real =
      (int (*)())must_sym(hf, "trt_mono_bridge_is_real");
  auto fmod_name =
      (const char* (*)())must_sym(hf, "trt_mono_bridge_name");
  auto steam_is_real =
      (int (*)())must_sym(hs, "trt_mono_bridge_is_real");
  auto steam_name =
      (const char* (*)())must_sym(hs, "trt_mono_bridge_name");
  if (fmod_is_real) CHECK(fmod_is_real() == 1, "fmod marker is_real==1");
  if (fmod_name)
    CHECK(strcmp(fmod_name(), "fmod-real") == 0, "fmod marker name==fmod-real");
  if (steam_is_real) CHECK(steam_is_real() == 1, "steam marker is_real==1");
  if (steam_name)
    CHECK(strcmp(steam_name(), "steam-real") == 0,
          "steam marker name==steam-real");

  // ============ FMOD: core ============
  auto sys_create = (int (*)(void**))must_sym(hf, "FMOD_System_Create");
  auto sys_init =
      (int (*)(void*, int, unsigned, void*))must_sym(hf, "FMOD_System_Init");
  auto sys_get_version =
      (int (*)(void*, unsigned*))must_sym(hf, "FMOD_System_GetVersion");
  auto sys_get_drivers =
      (int (*)(void*, int*))must_sym(hf, "FMOD_System_GetNumDrivers");
  auto sys_get_format = (int (*)(void*, int*, int*, int*))must_sym(
      hf, "FMOD_System_GetSoftwareFormat");
  void* core = nullptr;
  if (sys_create) CHECK(sys_create(&core) == FMOD_OK && core, "core Create");
  if (sys_init) CHECK(sys_init(core, 64, 0, nullptr) == FMOD_OK, "core Init");
  if (sys_get_version) {
    unsigned v = 0;
    CHECK(sys_get_version(core, &v) == FMOD_OK && v == 0x00020209,
          "core GetVersion==2.02.09 (got 0x%x)", v);
  }
  if (sys_get_drivers) {
    int n = 0;
    CHECK(sys_get_drivers(core, &n) == FMOD_OK && n == 1,
          "core GetNumDrivers==1");
  }
  if (sys_get_format) {
    int rate = 0, mode = 0, raw = 0;
    CHECK(sys_get_format(core, &rate, &mode, &raw) == FMOD_OK &&
              rate == 48000 && mode == 3 && raw == 2,
          "core GetSoftwareFormat==48000/stereo/2 (got %d/%d/%d)", rate, mode,
          raw);
  }

  // ============ FMOD: Studio ============
  auto set_hook = (void (*)(void*, void*))must_sym(
      hf, "trt_audio_set_voice_hook");
  if (set_hook) set_hook((void*)cap_submit, nullptr);

  auto st_create =
      (int (*)(void**, unsigned))must_sym(hf, "FMOD_Studio_System_Create");
  auto st_init = (int (*)(void*, int, unsigned, unsigned,
                          void*))must_sym(hf, "FMOD_Studio_System_Initialize");
  auto st_update = (int (*)(void*))must_sym(hf, "FMOD_Studio_System_Update");
  auto st_load = (int (*)(void*, const unsigned char*, unsigned,
                          void**))must_sym(hf, "FMOD_Studio_System_LoadBankFile");
  auto st_get_event = (int (*)(void*, const unsigned char*,
                               void**))must_sym(hf, "FMOD_Studio_System_GetEvent");
  auto st_get_vca = (int (*)(void*, const unsigned char*,
                             void**))must_sym(hf, "FMOD_Studio_System_GetVCA");
  auto st_get_bus = (int (*)(void*, const unsigned char*,
                             void**))must_sym(hf, "FMOD_Studio_System_GetBus");
  auto bank_count =
      (int (*)(void*, int*))must_sym(hf, "FMOD_Studio_Bank_GetEventCount");
  auto bank_loading = (int (*)(void*, int*))must_sym(
      hf, "FMOD_Studio_Bank_GetLoadingState");
  auto ev_path = (int (*)(void*, unsigned char*, int,
                          int*))must_sym(hf, "FMOD_Studio_EventDescription_GetPath");
  auto ev_length =
      (int (*)(void*, int*))must_sym(hf, "FMOD_Studio_EventDescription_GetLength");
  auto ev_create = (int (*)(void*, void**))must_sym(
      hf, "FMOD_Studio_EventDescription_CreateInstance");
  auto ev_load_samples = (int (*)(void*))must_sym(
      hf, "FMOD_Studio_EventDescription_LoadSampleData");
  auto inst_start =
      (int (*)(void*))must_sym(hf, "FMOD_Studio_EventInstance_Start");
  auto inst_stop = (int (*)(void*, int))must_sym(
      hf, "FMOD_Studio_EventInstance_Stop");
  auto inst_state = (int (*)(void*, int*))must_sym(
      hf, "FMOD_Studio_EventInstance_GetPlaybackState");
  auto vca_get_vol = (int (*)(void*, float*, float*))must_sym(
      hf, "FMOD_Studio_VCA_GetVolume");
  auto bus_get_vol = (int (*)(void*, float*, float*))must_sym(
      hf, "FMOD_Studio_Bus_GetVolume");
  auto parse_id = (int (*)(const unsigned char*,
                           unsigned char*))must_sym(hf, "FMOD_Studio_ParseID");
  auto st_get_bank = (int (*)(void*, const unsigned char*,
                              void**))must_sym(hf, "FMOD_Studio_System_GetBank");

  void* studio = nullptr;
  if (st_create) CHECK(st_create(&studio, 0) == FMOD_OK && studio, "studio Create");
  if (st_init) CHECK(st_init(studio, 1024, 0, 0, nullptr) == FMOD_OK, "studio Init");

  char path[1024];
  void* strings_bank = nullptr;
  void* sfx_bank = nullptr;
  if (st_load) {
    snprintf(path, sizeof(path), "%s/Master Bank.strings.bank", bank_dir);
    CHECK(st_load(studio, (const unsigned char*)path, 0, &strings_bank) ==
              FMOD_OK &&
              strings_bank,
          "LoadBank strings");
    snprintf(path, sizeof(path), "%s/sfx.bank", bank_dir);
    CHECK(st_load(studio, (const unsigned char*)path, 0, &sfx_bank) == FMOD_OK &&
              sfx_bank,
          "LoadBank sfx");
  }
  if (bank_loading) {
    int st = -1;
    CHECK(bank_loading(sfx_bank, &st) == FMOD_OK && st == LOADING_LOADED,
          "sfx bank LOADED (got %d)", st);
  }
  if (bank_count) {
    int n = 0;
    CHECK(bank_count(sfx_bank, &n) == FMOD_OK && n == 587,
          "sfx bank event count==587 (got %d)", n);
  }
  {
    // Handle-free global introspection (what the harness test uses).
    auto global_events =
        (int (*)())must_sym(hf, "trt_fmod_studio_global_event_count");
    if (global_events)
      CHECK(global_events() == 587,
            "global event count==587 (got %d)", global_events());
  }
  void* worldmap = nullptr;
  if (st_get_event) {
    CHECK(st_get_event(studio, (const unsigned char*)"event:/env/amb/worldmap",
                       &worldmap) == FMOD_OK &&
              worldmap,
          "GetEvent worldmap");
    void* missing = nullptr;
    CHECK(st_get_event(studio, (const unsigned char*)"event:/does/not/exist",
                       &missing) == FMOD_ERR_EVENT_NOTFOUND,
          "GetEvent missing -> ERR_EVENT_NOTFOUND(70)");
  }
  if (ev_path && worldmap) {
    unsigned char buf[256];
    int retrieved = 0;
    CHECK(ev_path(worldmap, buf, (int)sizeof(buf), &retrieved) == FMOD_OK &&
              strcmp((const char*)buf, "event:/env/amb/worldmap") == 0 &&
              retrieved == 24,  // 23 chars + NUL
          "GetPath worldmap (retrieved=%d)", retrieved);
  }
  if (ev_length && worldmap) {
    int len = 0;
    CHECK(ev_length(worldmap, &len) == FMOD_OK && len >= 33000 && len <= 34000,
          "GetLength worldmap ~33700ms (got %d)", len);
  }
  void* inst = nullptr;
  if (ev_create && worldmap)
    CHECK(ev_create(worldmap, &inst) == FMOD_OK && inst, "CreateInstance");
  if (ev_load_samples && worldmap)
    CHECK(ev_load_samples(worldmap) == FMOD_OK, "Event LoadSampleData");
  if (inst_start && inst) {
    CHECK(inst_start(inst) == FMOD_OK, "Instance Start");
    if (st_update) st_update(studio);
    usleep(600 * 1000);
    if (st_update) st_update(studio);
    usleep(600 * 1000);
    if (inst_state) {
      int ps = -1;
      CHECK(inst_state(inst, &ps) == FMOD_OK && ps == PLAYBACK_PLAYING,
            "Instance PLAYING (got %d)", ps);
    }
    CHECK(g_cap.submits > 0 && g_cap.bytes > 0,
          "voice hook got PCM (%d submits, %llu bytes)", g_cap.submits,
          g_cap.bytes);
    CHECK(g_cap.rate == 48000 && g_cap.channels == 2 && g_cap.bps == 2,
          "PCM format 48kHz/stereo/16-bit (got %d/%d/%d)", g_cap.rate,
          g_cap.channels, g_cap.bps);
    if (inst_stop) CHECK(inst_stop(inst, 1) == FMOD_OK, "Instance Stop");
    usleep(200 * 1000);
    if (inst_state) {
      int ps = -1;
      CHECK(inst_state(inst, &ps) == FMOD_OK && ps == PLAYBACK_STOPPED,
            "Instance STOPPED (got %d)", ps);
    }
  }
  void* vca = nullptr;
  if (st_get_vca) {
    CHECK(st_get_vca(studio, (const unsigned char*)"vca:/music", &vca) ==
              FMOD_OK &&
              vca,
          "GetVCA music");
    if (vca_get_vol && vca) {
      float vol = -1, final_vol = -1;
      CHECK(vca_get_vol(vca, &vol, &final_vol) == FMOD_OK && vol == 1.0f,
            "VCA music volume==1.0 (got %f)", vol);
    }
  }
  void* bus = nullptr;
  if (st_get_bus) {
    CHECK(st_get_bus(studio, (const unsigned char*)"bus:/music", &bus) ==
              FMOD_OK &&
              bus,
          "GetBus music");
    if (bus_get_vol && bus) {
      float vol = -1, final_vol = -1;
      CHECK(bus_get_vol(bus, &vol, &final_vol) == FMOD_OK && vol == 1.0f,
            "Bus music volume==1.0 (got %f)", vol);
    }
  }
  if (parse_id) {
    unsigned char guid[16];
    CHECK(parse_id((const unsigned char*)"{00112233-4455-6677-8899-aabbccddeeff}",
                   guid) == FMOD_OK &&
              guid[0] == 0x33 && guid[1] == 0x22 && guid[2] == 0x11 &&
              guid[3] == 0x00 && guid[4] == 0x55 && guid[5] == 0x44 &&
              guid[6] == 0x77 && guid[7] == 0x66 && guid[8] == 0x88 &&
              guid[9] == 0x99 && guid[15] == 0xff,
          "ParseID GUID byte order");
    CHECK(parse_id((const unsigned char*)"not-a-guid", guid) != FMOD_OK,
          "ParseID rejects garbage");
  }
  if (st_get_bank) {
    // GetBank is NOT implemented (no Bank-by-path lookup in the engine):
    // it must fail VISIBLE, never pretend success.
    void* b = nullptr;
    CHECK(st_get_bank(studio, (const unsigned char*)"bank:/Master Bank", &b) ==
              FMOD_ERR_UNSUPPORTED,
          "GetBank -> ERR_UNSUPPORTED(64) fail-visible");
  }

  // ============ Steam ============
  auto set_appid =
      (void (*)(unsigned))must_sym(hs, "trt_steam_set_appid");
  if (set_appid) set_appid(504230);
  auto s_init = (int (*)())must_sym(hs, "Init");
  auto s_shutdown = (void (*)())must_sym(hs, "Shutdown");
  auto s_restart =
      (int (*)(unsigned))must_sym(hs, "RestartAppIfNecessary");
  auto s_running = (int (*)())must_sym(hs, "IsSteamRunning");
  auto s_client = (void* (*)())must_sym(hs, "SteamClient_");
  auto s_run_cb = (void (*)())must_sym(hs, "RunCallbacks");
  auto s_get_user = (void* (*)(int, int, const char*))must_sym(
      hs, "ISteamClient_GetISteamUser");
  auto s_get_utils = (void* (*)(int, const char*))must_sym(
      hs, "ISteamClient_GetISteamUtils");
  auto s_get_id = (unsigned long long (*)())must_sym(hs, "ISteamUser_GetSteamID");
  auto s_logged_on = (int (*)())must_sym(hs, "ISteamUser_BLoggedOn");
  auto s_user_data_folder =
      (int (*)(char*, int))must_sym(hs, "ISteamUser_GetUserDataFolder");
  auto s_get_appid = (unsigned (*)())must_sym(hs, "ISteamUtils_GetAppID");
  auto s_ui_lang =
      (const char* (*)())must_sym(hs, "ISteamUtils_GetSteamUILanguage");
  auto s_game_lang =
      (const char* (*)())must_sym(hs, "ISteamApps_GetCurrentGameLanguage");
  auto s_persona = (const char* (*)())must_sym(hs, "ISteamFriends_GetPersonaName");
  auto s_rs_write = (int (*)(const char*, const void*,
                             int))must_sym(hs, "ISteamRemoteStorage_FileWrite");
  auto s_rs_read = (int (*)(const char*, void*, int))must_sym(
      hs, "ISteamRemoteStorage_FileRead");
  auto s_rs_exists =
      (int (*)(const char*))must_sym(hs, "ISteamRemoteStorage_FileExists");
  auto s_rs_persisted = (int (*)(const char*))must_sym(
      hs, "ISteamRemoteStorage_FilePersisted");
  auto s_rs_delete =
      (int (*)(const char*))must_sym(hs, "ISteamRemoteStorage_FileDelete");
  auto s_rs_size =
      (int (*)(const char*))must_sym(hs, "ISteamRemoteStorage_GetFileSize");
  auto s_req_stats =
      (int (*)())must_sym(hs, "ISteamUserStats_RequestCurrentStats");
  auto s_digital = (ctrl_digital (*)(unsigned long long,
                                     unsigned long long))must_sym(
      hs, "ISteamController_GetDigitalActionData");
  auto s_analog = (ctrl_analog (*)(unsigned long long,
                                   unsigned long long))must_sym(
      hs, "ISteamController_GetAnalogActionData");
  auto s_motion = (ctrl_motion (*)(unsigned long long))must_sym(
      hs, "ISteamController_GetMotionData");

  if (s_init) CHECK(s_init() == 1, "Steam Init==true");
  if (s_running) CHECK(s_running() == 1, "Steam IsSteamRunning==true");
  if (s_restart)
    CHECK(s_restart(504230) == 0, "Steam RestartAppIfNecessary==false");
  void* client = nullptr;
  if (s_client) {
    client = s_client();
    CHECK(client != nullptr, "SteamClient_ non-NULL");
  }
  if (s_get_user) {
    void* u = s_get_user(1, 1, "SteamUser023");
    CHECK(u != nullptr, "GetISteamUser non-NULL");
  }
  if (s_get_id)
    CHECK(s_get_id() == 0x0110000100000001ULL,
          "GetSteamID==76561197960265729 (got %llu)", s_get_id());
  if (s_logged_on) CHECK(s_logged_on() == 1, "BLoggedOn==true");
  if (s_user_data_folder) {
    char buf[512];
    memset(buf, 0, sizeof(buf));
    CHECK(s_user_data_folder(buf, (int)sizeof(buf)) == 1 && buf[0] != 0,
          "GetUserDataFolder non-empty (%s)", buf);
  }
  if (s_get_utils) {
    void* u = s_get_utils(1, "SteamUtils010");
    CHECK(u != nullptr, "GetISteamUtils non-NULL");
  }
  if (s_get_appid)
    CHECK(s_get_appid() == 504230, "GetAppID==504230 (got %u)", s_get_appid());
  if (s_ui_lang)
    CHECK(strcmp(s_ui_lang(), "english") == 0, "GetSteamUILanguage==english");
  if (s_game_lang)
    CHECK(strcmp(s_game_lang(), "english") == 0,
          "GetCurrentGameLanguage==english");
  if (s_persona)
    CHECK(strcmp(s_persona(), "TiramisuPlayer") == 0,
          "GetPersonaName==TiramisuPlayer");
  if (s_rs_exists)
    CHECK(s_rs_exists("doortest-missing.sav") == 0,
          "FileExists missing==false");
  if (s_rs_write && s_rs_read && s_rs_exists && s_rs_delete && s_rs_size) {
    const char data[5] = {'h', 'e', 'l', 'l', 'o'};
    CHECK(s_rs_write("doortest.bin", data, 5) == 1, "FileWrite==true");
    CHECK(s_rs_exists("doortest.bin") == 1, "FileExists written==true");
    CHECK(s_rs_size("doortest.bin") == 5, "GetFileSize==5");
    char buf[8];
    memset(buf, 0, sizeof(buf));
    CHECK(s_rs_read("doortest.bin", buf, 5) == 5 &&
              memcmp(buf, data, 5) == 0,
          "FileRead round-trips");
    if (s_rs_persisted)
      CHECK(s_rs_persisted("doortest.bin") == 1, "FilePersisted==true");
    CHECK(s_rs_delete("doortest.bin") == 1, "FileDelete==true");
    CHECK(s_rs_exists("doortest.bin") == 0, "FileExists deleted==false");
  }
  if (s_req_stats)
    CHECK(s_req_stats() == 0,
          "RequestCurrentStats==false (no stats backend, fail-visible)");
  if (s_digital) {
    ctrl_digital d = s_digital(0, 0);
    CHECK(d.bState == 0 && d.bActive == 0, "DigitalActionData zeroed");
  }
  if (s_analog) {
    ctrl_analog a = s_analog(0, 0);
    CHECK(a.eMode == 0 && a.x == 0 && a.y == 0 && a.bActive == 0,
          "AnalogActionData zeroed");
  }
  if (s_motion) {
    ctrl_motion m = s_motion(0);
    int allzero = 1;
    for (int i = 0; i < 10; ++i)
      if (m.v[i] != 0) allzero = 0;
    CHECK(allzero, "MotionData zeroed (40-byte sret ABI)");
  }
  if (s_run_cb) {
    s_run_cb();
    printf("PASS RunCallbacks no-crash\n");
  }
  if (s_shutdown) {
    s_shutdown();
    printf("PASS Shutdown no-crash\n");
  }

  printf("RESULT: %s\n", g_fail ? "FAIL" : "PASS");
  return g_fail ? 1 : 0;
}
