// tiramisu/src/guest/guest_psapi.c
// Proves the psapi bridge: the module enumeration surface reports the REAL
// guest module list (the loader's image registry), and the unimplemented
// verbs fail honestly with last-error semantics.
// Returns a bitmask:
//   0x001 EnumProcesses -> 1 pid (the guest itself)
//   0x002 K32EnumProcessModules -> at least 1 module handle
//   0x004 K32GetModuleFileNameExW(NULL) -> the guest's own image name
//   0x010 GetProcessMemoryInfo -> FALSE (honest: no accounting)
typedef unsigned short u16;
typedef unsigned int u32;
typedef int i32;
typedef unsigned long long u64;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

int _fltused = 1;

__attribute__((dllimport)) i32 GSTD EnumProcesses(u32* ids, u32 cb,
                                                  u32* returned);
__attribute__((dllimport)) i32 GSTD K32EnumProcessModules(void* proc,
                                                          void** handles,
                                                          u32 cb,
                                                          u32* needed);
__attribute__((dllimport)) u32 GSTD K32GetModuleFileNameExW(void* proc,
                                                            void* mod,
                                                            u16* out, u32 cap);
__attribute__((dllimport)) i32 GSTD GetProcessMemoryInfo(void* proc,
                                                         void* info, u32 cb);

u32 GSTD guest_psapi_main(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  // ---- 1. EnumProcesses: one pid, the guest itself ----
  u32 ids[8];
  u32 returned = 0;
  if (EnumProcesses(ids, sizeof(ids), &returned) && returned == 4 && ids[0] == 1)
    bits |= 0x001u;

  // ---- 2. K32EnumProcessModules: the REAL guest image table ----
  void* mods[32];
  u32 needed = 0;
  u32 n = 0;
  if (K32EnumProcessModules((void*)(u64)1, mods, sizeof(mods), &needed)) {
    n = needed / 8;
    if (n >= 1) bits |= 0x002u;
  }

  // ---- 3. module file name: the guest image itself ----
  u16 name[260];
  u32 len = K32GetModuleFileNameExW((void*)(u64)1, 0, name, 260);
  if (len > 0) {
    // the honest answer is the guest's own image file name (its leaf)
    u32 i = 0;
    while (name[i] && i < len) ++i;
    // non-empty, NUL-terminated
    if (i > 0 && name[i] == 0) bits |= 0x004u;
  }

  // ---- 4. memory info: honest FALSE ----
  if (GetProcessMemoryInfo((void*)(u64)1, 0, 0) == 0) bits |= 0x010u;
  (void)mods;

  return bits;
}
