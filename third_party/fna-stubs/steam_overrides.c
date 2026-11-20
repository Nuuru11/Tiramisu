// Hand-tuned Steamworks stubs (spike parity).
//
// These are the functions whose return values gate boot progress, whose
// strings the managed wrapper reads, or whose struct-by-value returns a
// generated stub cannot express. Every other Steamworks P/Invoke gets a
// generated default stub (bool true, fake non-NULL pointer, numeric 0)
// from gen-stubs.py. There is no Steam here: no auth, no callbacks, no
// network, no DRM interaction of any kind — Init simply reports success so
// the game proceeds, exactly as the Path B spike did. The next step
// replaces these with real dllmaps to Tiramisu's 64-bit Steam bridge.
//
// Signatures are positional matches of the managed P/Invoke declarations
// in steamworks.implmap (monodis --implmap of Steamworks.NET.dll).
#include <string.h>

// The fake player identity. 76561197960265729 is the long-standing
// placeholder SteamID used across Tiramisu's offline profiles.
#define FAKE_STEAM_ID 76561197960265729ULL
#define CELESTE_APPID 504230u

static const char stub_english[] = "english";
static const char stub_player[] = "Player";
static const char stub_us[] = "US";
static const char stub_empty[] = "";

// --- boot gates: BOTH must be true or Celeste aborts 'Steam not found!' ---
int Init(void) { return 1; }            // SteamAPI_Init
int IsSteamRunning(void) { return 1; }  // SteamAPI_IsSteamRunning

// --- "true means exit" gates: these MUST be false ---
// RestartAppIfNecessary returning true tells the game "Steam is relaunching
// you, exit now" — Celeste.Main returns silently and the game never boots.
// The generated bool default (true) is exactly wrong here.
int RestartAppIfNecessary(void) { return 0; }  // no restart needed
int ISteamGameServer_WasRestartRequested(void) { return 0; }
// No cloud files exist, so nothing is there to read back.
int ISteamRemoteStorage_FileExists(void) { return 0; }
int ISteamRemoteStorage_FilePersisted(void) { return 0; }

// --- handles: 0 reads as invalid, so hand out 1 ---
int GetHSteamPipe_(void) { return 1; }
int GetHSteamUser_(void) { return 1; }
int Steam_GetHSteamUserCurrent_(void) { return 1; }
int GameServer_GetHSteamPipe(void) { return 1; }
int GameServer_GetHSteamUser(void) { return 1; }
int ISteamClient_CreateSteamPipe(void) { return 1; }
int ISteamClient_ConnectToGlobalUser(void *pipe) { (void)pipe; return 1; }
int ISteamClient_CreateLocalUser(int *pipe, int account_type) {
  (void)account_type;
  *pipe = 1;
  return 1;
}

// --- identity ---
unsigned long long ISteamUser_GetSteamID(void) { return FAKE_STEAM_ID; }
unsigned long long ISteamGameServer_GetSteamID(void) { return FAKE_STEAM_ID; }
unsigned long long GameServer_GetSteamID(void) { return FAKE_STEAM_ID; }
unsigned int ISteamUtils_GetAppID(void) { return CELESTE_APPID; }
unsigned int ISteamGameServerUtils_GetAppID(void) { return CELESTE_APPID; }

