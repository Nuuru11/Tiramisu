// tiramisu/src/guest/guest_steam.c
// The Steamworks path a game uses at boot:
//   SteamAPI_RestartAppIfNecessary -> SteamAPI_Init -> GetSteamID ->
//   SetRichPresence -> RunCallbacks -> Shutdown.
// A non-DRM'd build boots; a DRM'd build refuses later (its OWN check).
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef unsigned long long u64;
typedef int i32;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

__attribute__((dllimport)) i32 GSTD SteamAPI_RestartAppIfNecessary(u32 appid);
__attribute__((dllimport)) i32 GSTD SteamAPI_Init(void);
__attribute__((dllimport)) void GSTD SteamAPI_Shutdown(void);
__attribute__((dllimport)) int GSTD SteamAPI_IsSteamRunning(void);
__attribute__((dllimport)) u64 GSTD SteamAPI_GetSteamID(void);
__attribute__((dllimport)) i32 GSTD SteamAPI_SetRichPresence(const char* key,
                                                             const char* val);
__attribute__((dllimport)) void GSTD SteamAPI_RunCallbacks(void);

u32 GSTD steam_boot(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  // 1. the app-id gate (DRM'd builds call this first; we answer "no restart")
  if (SteamAPI_RestartAppIfNecessary(480) == 0) bits |= 0x1u;

  // 2. init
  if (SteamAPI_Init()) bits |= 0x2u;

  // 3. a user id in the valid format
  const u64 id = SteamAPI_GetSteamID();
  bits |= ((id >> 52) == 0x011ull) ? 0x4u : 0u;

  // 4. running + presence + callbacks
  bits |= SteamAPI_IsSteamRunning() ? 0x8u : 0u;
  bits |= SteamAPI_SetRichPresence("status", "Playing") ? 0x10u : 0u;
  SteamAPI_RunCallbacks();
  bits |= 0x20u;

  // 5. shutdown is clean
  SteamAPI_Shutdown();
  bits |= 0x40u;

  return bits;
}
