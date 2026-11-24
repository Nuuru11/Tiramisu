// tiramisu/src/trt/bridge_steam.h
// Steamworks auth bridge for the guest (bridge_steam.cpp).
// AUTH ONLY: no DRM bypass, ever. A DRM'd build refuses visibly.
//
// The flat trt_SteamAPI_* functions and the st_* interface functions are
// declared here so the Mono dllmap door (third_party/mono-bridges) can call
// the SAME implementation the 64-bit steam_api64 bridge uses: one Steam
// client, two front doors. All bridge functions are TRT_GUEST_ABI (ms_abi
// on Linux); the Mono door wraps them in SysV functions.
#ifndef TIRAMISU_BRIDGE_STEAM_H
#define TIRAMISU_BRIDGE_STEAM_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// A Steam interface object: one vtable pointer, like a COM object.
// The door hands these pointers to managed code as ISteam* handles.
struct fake_steam_iface {
  const void** vtbl;
};

extern struct fake_steam_iface g_steam_client_obj;
extern struct fake_steam_iface g_steam_user_obj;
extern struct fake_steam_iface g_steam_apps_obj;
extern struct fake_steam_iface g_steam_utils_obj;
extern struct fake_steam_iface g_steam_friends_obj;
extern struct fake_steam_iface g_steam_userstats_obj;
extern struct fake_steam_iface g_steam_matchmaking_obj;
extern struct fake_steam_iface g_steam_networking_obj;
extern struct fake_steam_iface g_steam_rs_obj;
extern struct fake_steam_iface g_steam_input_obj;

// Host <-> Bridge Friends & Achievement observation (for in-game overlay & UI)
struct trt_steam_friend_info {
  u64 steam_id;
  char persona[64];
  int state;           // 0=Offline, 1=Online, 3=Away, 6=In-Game
  char game_name[64];  // non-empty when playing
};

struct trt_steam_achievement_info {
  char name[128];
  bool achieved;
  u32 unlock_time;
  u32 cur_progress;
  u32 max_progress;
};

void trt_steam_set_friends(const trt_steam_friend_info* list, int count);
int  trt_steam_get_friend_count(void);
int  trt_steam_get_friend_at(int idx, trt_steam_friend_info* out);
const char* trt_steam_current_persona(void);
int  trt_steam_get_achievement_count(void);
int  trt_steam_get_achievement_at(int idx, trt_steam_achievement_info* out);

int trt_bridge_register_steam(void);

// Harness config (SysV, not guest ABI): the AppID the bridge reports.
// The Mono door sets this from steam_appid.txt (or the test harness).
void trt_steam_set_appid(u32 app_id);

// ---- flat SteamAPI ----
// NOTE: trt_SteamAPI_Init/InitEx/InitFlat take no explicit app id; the
// AppID comes from trt_steam_set_appid / RestartAppIfNecessary.
int TRT_GUEST_ABI trt_SteamAPI_Init(void);
int TRT_GUEST_ABI trt_SteamAPI_InitEx(void* ctx);
int TRT_GUEST_ABI trt_SteamAPI_InitFlat(char* out_err_msg);
void TRT_GUEST_ABI trt_SteamAPI_Shutdown(void);
int TRT_GUEST_ABI trt_SteamAPI_RestartAppIfNecessary(u32 app_id);
int TRT_GUEST_ABI trt_SteamAPI_IsSteamRunning(void);
u64 TRT_GUEST_ABI trt_SteamAPI_GetSteamID(void);
int TRT_GUEST_ABI trt_SteamAPI_ActivateGameOverlay(const char* url);
int trt_steam_overlay_check_and_reset(void);
void trt_steam_trigger_overlay(void);
int TRT_GUEST_ABI trt_SteamAPI_SetRichPresence(const char* key,
                                              const char* value);
