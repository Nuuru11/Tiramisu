// tiramisu/third_party/mono-bridges/mono_steam_door.cpp
// SysV front door for Steamworks.NET CSteamworks P/Invokes from managed
// code (Celeste under embedded Mono + FNA). One .so serves both the
// "CSteamworks" and "sdkencryptedappticket" dllmaps.
//
// Every real function forwards to src/trt/bridge_steam.cpp — the same
// Steam client the 64-bit native bridge uses. One Steam client, two
// front doors. The bridge's st_* vtable functions are TRT_GUEST_ABI
// (ms_abi on Linux); these wrappers are plain SysV and the compiler
// handles the ABI transition at the call.
//
// Two marshaling traps, handled here:
//  * ISteamClient_GetISteam* takes (user, pipe, version) in the managed
//    API but the bridge takes (self, pipe, user, version) — the door
//    swaps user/pipe. (GetISteamUtils has no user param at all.)
//  * ISteamUser_GetSteamID returns a plain u64 in RAX (the managed
//    signature), so the door calls st_user_GetSteamID(self, &tmp) and
//    returns tmp — NOT through the vtable (the bridge's vtable call
//    would expect the MS-ABI hidden-sret convention).
//
// Functions the bridge does NOT implement get a fail-visible function
// from gen/gen_steam_failvisible.inc (loud log + safe failure value),
// never a silent stub that pretends success. Notably
// ISteamUserStats_RequestCurrentStats / RequestGlobalStats have no stats
// backend (the bridge's vtable slots are NULL) and return false.
//
// Marker exports (the harness asserts these exist to prove the REAL .so
// is loaded — the old stubs export neither):
//   trt_mono_bridge_is_real() -> 1
//   trt_mono_bridge_name() -> "steam-real"
#include <cstdio>
#include "../../src/trt/thunk.h"
#include <cstdlib>
#include <cstring>

#include "../../src/trt/bridge_steam.h"
#include "../../src/trt/trt.h"