// --- strings the wrapper reads (const char* returns) ---
const char *ISteamApps_GetCurrentGameLanguage(void) { return stub_english; }
const char *ISteamApps_GetAvailableGameLanguages(void) { return stub_english; }
const char *ISteamApps_GetLaunchQueryParam(void) { return stub_empty; }
const char *ISteamUtils_GetSteamUILanguage(void) { return stub_english; }
const char *ISteamGameServerUtils_GetSteamUILanguage(void) { return stub_english; }
const char *ISteamUtils_GetIPCountry(void) { return stub_us; }
const char *ISteamGameServerUtils_GetIPCountry(void) { return stub_us; }
const char *ISteamFriends_GetPersonaName(void) { return stub_player; }
const char *ISteamFriends_GetFriendPersonaName(void) { return stub_player; }
const char *ISteamFriends_GetPlayerNickname(void) { return stub_player; }
const char *ISteamFriends_GetClanName(void) { return stub_empty; }
const char *ISteamFriends_GetClanTag(void) { return stub_empty; }
const char *ISteamFriends_GetFriendPersonaNameHistory(void) { return stub_empty; }
const char *ISteamFriends_GetFriendRichPresence(void) { return stub_empty; }
const char *ISteamFriends_GetFriendRichPresenceKeyByIndex(void) { return stub_empty; }
const char *ISteamFriends_GetFriendsGroupName(void) { return stub_empty; }
const char *ISteamController_GetGlyphForActionOrigin(void) { return stub_empty; }
const char *ISteamController_GetStringForActionOrigin(void) { return stub_empty; }
const char *ISteamMatchmaking_GetLobbyData(void) { return stub_empty; }
const char *ISteamMatchmaking_GetLobbyMemberData(void) { return stub_empty; }
const char *ISteamUserStats_GetAchievementDisplayAttribute(void) { return stub_empty; }
const char *ISteamUserStats_GetAchievementName(void) { return stub_empty; }
const char *ISteamUserStats_GetLeaderboardName(void) { return stub_empty; }

// bool GetUserDataFolder(char* buffer, int size): write a relative path and
// report success. A Windows absolute path would be meaningless on Linux;
// the game never uses this path for saves (it uses LocalApplicationData).
int ISteamUser_GetUserDataFolder(char *buffer, int size) {
  static const char path[] = "Celeste";
  if (buffer != NULL && size > 0) {
    int n = (int)sizeof(path) < size ? (int)sizeof(path) : size;
    memcpy(buffer, path, (size_t)n);
    buffer[n - 1] = 0;
  }
  return 1;
}

// --- enums / scalars where the generated 0 is the wrong answer ---
int ISteamFriends_GetPersonaState(void) { return 1; }  // EPersonaState::Online
float ISteamMusic_GetVolume(void) { return 1.0f; }
int ISteamUtils_GetConnectedUniverse(void) { return 1; }  // EUniverse::Public
int ISteamGameServerUtils_GetConnectedUniverse(void) { return 1; }
int ISteamUtils_GetCurrentBatteryPower(void) { return 255; }  // AC / unknown
int ISteamGameServerUtils_GetCurrentBatteryPower(void) { return 255; }

// --- encrypted app ticket ---
int SteamEncryptedAppTicket_BUserIsVacBanned(void) { return 0; }  // NOT banned
unsigned int SteamEncryptedAppTicket_GetTicketAppID(void) { return CELESTE_APPID; }
void SteamEncryptedAppTicket_GetTicketSteamID(void *ticket, unsigned int size,
                                             unsigned long long *steamid) {
  (void)ticket; (void)size;
  *steamid = FAKE_STEAM_ID;
}

// --- struct-by-value returns (SysV ABI; a generated int stub would be wrong) ---
// ControllerDigitalActionData_t is 2 bytes ({bool bState; bool bActive}):
// returned in RAX, so int 0 is the correct zero value.
int ISteamController_GetDigitalActionData(void) { return 0; }

// ControllerAnalogActionData_t is 16 bytes (mode + x + y + active, padded):
// returned in RAX:RDX. A struct of two longs has the same ABI class.
struct analog16 { long long a, b; };
struct analog16 ISteamController_GetAnalogActionData(void) {
  struct analog16 r = {0, 0};
  return r;
}

// ControllerMotionData_t is 40 bytes (10 floats): returned via a hidden
// sret pointer passed as the first argument. Zero the caller's buffer.
void ISteamController_GetMotionData(void *ret) {
  memset(ret, 0, 40);
}