void TRT_GUEST_ABI trt_SteamAPI_RunCallbacks(void);
void TRT_GUEST_ABI trt_SteamAPI_RegisterCallback(void* callback, int id);
void TRT_GUEST_ABI trt_SteamAPI_UnregisterCallback(void* callback);
int TRT_GUEST_ABI trt_SteamAPI_BIsSubscribedApp(u32 app_id);
int TRT_GUEST_ABI trt_SteamAPI_BIsDlcInstalled(u32 dlc_id);
const char* TRT_GUEST_ABI trt_SteamAPI_GetCurrentGameLanguage(void);
int TRT_GUEST_ABI trt_SteamAPI_GetHSteamUser(void);
int TRT_GUEST_ABI trt_SteamAPI_GetHSteamPipe(void);
void TRT_GUEST_ABI trt_SteamAPI_RegisterCallResult(void* cb, u64 call_id);
void TRT_GUEST_ABI trt_SteamAPI_UnregisterCallResult(void* cb, u64 call_id);
void TRT_GUEST_ABI trt_SteamAPI_ManualDispatch_Init(void);
void TRT_GUEST_ABI trt_SteamAPI_ManualDispatch_RunFrame(int pipe);
int TRT_GUEST_ABI trt_SteamAPI_ManualDispatch_GetNextCallback(int pipe,
                                                             void* msg);
void TRT_GUEST_ABI trt_SteamAPI_ManualDispatch_FreeLastCallback(int pipe);
void* TRT_GUEST_ABI trt_SteamAPI_SteamUser_v021(void);
void* TRT_GUEST_ABI trt_SteamAPI_SteamUser_v023(void);
void* TRT_GUEST_ABI trt_SteamAPI_SteamApps_v008(void);

// Registration-table observability (host side only; no guest export).
int trt_steam_callresult_count(void);
int trt_steam_callback_count(void);
void* TRT_GUEST_ABI trt_SteamAPI_SteamUtils_v010(void);
int TRT_GUEST_ABI trt_SteamAPI_ISteamApps_BIsSubscribedApp(void*, u32 app_id);
int TRT_GUEST_ABI trt_SteamAPI_ISteamApps_BIsDlcInstalled(void*, u32 dlc_id);
const char* TRT_GUEST_ABI trt_SteamAPI_ISteamApps_GetCurrentGameLanguage(
    void*);
u32 TRT_GUEST_ABI trt_SteamAPI_ISteamUtils_GetAppID(void*);

// ---- SteamInternal interface factory ----
void* TRT_GUEST_ABI trt_SteamInternal_CreateInterface(const char* version);
void* TRT_GUEST_ABI trt_SteamInternal_FindOrCreateUserInterface(int user,
                                                                const char* ver);
void* TRT_GUEST_ABI trt_SteamInternal_FindOrCreateGameServerInterface(
    int user, const char* ver);
void* TRT_GUEST_ABI trt_SteamInternal_ContextInit(void* ctx_init_data);

// ---- ISteamApps ----
int TRT_GUEST_ABI st_apps_BIsSubscribed(void*);
int TRT_GUEST_ABI st_apps_BIsLowViolence(void*);
int TRT_GUEST_ABI st_apps_BIsCybercafe(void*);
int TRT_GUEST_ABI st_apps_BIsVACBanned(void*);
const char* TRT_GUEST_ABI st_apps_GetLang(void*);
const char* TRT_GUEST_ABI st_apps_GetAvailLangs(void*);
int TRT_GUEST_ABI st_apps_BIsSubscribedApp(void*, u32 app_id);
int TRT_GUEST_ABI st_apps_BIsDlcInstalled(void*, u32 dlc_id);
u32 TRT_GUEST_ABI st_apps_GetAppInstallDir(void*, u32 app_id, char* pchFolder,
                                          u32 cap);
const char* TRT_GUEST_ABI st_apps_GetLaunchQueryParam(void*, const char* key);
int TRT_GUEST_ABI st_apps_GetAppBuildId(void*);
int TRT_GUEST_ABI st_apps_GetCurrentBetaName(void*, char* pchName,
                                            int cchNameBuffer);

// ---- ISteamUser ----
// GetSteamID takes the hidden sret pointer: CSteamID has constructors, so
// MSVC returns it via a hidden first pointer (the guest ABI). The door
// calls st_user_GetSteamID(self, &tmp) and returns tmp in RAX.
int TRT_GUEST_ABI st_user_GetHSteamUser(void*);
int TRT_GUEST_ABI st_user_BLoggedOn(void*);
u64 TRT_GUEST_ABI st_user_GetSteamID(void*, u64* hidden_sret);
int TRT_GUEST_ABI st_user_GetUserDataFolder(void* self, char* buf, int size);

