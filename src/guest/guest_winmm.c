// tiramisu/src/guest/guest_winmm.c
// Proves winmm!timeGetTime actually advances and timeBeginPeriod is
// accepted — the frame-timing clock games live on. Returns a bitmask:
//   0x01 timeBeginPeriod(1) accepted
//   0x02 clock nondecreasing
//   0x04 clock advances (within a busy-spin window)
//   0x08 waveOutGetNumDevs answers 1 (one real output device)
//   0x10 timeGetDevCaps returns sane min/max
//   0x20 mciSendStringW fails visibly (263), no crash
//   0x40 joyGetPos reports controller state and WinMM errors
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef unsigned long long u64;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

int _fltused = 1;  // any float in the TU makes MSVC linkers want this

__attribute__((dllimport)) u32 GSTD timeGetTime(void);
__attribute__((dllimport)) u32 GSTD timeBeginPeriod(u32 ms);
__attribute__((dllimport)) u32 GSTD timeEndPeriod(u32 ms);
__attribute__((dllimport)) u32 GSTD timeGetDevCaps(void* caps, u32 size);
__attribute__((dllimport)) u32 GSTD joyGetPos(u32 id, void* info);
__attribute__((dllimport)) u32 GSTD waveOutGetNumDevs(void);
__attribute__((dllimport)) u32 GSTD mciSendStringW(const u16* cmd, u16* ret,
                                                    u32 ret_len, void* hwnd);

u32 GSTD guest_winmm_main(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  // 1. raise timer resolution
  if (timeBeginPeriod(1) == 0) bits |= 0x01u;

  // 2. clock nondecreasing across a busy window, and actually advances
  const u32 t0 = timeGetTime();
  u32 t1 = t0;
  // Poll until a millisecond boundary rather than assuming a fixed number
  // of empty iterations takes at least a millisecond on the runner.
  for (u32 i = 0; i < 1000000 && t1 == t0; ++i) t1 = timeGetTime();
  if (t1 >= t0) bits |= 0x02u;
  if (t1 > t0) bits |= 0x04u;

  // 3. legacy device probes answer without crashing
  if (waveOutGetNumDevs() == 1) bits |= 0x08u;

  // 4. TIMECAPS sane
  u32 caps[2] = {0, 0};
  if (timeGetDevCaps(caps, (u32)sizeof(caps)) == 0 && caps[0] >= 1 &&
      caps[1] >= caps[0])
    bits |= 0x10u;

  // 5. MCI fails visibly with the "no such device" code, no crash
  const u16 cmd[] = { 'o','p','e','n',' ','p','a','u','s','e','.','w','a','v',
                      ' ','t','y','p','e',' ','w','a','v','e','a','u','d','i',
                      'o', 0 };
  u16 ret[32];
  if (mciSendStringW(cmd, ret, 32, 0) == 263) bits |= 0x20u;

  u32 joy[4] = {0, 0, 0, 0};
  if (joyGetPos(1, joy) == 167 && joyGetPos(4, joy) == 2 &&
      joyGetPos(0, 0) == 165 && joyGetPos(0, joy) == 0 &&
      joy[0] == 65535 && joy[1] == 65535 && joy[2] == 32768 && joy[3] == 5)
    bits |= 0x40u;

  timeEndPeriod(1);
  return bits;
}
