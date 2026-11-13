// tiramisu/src/trt/bridge_winmm.cpp
// winmm.dll for the guest — the multimedia clock.
//
// Nearly every game pulls its frame timing from winmm!timeGetTime (even
// XAudio2-era engines keep it as the scheduler clock), and startup often
// calls timeBeginPeriod to raise the timer resolution. Without this bridge
// those calls hit a fail-visible STUB and the game either spins at 100%
// waiting on a clock that never moves or aborts with a named error.
//
// waveOut*/midiOut* answer "no legacy devices" (0 devices) instead of
// failing: engines probe them and fall back to XAudio2, which is the real
// path this runtime serves.
#include "bridge_winmm.h"
#include "trt.h"
#include "thunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace {

// timeGetTime: ms since boot, wrapping at 2^32 like Windows.
u32 now_ms() {
  struct timespec ts;
  timespec_get(&ts, TIME_UTC);  // portable across Windows/UWP and Linux
  return (u32)((u64)ts.tv_sec * 1000 + (u64)ts.tv_nsec / 1000000);
}

// the requested period (ms), for visibility in logs
u32 g_min_period = 0;

} // namespace

extern "C" {

// ---- timers ----

u32 TRT_GUEST_ABI trt_timeGetTime(void) { return now_ms(); }

u32 TRT_GUEST_ABI trt_timeBeginPeriod(u32 period_ms) {
  if (period_ms && (g_min_period == 0 || period_ms < g_min_period))
    g_min_period = period_ms;
  // TIMERR_NOERROR
  return 0;
}

u32 TRT_GUEST_ABI trt_timeEndPeriod(u32) { return 0; }

u32 TRT_GUEST_ABI trt_timeGetDevCaps(void* caps, u32 size) {
  if (!caps || size < 8) return 2 /* TIMERR_STRUCT */;
  u8* p = (u8*)caps;
  memset(p, 0, size);
  // TIMECAPS { UINT wPeriodMin; UINT wPeriodMax; }
  *(u32*)(p + 0) = 1;      // wPeriodMin
  *(u32*)(p + 4) = 1000;   // wPeriodMax
  return 0;
}

// ---- legacy audio device probes: "no devices", never a crash ----

u32 TRT_GUEST_ABI trt_waveOutGetNumDevs(void) { return 0; }

u32 TRT_GUEST_ABI trt_waveOutGetDevCapsW(u64, void*, u32) { return 4 /* MMSYSERR_BADDEVICEID */; }

u32 TRT_GUEST_ABI trt_midiOutGetNumDevs(void) { return 0; }

u32 TRT_GUEST_ABI trt_joyGetNumDevs(void) { return 0; }

u32 TRT_GUEST_ABI trt_joyGetPosEx(u32, void*) { return 167 /* JOYERR_UNPLUGGED */; }

// ---- mciSendStringW: engines probe codecs with it; answer "cannot find
// the device" (MCIERR_INVALID_DEVICE_NAME = 263) visibly rather than crash ----

u32 TRT_GUEST_ABI trt_mciSendStringW(const u16* cmd, u16* ret, u32 ret_len, void*) {
  char line[192];
  char u8[96];
  size_t n = 0;
  for (size_t i = 0; cmd && cmd[i] && n + 1 < sizeof(u8); ++i)
    u8[n++] = (cmd[i] >= 0x20 && cmd[i] < 0x7F) ? (char)cmd[i] : '?';
  u8[n] = 0;
  snprintf(line, sizeof(line), "winmm: mciSendStringW(\"%s\") -> MCIERR_INVALID_DEVICE_NAME", u8);
  trt_core_log(line);
  (void)ret; (void)ret_len;
  return 263;
}

// ---- media key helpers some engines import ----

u32 TRT_GUEST_ABI trt_GetProfileStringW(const u16* app, const u16* key,
                                      const u16* def, u16* out, u32 out_len) {
  if (!out || out_len == 0) return 0;
  out[0] = 0;
  (void)app; (void)key;
  // return the caller's default (copy min(def, out_len-1) UTF-16 chars)
  if (!def) return 0;
  u32 n = 0;
  while (def[n] && n + 1 < out_len) { out[n] = def[n]; ++n; }
  out[n] = 0;
  return n;
}

u32 TRT_GUEST_ABI trt_PlaySoundW(const u16* name, void*, u32 flags) {
  char line[128];
  snprintf(line, sizeof(line), "winmm: PlaySoundW(flags=%u) -> no legacy audio, ignored", flags);
  trt_core_log(line);
  (void)name;
  return 1;  // pretend handled so the engine does not retry forever
}

int trt_bridge_register_winmm(void) {
  static const trt_named_fn k_fns[] = {
      {"timeGetTime", (void*)&trt_timeGetTime},
      {"timeBeginPeriod", (void*)&trt_timeBeginPeriod},
      {"timeEndPeriod", (void*)&trt_timeEndPeriod},
      {"timeGetDevCaps", (void*)&trt_timeGetDevCaps},
      {"waveOutGetNumDevs", (void*)&trt_waveOutGetNumDevs},
      {"waveOutGetDevCapsW", (void*)&trt_waveOutGetDevCapsW},
      {"midiOutGetNumDevs", (void*)&trt_midiOutGetNumDevs},
      {"joyGetNumDevs", (void*)&trt_joyGetNumDevs},
      {"joyGetPosEx", (void*)&trt_joyGetPosEx},
      {"mciSendStringW", (void*)&trt_mciSendStringW},
      {"GetProfileStringW", (void*)&trt_GetProfileStringW},
      {"PlaySoundW", (void*)&trt_PlaySoundW},
  };
  trt_module_register("winmm.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  // winmm is served through the api-ms-win-mm apiset too (apiset.cpp maps
  // the contract here; registering the concrete name is the load path)
  trt_module_register("winmmbase.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

} // extern "C"