extern "C" {

// ---- markers: prove the real bridge is loaded ----
int trt_mono_bridge_is_real(void) { return 1; }
const char* trt_mono_bridge_name(void) { return "steam-real"; }

// ---- helpers ----
void* steam_client(void) { return &g_steam_client_obj; }
void* steam_user(void) { return &g_steam_user_obj; }
void* steam_apps(void) { return &g_steam_apps_obj; }
void* steam_utils(void) { return &g_steam_utils_obj; }
void* steam_friends(void) { return &g_steam_friends_obj; }
void* steam_userstats(void) { return &g_steam_userstats_obj; }
void* steam_rs(void) { return &g_steam_rs_obj; }
void* steam_input(void) { return &g_steam_input_obj; }

// Read steam_appid.txt the way the real Steam client does: from the game
// dir (mono_host sets TIRAMISU_GAME_DIR) or the current directory. The
// game ALSO sets the AppID itself via RestartAppIfNecessary(appid), so
// a missing file is not fatal — the bridge default (0) stands until the
// game calls RestartAppIfNecessary.
void steam_read_appid_file(void) {
  char path[1024];
  const char* dir = getenv("TIRAMISU_GAME_DIR");
  FILE* f = nullptr;
  if (dir && dir[0]) {
    snprintf(path, sizeof(path), "%s/steam_appid.txt", dir);
    f = fopen(path, "r");
  }
  if (!f) f = fopen("steam_appid.txt", "r");
  if (!f) return;
  unsigned int appid = 0;
  if (fscanf(f, "%u", &appid) == 1 && appid != 0)
    trt_steam_set_appid(appid);
  fclose(f);
}

// ---- flat SteamAPI functions ----
int Init(void) {
  trt_core_log("steam-door: Init called");
  steam_read_appid_file();
  const int r = trt_SteamAPI_Init();
  char line[96];
  std::snprintf(line, sizeof(line), "steam-door: Init -> %d", r);
  trt_core_log(line);
  return r;
}
void Shutdown(void) { trt_SteamAPI_Shutdown(); }
int RestartAppIfNecessary(unsigned int appid) {
  trt_core_log("steam-door: RestartAppIfNecessary called");
  return trt_SteamAPI_RestartAppIfNecessary(appid);
}
void RunCallbacks(void) { trt_SteamAPI_RunCallbacks(); }
int IsSteamRunning(void) {
  trt_core_log("steam-door: IsSteamRunning called");
  return trt_SteamAPI_IsSteamRunning();
}
void Steam_RunCallbacks_(int pipe, int game_server) {
  (void)game_server;  // one dispatch queue in the bridge
  trt_SteamAPI_ManualDispatch_RunFrame(pipe);
}
int Steam_GetHSteamUserCurrent_(void) {
  return trt_SteamAPI_GetHSteamUser();
}
int GetHSteamPipe_(void) { return trt_SteamAPI_GetHSteamPipe(); }
int GetHSteamUser_(void) { return trt_SteamAPI_GetHSteamUser(); }
void GameServer_RunCallbacks(void) { trt_SteamAPI_RunCallbacks(); }
void* SteamClient_(void) { return steam_client(); }

// ---- ISteamApps ----
int ISteamApps_BIsSubscribed(void) {
  return st_apps_BIsSubscribed(steam_apps());
}
int ISteamApps_BIsLowViolence(void) {
  return st_apps_BIsLowViolence(steam_apps());
}
int ISteamApps_BIsCybercafe(void) {
  return st_apps_BIsCybercafe(steam_apps());
}
int ISteamApps_BIsVACBanned(void) {
  return st_apps_BIsVACBanned(steam_apps());
}
const char* ISteamApps_GetCurrentGameLanguage(void) {
  return st_apps_GetLang(steam_apps());
}
const char* ISteamApps_GetAvailableGameLanguages(void) {
  return st_apps_GetAvailLangs(steam_apps());
}
int ISteamApps_BIsSubscribedApp(unsigned int appid) {
  return st_apps_BIsSubscribedApp(steam_apps(), appid);
}
int ISteamApps_BIsDlcInstalled(unsigned int appid) {
  return st_apps_BIsDlcInstalled(steam_apps(), appid);
}
int ISteamApps_GetCurrentBetaName(char* buf, int size) {
  return st_apps_GetCurrentBetaName(steam_apps(), buf, size);
}
unsigned int ISteamApps_GetAppInstallDir(unsigned int appid, char* folder,
                                         unsigned int cap) {
  return st_apps_GetAppInstallDir(steam_apps(), appid, folder, cap);
}
const char* ISteamApps_GetLaunchQueryParam(const char* key) {
  return st_apps_GetLaunchQueryParam(steam_apps(), key);
}
int ISteamApps_GetAppBuildId(void) {
  return st_apps_GetAppBuildId(steam_apps());
}

// ---- ISteamClient ----
// Managed (user, pipe, version) -> bridge (self, pipe, user, version).
int ISteamClient_CreateSteamPipe(void) {
  return st_client_CreateSteamPipe(steam_client());
}
int ISteamClient_BReleaseSteamPipe(int pipe) {
  return st_client_BReleaseSteamPipe(steam_client(), pipe);
}
int ISteamClient_ConnectToGlobalUser(int pipe) {
  return st_client_ConnectToGlobalUser(steam_client(), pipe);
}
int ISteamClient_CreateLocalUser(int* pipe, int acct) {
  return st_client_CreateLocalUser(steam_client(), pipe, acct);
}
void ISteamClient_ReleaseUser(int pipe, int user) {
  st_client_ReleaseUser(steam_client(), pipe, user);
}
void* ISteamClient_GetISteamUser(int user, int pipe, const char* version) {
  return st_client_GetISteamUser(steam_client(), pipe, user, version);
}
void* ISteamClient_GetISteamGameServer(int user, int pipe,
                                       const char* version) {
  return st_client_GetISteamGameServer(steam_client(), pipe, user, version);
}
void ISteamClient_SetLocalIPBinding(unsigned int addr, unsigned short port) {
  // The bridge takes a pointer to the address; the managed API passes
  // it by value.
  u32 a = addr;
  st_client_SetLocalIPBinding(steam_client(), &a, port);
}
void* ISteamClient_GetISteamFriends(int user, int pipe, const char* version) {
  return st_client_GetISteamFriends(steam_client(), pipe, user, version);
}
void* ISteamClient_GetISteamUtils(int pipe, const char* version) {
  return st_client_GetISteamUtils(steam_client(), pipe, version);
}
void* ISteamClient_GetISteamMatchmaking(int user, int pipe,
                                        const char* version) {
  return st_client_GetISteamMatchmaking(steam_client(), pipe, user, version);
}
void* ISteamClient_GetISteamMatchmakingServers(int user, int pipe,
                                               const char* version) {
  return st_client_GetISteamMatchmakingServers(steam_client(), pipe, user,
                                               version);
}
void* ISteamClient_GetISteamGenericInterface(int user, int pipe,
                                             const char* version) {
  return st_client_GetISteamGenericInterface(steam_client(), pipe, user,
                                             version);
}
void* ISteamClient_GetISteamUserStats(int user, int pipe,
                                      const char* version) {
  return st_client_GetISteamUserStats(steam_client(), pipe, user, version);
}
void* ISteamClient_GetISteamGameServerStats(int user, int pipe,
                                            const char* version) {
  return st_client_GetISteamGameServerStats(steam_client(), pipe, user,
                                            version);
}
void* ISteamClient_GetISteamApps(int user, int pipe, const char* version) {
  return st_client_GetISteamApps(steam_client(), pipe, user, version);
}
void* ISteamClient_GetISteamNetworking(int user, int pipe,
                                       const char* version) {
  return st_client_GetISteamNetworking(steam_client(), pipe, user, version);
}
void* ISteamClient_GetISteamRemoteStorage(int user, int pipe,
                                          const char* version) {
  return st_client_GetISteamRemoteStorage(steam_client(), pipe, user,
                                          version);
}
void* ISteamClient_GetISteamScreenshots(int user, int pipe,
                                        const char* version) {
  return st_client_GetISteamScreenshots(steam_client(), pipe, user, version);
}
unsigned int ISteamClient_GetIPCCallCount(void) {
  return st_client_GetIPCCallCount(steam_client());
}
void ISteamClient_SetWarningMessageHook(void* hook) {
  st_client_SetWarningMessageHook(steam_client(), hook);
}
int ISteamClient_BShutdownIfAllPipesClosed(void) {
  return st_client_BShutdownIfAllPipesClosed(steam_client());
}
void* ISteamClient_GetISteamHTTP(int user, int pipe, const char* version) {
  return st_client_GetISteamHTTP(steam_client(), pipe, user, version);
}
void* ISteamClient_GetISteamUnifiedMessages(int user, int pipe,
                                            const char* version) {
  return st_client_GetISteamUnifiedMessages(steam_client(), pipe, user,
                                            version);
}
void* ISteamClient_GetISteamController(int user, int pipe,
                                       const char* version) {
  return st_client_GetISteamController(steam_client(), pipe, user, version);
}
void* ISteamClient_GetISteamUGC(int user, int pipe, const char* version) {
  return st_client_GetISteamUGC(steam_client(), pipe, user, version);
}
void* ISteamClient_GetISteamAppList(int user, int pipe, const char* version) {
  return st_client_GetISteamAppList(steam_client(), pipe, user, version);
}
void* ISteamClient_GetISteamMusic(int user, int pipe, const char* version) {
  return st_client_GetISteamMusic(steam_client(), pipe, user, version);
}

// ---- ISteamController (old Steamworks.NET names for the input API) ----
int ISteamController_Init(void) {
  // bExplicitlyCallRunFrame=false: the bridge runs its own frame pump.
  return st_input_Init(steam_input(), 0);
}
int ISteamController_Shutdown(void) {
  return st_input_Shutdown(steam_input());
}
void ISteamController_RunFrame(void) {
  st_input_RunFrame(steam_input(), 0);
}
int ISteamController_GetConnectedControllers(u64* handles) {
  return st_input_GetConnectedControllers(steam_input(), handles);
}
int ISteamController_GetGamepadIndexForController(u64 handle) {
  return st_input_GetGamepadIndexForController(steam_input(), handle);
}
u64 ISteamController_GetControllerForGamepadIndex(int index) {
  return st_input_GetControllerForGamepadIndex(steam_input(), index);
}
void ISteamController_TriggerVibration(u64 handle, unsigned short left,
                                       unsigned short right) {
  st_input_TriggerVibration(steam_input(), handle, left, right);
}

// Struct-by-value returns. SysV ABI: a 2-byte struct returns in RAX
// (int 0 = zeroed), a 16-byte struct returns in RAX:RDX, a 40-byte
// struct returns via a hidden sret pointer as the first argument.
// There is no action backend in the bridge (no action sets are ever
// loaded), so the honest answer is "no action data" — zeroed — with a
// loud one-time log, not fabricated input.
struct controller_digital_action_data {
  unsigned char bState;
  unsigned char bActive;
};
struct controller_analog_action_data {
  int eMode;
  float x;
  float y;
  unsigned char bActive;
  unsigned char _pad[3];
};
static_assert(sizeof(controller_digital_action_data) == 2,
              "ControllerDigitalActionData_t must be 2 bytes");
static_assert(sizeof(controller_analog_action_data) == 16,
              "ControllerAnalogActionData_t must be 16 bytes");

controller_digital_action_data ISteamController_GetDigitalActionData(
    u64 handle, u64 action) {
  (void)handle;
  (void)action;
  static int warned;
  if (!warned) {
    warned = 1;
    fprintf(stderr,
            "[trt-steam] ISteamController_GetDigitalActionData: no action "
            "backend — returning zeroed data\n");
  }
  controller_digital_action_data out{};
  return out;
}
controller_analog_action_data ISteamController_GetAnalogActionData(
    u64 handle, u64 action) {
  (void)handle;
  (void)action;
  static int warned;
  if (!warned) {
    warned = 1;
    fprintf(stderr,
            "[trt-steam] ISteamController_GetAnalogActionData: no action "
            "backend — returning zeroed data\n");
  }
  controller_analog_action_data out{};
  return out;
}
// 40-byte struct: SysV passes a hidden sret pointer as arg 0. Mono
// marshals it the same way.
struct controller_motion_data {
  float rotQuatX, rotQuatY, rotQuatZ, rotQuatW;
  float posAccelX, posAccelY, posAccelZ;
  float rotVelX, rotVelY, rotVelZ;
};
static_assert(sizeof(controller_motion_data) == 40,
              "ControllerMotionData_t must be 40 bytes");

controller_motion_data ISteamController_GetMotionData(u64 handle) {
  (void)handle;
  static int warned;
  if (!warned) {
    warned = 1;
    fprintf(stderr,
            "[trt-steam] ISteamController_GetMotionData: no action backend "
            "— returning zeroed data\n");
  }
  controller_motion_data out{};
  return out;
}

// ---- ISteamFriends ----
const char* ISteamFriends_GetPersonaName(void) {
  return st_friends_GetPersonaName(steam_friends());
}
int ISteamFriends_GetPersonaState(void) {
  return st_friends_GetPersonaState(steam_friends());
}
const char* ISteamFriends_GetFriendPersonaName(u64 steam_id) {
  return st_friends_GetFriendPersonaName(steam_friends(), steam_id);
}
const char* ISteamFriends_GetFriendPersonaNameHistory(u64 steam_id, int index) {
  return st_friends_GetFriendPersonaNameHistory(steam_friends(), steam_id,
                                                index);
}
const char* ISteamFriends_GetPlayerNickname(u64 steam_id) {
  return st_friends_GetPlayerNickname(steam_friends(), steam_id);
}
const char* ISteamFriends_GetFriendsGroupName(unsigned short group_id) {
  return st_friends_GetFriendsGroupName(steam_friends(), group_id);
}
const char* ISteamFriends_GetClanName(u64 steam_id) {
  return st_friends_GetClanName(steam_friends(), steam_id);
}
const char* ISteamFriends_GetClanTag(u64 steam_id) {
  return st_friends_GetClanTag(steam_friends(), steam_id);
}
const char* ISteamFriends_GetFriendRichPresence(u64 steam_id,
                                                const char* key) {
  return st_friends_GetFriendRichPresence(steam_friends(), steam_id, key);
}
const char* ISteamFriends_GetFriendRichPresenceKeyByIndex(u64 steam_id,
                                                          int index) {
  return st_friends_GetFriendRichPresenceKeyByIndex(steam_friends(), steam_id,
                                                    index);
}

// ---- ISteamRemoteStorage (real local cloud-file store in the bridge) ----
int ISteamRemoteStorage_FileWrite(const char* name, const void* data,
                                  int len) {
  return st_rs_FileWrite(steam_rs(), name, data, len);
}
int ISteamRemoteStorage_FileRead(const char* name, void* data, int len) {
  return st_rs_FileRead(steam_rs(), name, data, len);
}
int ISteamRemoteStorage_FileForget(const char* name) {
  return st_rs_FileForget(steam_rs(), name);
}
int ISteamRemoteStorage_FileDelete(const char* name) {
  return st_rs_FileDelete(steam_rs(), name);
}
int ISteamRemoteStorage_FileExists(const char* name) {
  return st_rs_FileExists(steam_rs(), name);
}
int ISteamRemoteStorage_FilePersisted(const char* name) {
  return st_rs_FilePersisted(steam_rs(), name);
}
int ISteamRemoteStorage_GetFileSize(const char* name) {
  return st_rs_GetFileSize(steam_rs(), name);
}
long long ISteamRemoteStorage_GetFileTimestamp(const char* name) {
  return st_rs_GetFileTimestamp(steam_rs(), name);
}
int ISteamRemoteStorage_GetFileCount(void) {
  return st_rs_GetFileCount(steam_rs());
}
const char* ISteamRemoteStorage_GetFileNameAndSize(int index, int* size) {
  return st_rs_GetFileNameAndSize(steam_rs(), index, size);
}
int ISteamRemoteStorage_GetQuota(u64* total, u64* avail) {
  return st_rs_GetQuota(steam_rs(), total, avail);
}
int ISteamRemoteStorage_IsCloudEnabledForAccount(void) {
  return st_rs_IsCloudEnabledForAccount(steam_rs());
}
int ISteamRemoteStorage_IsCloudEnabledForApp(void) {
  return st_rs_IsCloudEnabledForApp(steam_rs());
}
void ISteamRemoteStorage_SetCloudEnabledForApp(int enabled) {
  st_rs_SetCloudEnabledForApp(steam_rs(), enabled);
}

// ---- ISteamUser ----
int ISteamUser_BLoggedOn(void) { return st_user_BLoggedOn(steam_user()); }
u64 ISteamUser_GetSteamID(void) {
  // The managed signature returns a plain u64 in RAX. The bridge's
  // vtable function ALSO writes the hidden sret slot and returns the id
  // in RAX; call it directly and return the value.
  u64 tmp = 0;
  return st_user_GetSteamID(steam_user(), &tmp);
}
int ISteamUser_GetUserDataFolder(char* buf, int size) {
  return st_user_GetUserDataFolder(steam_user(), buf, size);
}

// ---- ISteamUserStats (string getters only; the stats backend does not
// exist, so RequestCurrentStats etc. are fail-visible false) ----
const char* ISteamUserStats_GetAchievementDisplayAttribute(const char* ach,
                                                           const char* key) {
  return st_userstats_GetAchievementDisplayAttribute(steam_userstats(), ach,
                                                     key);
}
const char* ISteamUserStats_GetAchievementName(unsigned int index) {
  return st_userstats_GetAchievementName(steam_userstats(), index);
}
const char* ISteamUserStats_GetLeaderboardName(u64 leaderboard) {
  return st_userstats_GetLeaderboardName(steam_userstats(), leaderboard);
}

// ---- ISteamUtils ----
unsigned int ISteamUtils_GetSecondsSinceAppActive(void) {
  return st_utils_GetSecondsSinceAppActive(steam_utils());
}
unsigned int ISteamUtils_GetSecondsSinceComputerActive(void) {
  return st_utils_GetSecondsSinceComputerActive(steam_utils());
}
int ISteamUtils_GetConnectedUniverse(void) {
  return st_utils_GetConnectedUniverse(steam_utils());
}
unsigned int ISteamUtils_GetServerRealTime(void) {
  return st_utils_GetServerRealTime(steam_utils());
}
const char* ISteamUtils_GetIPCountry(void) {
  return st_utils_GetIPCountry(steam_utils());
}
unsigned char ISteamUtils_GetCurrentBatteryPower(void) {
  return st_utils_GetCurrentBatteryPower(steam_utils());
}
unsigned int ISteamUtils_GetAppID(void) {
  return st_utils_GetAppID(steam_utils());
}
const char* ISteamUtils_GetSteamUILanguage(void) {
  return st_utils_GetSteamUILanguage(steam_utils());
}

// ---- fail-visible: everything the bridge does not implement ----
#include "gen/gen_steam_failvisible.inc"

}  // extern "C"