// ---- ISteamUtils ----
u32 TRT_GUEST_ABI st_utils_GetSecondsSinceAppActive(void*);
u32 TRT_GUEST_ABI st_utils_GetSecondsSinceComputerActive(void*);
int TRT_GUEST_ABI st_utils_GetConnectedUniverse(void*);
u32 TRT_GUEST_ABI st_utils_GetServerRealTime(void*);
const char* TRT_GUEST_ABI st_utils_GetIPCountry(void*);
unsigned char TRT_GUEST_ABI st_utils_GetCurrentBatteryPower(void*);
u32 TRT_GUEST_ABI st_utils_GetAppID(void*);
const char* TRT_GUEST_ABI st_utils_GetSteamUILanguage(void*);

// ---- ISteamFriends ----
const char* TRT_GUEST_ABI st_friends_GetPersonaName(void*);
int TRT_GUEST_ABI st_friends_GetPersonaState(void*);
const char* TRT_GUEST_ABI st_friends_GetFriendPersonaName(void* self,
                                                         u64 steam_id);
const char* TRT_GUEST_ABI st_friends_GetFriendPersonaNameHistory(void* self,
                                                                u64 steam_id,
                                                                int index);
const char* TRT_GUEST_ABI st_friends_GetPlayerNickname(void* self,
                                                      u64 steam_id);
const char* TRT_GUEST_ABI st_friends_GetFriendsGroupName(void* self,
                                                        u16 group_id);
const char* TRT_GUEST_ABI st_friends_GetClanName(void* self, u64 steam_id);
const char* TRT_GUEST_ABI st_friends_GetClanTag(void* self, u64 steam_id);
const char* TRT_GUEST_ABI st_friends_GetFriendRichPresence(void* self,
                                                          u64 steam_id,
                                                          const char* key);
const char* TRT_GUEST_ABI st_friends_GetFriendRichPresenceKeyByIndex(
    void* self, u64 steam_id, int index);

// ---- ISteamUserStats (only the string getters are real; Request* /
// GetStat / SetStat / StoreStats have NO backend — the door returns safe
// failure for those rather than calling the NULL vtable slots) ----
const char* TRT_GUEST_ABI st_userstats_GetAchievementDisplayAttribute(
    void* self, const char* name, const char* key);
const char* TRT_GUEST_ABI st_userstats_GetAchievementName(void* self,
                                                         u32 index);
const char* TRT_GUEST_ABI st_userstats_GetLeaderboardName(void* self,
                                                         u64 leaderboard);

// ---- ISteamRemoteStorage (real local cloud-file store under the VFS) ----
int TRT_GUEST_ABI st_rs_FileWrite(void*, const char* pchFile,
                                 const void* pvData, i32 cubData);
i32 TRT_GUEST_ABI st_rs_FileRead(void*, const char* pchFile, void* pvData,
                                i32 cubDataToRead);
int TRT_GUEST_ABI st_rs_FileForget(void*, const char* pchFile);
int TRT_GUEST_ABI st_rs_FileDelete(void*, const char* pchFile);
int TRT_GUEST_ABI st_rs_FileExists(void*, const char* pchFile);
int TRT_GUEST_ABI st_rs_FilePersisted(void* self, const char* pchFile);
i32 TRT_GUEST_ABI st_rs_GetFileSize(void*, const char* pchFile);
i64 TRT_GUEST_ABI st_rs_GetFileTimestamp(void* self, const char* pchFile);
i32 TRT_GUEST_ABI st_rs_GetFileCount(void*);
const char* TRT_GUEST_ABI st_rs_GetFileNameAndSize(void*, int iFile,
                                                  int* pnSize);
int TRT_GUEST_ABI st_rs_GetQuota(void*, u64* total, u64* avail);
int TRT_GUEST_ABI st_rs_IsCloudEnabledForAccount(void*);
int TRT_GUEST_ABI st_rs_IsCloudEnabledForApp(void*);
void TRT_GUEST_ABI st_rs_SetCloudEnabledForApp(void*, int enabled);

// ---- ISteamInput ----
int TRT_GUEST_ABI st_input_Init(void*, int bExplicitlyCallRunFrame);
int TRT_GUEST_ABI st_input_Shutdown(void*);
void TRT_GUEST_ABI st_input_RunFrame(void*, int bReserved);
int TRT_GUEST_ABI st_input_GetConnectedControllers(void*, u64* handles_out);
u64 TRT_GUEST_ABI st_input_GetControllerForGamepadIndex(void*, int index);
int TRT_GUEST_ABI st_input_GetGamepadIndexForController(void*, u64 handle);
int TRT_GUEST_ABI st_input_GetInputTypeForHandle(void*, u64 handle);
void TRT_GUEST_ABI st_input_TriggerVibration(void*, u64 handle,
                                            u16 left_speed, u16 right_speed);
void TRT_GUEST_ABI st_input_TriggerVibrationExtended(void* self, u64 handle,
                                                     u16 left, u16 right,
                                                     u16 lt, u16 rt);
const char* TRT_GUEST_ABI st_input_GetStringForDigitalActionName(void* self,
                                                                u64 action_handle);
const char* TRT_GUEST_ABI st_input_GetStringForAnalogActionName(void* self,
                                                               u64 action_handle);

// ---- ISteamClient ----
int TRT_GUEST_ABI st_client_CreateSteamPipe(void*);
int TRT_GUEST_ABI st_client_BReleaseSteamPipe(void*, int pipe);
int TRT_GUEST_ABI st_client_ConnectToGlobalUser(void*, int pipe);
int TRT_GUEST_ABI st_client_CreateLocalUser(void*, int* pipe, int acct);
void TRT_GUEST_ABI st_client_ReleaseUser(void*, int pipe, int user);
void* TRT_GUEST_ABI st_client_GetISteamUser(void*, int pipe, int user,
                                           const char* ver);
void* TRT_GUEST_ABI st_client_GetISteamGameServer(void*, int pipe, int user,
                                                 const char* ver);
void TRT_GUEST_ABI st_client_SetLocalIPBinding(void*, const void* addr,
                                              u16 port);
void* TRT_GUEST_ABI st_client_GetISteamFriends(void*, int pipe, int user,
                                              const char* ver);
void* TRT_GUEST_ABI st_client_GetISteamUtils(void*, int pipe, const char* ver);
void* TRT_GUEST_ABI st_client_GetISteamMatchmaking(void*, int pipe, int user,
                                                  const char* ver);
void* TRT_GUEST_ABI st_client_GetISteamMatchmakingServers(void*, int pipe,
                                                         int user,
                                                         const char* ver);
void* TRT_GUEST_ABI st_client_GetISteamGenericInterface(void*, int pipe,
                                                       int user,
                                                       const char* ver);
void* TRT_GUEST_ABI st_client_GetISteamUserStats(void*, int pipe, int user,
                                                const char* ver);
void* TRT_GUEST_ABI st_client_GetISteamGameServerStats(void*, int pipe,
                                                      int user,
                                                      const char* ver);
void* TRT_GUEST_ABI st_client_GetISteamApps(void*, int pipe, int user,
                                           const char* ver);
void* TRT_GUEST_ABI st_client_GetISteamNetworking(void*, int pipe, int user,
                                                 const char* ver);
void* TRT_GUEST_ABI st_client_GetISteamRemoteStorage(void*, int pipe, int user,
                                                    const char* ver);
void* TRT_GUEST_ABI st_client_GetISteamScreenshots(void*, int pipe, int user,
                                                  const char* ver);
void* TRT_GUEST_ABI st_client_GetISteamGameSearch(void*, int pipe, int user,
                                                 const char* ver);
void TRT_GUEST_ABI st_client_RunFrame(void*);
u32 TRT_GUEST_ABI st_client_GetIPCCallCount(void*);
void TRT_GUEST_ABI st_client_SetWarningMessageHook(void*, void* hook);
int TRT_GUEST_ABI st_client_BShutdownIfAllPipesClosed(void*);
void* TRT_GUEST_ABI st_client_GetISteamHTTP(void*, int pipe, int user,
                                           const char* ver);
void* TRT_GUEST_ABI st_client_GetISteamUnifiedMessages(void*, int pipe,
                                                      int user,
                                                      const char* ver);
void* TRT_GUEST_ABI st_client_GetISteamController(void*, int pipe, int user,
                                                 const char* ver);
void* TRT_GUEST_ABI st_client_GetISteamUGC(void*, int pipe, int user,
                                          const char* ver);
void* TRT_GUEST_ABI st_client_GetISteamAppList(void*, int pipe, int user,
                                              const char* ver);
void* TRT_GUEST_ABI st_client_GetISteamMusic(void*, int pipe, int user,
                                            const char* ver);

#ifdef __cplusplus
}
#endif

#endif  // TIRAMISU_BRIDGE_STEAM_H
