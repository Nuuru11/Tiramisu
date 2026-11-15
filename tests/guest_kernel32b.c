// tiramisu/tests/guest_kernel32b.c
// Guest for tests/test_kernel32b.cpp: the work-order item 12 remainder
// surface (job objects, console attach, toolhelp module walk, mitigation
// policy, stack guarantee) plus the nvapi/nvml/atiadl honest-stub probes
// reached through LoadLibraryW/GetProcAddress — the same path a real game
// takes. Returns a checksum bitmask; the host test decodes each bit into a
// PASS line and verifies the on-disk results.txt.
//
//   0x0001 CreateJobObjectW + AssignProcessToJobObject(self)
//          + SetInformationJobObject(EXTENDED, PROCESS_MEMORY) + Terminate
//   0x0002 OpenJobObjectW honest-fails (NULL, ERROR_FILE_NOT_FOUND)
//   0x0004 AllocConsole -> TRUE
//   0x0008 GetConsoleCP -> 437
//   0x0010 FreeConsole -> TRUE
//   0x0020 CreateToolhelp32Snapshot(TH32CS_SNAPMODULE) -> valid handle
//   0x0040 Module32FirstW: exe module, nonzero base + size + name
//   0x0080 Module32NextW terminates FALSE with ERROR_NO_MORE_FILES (18)
//   0x0100 CloseToolhelp32Snapshot -> TRUE
//   0x0200 GetProcessMitigationPolicy -> TRUE with an all-zero buffer
//   0x0400 nvapi probe: NvAPI_Initialize -> -160, QueryInterface -> NULL
//   0x0800 nvml probe: nvmlInit_v2 -> 9 (NVML_ERROR_NOT_SUPPORTED)
//   0x1000 atiadl probe: ADL2_Main_Control_Create -> -9
//   0x2000 SetThreadStackGuarantee/GetThreadStackGuarantee round-trip
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned long long u64;
typedef long long i64;
typedef int i32;
typedef unsigned char u8;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

int _fltused = 1;

// ---- kernel32 imports ----
__attribute__((dllimport)) void* GSTD CreateJobObjectW(void* attrs,
                                                       const u16* name);
__attribute__((dllimport)) int GSTD AssignProcessToJobObject(void* job,
                                                             void* proc);
__attribute__((dllimport)) int GSTD SetInformationJobObject(void* job,
                                                            u32 cls,
                                                            void* info,
                                                            u32 cb);
__attribute__((dllimport)) int GSTD TerminateJobObject(void* job,
                                                       u32 exit_code);
__attribute__((dllimport)) void* GSTD OpenJobObjectW(u32 access, int inherit,
                                                     const u16* name);
__attribute__((dllimport)) int GSTD IsProcessInJob(void* proc, void* job,
                                                   int* out);
__attribute__((dllimport)) void* GSTD GetCurrentProcess(void);
__attribute__((dllimport)) int GSTD AllocConsole(void);
__attribute__((dllimport)) int GSTD FreeConsole(void);
__attribute__((dllimport)) u32 GSTD GetConsoleCP(void);
__attribute__((dllimport)) void* GSTD CreateToolhelp32Snapshot(u32 flags,
                                                               u32 pid);
__attribute__((dllimport)) int GSTD Module32FirstW(void* snap, void* entry);
__attribute__((dllimport)) int GSTD Module32NextW(void* snap, void* entry);
__attribute__((dllimport)) int GSTD CloseToolhelp32Snapshot(void* snap);
__attribute__((dllimport)) int GSTD GetProcessMitigationPolicy(void* proc,
                                                               u32 policy,
                                                               void* buffer,
                                                               u64 len);
__attribute__((dllimport)) int GSTD SetThreadStackGuarantee(u32* size);
__attribute__((dllimport)) int GSTD GetThreadStackGuarantee(u32* size);
__attribute__((dllimport)) u32 GSTD GetLastError(void);
__attribute__((dllimport)) void* GSTD LoadLibraryW(const u16* name);
__attribute__((dllimport)) void* GSTD GetProcAddress(void* mod,
                                                     const char* name);
__attribute__((dllimport)) void* GSTD CreateFileW(const u16* name, u32 access,
                                                  u32 share, void* sa,
                                                  u32 disp, u32 flags,
                                                  void* tmpl);
__attribute__((dllimport)) int GSTD WriteFile(void* h, const void* buf,
                                              u32 bytes, u32* written,
                                              void* ov);
__attribute__((dllimport)) int GSTD CloseHandle(void* h);

// MODULEENTRY32W, x64 layout (1080 bytes): the guest declares it exactly
// like a real game would — the bridge fills it through the documented
// offsets.
typedef struct {
  u32 dwSize;          // 0
  u32 th32ModuleID;    // 4
  u32 th32ProcessID;   // 8
  u32 GlblcntUsage;    // 12
  u32 ProccntUsage;    // 16
  u8* modBaseAddr;     // 24
  u32 modBaseSize;     // 32
  u64 hModule;         // 40 (HMODULE, pointer-sized)
  u16 szModule[256];   // 48
  u16 szExePath[260];  // 568
} MODULEENTRY32W;      // 1080 bytes

#define TH32CS_SNAPMODULE 0x8u
#define ERROR_NO_MORE_FILES 18u
#define JobObjectExtendedLimitInformation 9u
#define JOB_OBJECT_LIMIT_PROCESS_MEMORY 0x100u

typedef struct {
  u64 per_process_user_time_limit;  // 0
  u64 per_job_user_time_limit;      // 8
  u32 limit_flags;                  // 16
  u32 pad0;                         // 20
  u64 minimum_working_set_size;     // 24
  u64 maximum_working_set_size;     // 32
  u32 active_process_limit;         // 40
  u32 pad1;                         // 44
  u64 affinity;                     // 48
  u32 priority_class;               // 56
  u32 scheduling_class;             // 60
  // IO_COUNTERS (48 bytes)                     64
  u64 io[6];
  u64 process_memory_limit;  // 112
  u64 job_memory_limit;      // 120
  u64 peak_process_memory;   // 128
  u64 peak_job_memory;       // 136
} JOBOBJECT_EXTENDED_LIMIT_INFORMATION;  // 144 bytes

static u16 wlen(const u16* s) {
  u16 n = 0;
  while (s[n]) ++n;
  return n;
}

u32 GSTD guest_k32b_main(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  // ---- (a) job objects ----
  void* job = CreateJobObjectW((void*)0, (const u16*)0);
  if (job != (void*)0 && job != (void*)(long long)-1) {
    int ok = AssignProcessToJobObject(job, GetCurrentProcess());
    // EXTENDED limits with the PROCESS_MEMORY flag: the logged no-op path
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION ext;
    u8* p = (u8*)&ext;
    u32 i;
    for (i = 0; i < sizeof(ext); ++i) p[i] = 0;
    ext.limit_flags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    ext.process_memory_limit = (u64)2048 << 20;  // 2 GiB cap, recorded only
    ok &= SetInformationJobObject(
        job, JobObjectExtendedLimitInformation, &ext,
        (u32)sizeof(ext));
    ok &= TerminateJobObject(job, 0);
    int in_job = 2;
    ok &= IsProcessInJob(GetCurrentProcess(), job, &in_job);
    if (ok && in_job == 1) bits |= 0x0001u;
    CloseHandle(job);
  }
  {
    void* opened = OpenJobObjectW(0x1F0003u /*JOB_OBJECT_ALL_ACCESS*/, 0,
                                  (const u16*)0);
    if (opened == (void*)0 && GetLastError() == 2u /*ERROR_FILE_NOT_FOUND*/)
      bits |= 0x0002u;
  }

  // ---- (b) console ----
  if (AllocConsole() == 1) bits |= 0x0004u;
  if (GetConsoleCP() == 437u) bits |= 0x0008u;
  if (FreeConsole() == 1) bits |= 0x0010u;

  // ---- (c) toolhelp module walk ----
  {
    void* snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap != (void*)0 && snap != (void*)(long long)-1) {
      bits |= 0x0020u;
      MODULEENTRY32W me;
      u8* p = (u8*)&me;
      u32 i;
      for (i = 0; i < sizeof(me); ++i) p[i] = 0;
      me.dwSize = (u32)sizeof(me);
      if (Module32FirstW(snap, &me) == 1 && me.modBaseAddr != (u8*)0 &&
          me.modBaseSize != 0u && wlen(me.szModule) > 0) {
        bits |= 0x0040u;
        // walk to the clean terminator
        int walked = 1;
        for (;;) {
          if (Module32NextW(snap, &me) == 1) {
            if (me.modBaseAddr == (u8*)0 || me.modBaseSize == 0u) {
              walked = 0;
              break;
            }
            continue;
          }
          if (GetLastError() == ERROR_NO_MORE_FILES) bits |= 0x0080u;
          break;
        }
        if (walked && (bits & 0x0080u) && CloseToolhelp32Snapshot(snap) == 1)
          bits |= 0x0100u;
      } else {
        CloseToolhelp32Snapshot(snap);
      }
    }
  }

  // ---- (d) mitigation policy ----
  {
    u8 pol[8];
    u32 i;
    for (i = 0; i < sizeof(pol); ++i) pol[i] = 0xAA;
    if (GetProcessMitigationPolicy(GetCurrentProcess(), 0 /*DEP*/, pol,
                                   sizeof(pol)) == 1) {
      int zeroed = 1;
      for (i = 0; i < sizeof(pol); ++i)
        if (pol[i] != 0) zeroed = 0;
      if (zeroed) bits |= 0x0200u;
    }
  }

  // ---- (e) vendor GPU stubs, through LoadLibraryW/GetProcAddress ----
  {
    // nvapi.dll: NvAPI_Initialize -> NVAPI_NVIDIA_NOT_FOUND (-160)
    static const u16 nvapi[] = {'n', 'v', 'a', 'p', 'i', '.', 'd', 'l', 'l', 0};
    void* m = LoadLibraryW(nvapi);
    if (m) {
      typedef i32 (GSTD *init_fn)(void);
      typedef void* (GSTD *qi_fn)(u32);
      init_fn init = (init_fn)GetProcAddress(m, "NvAPI_Initialize");
      qi_fn qi = (qi_fn)GetProcAddress(m, "NvAPI_QueryInterface");
      if (init && qi && init() == (i32)-160 && qi(0x12345678u) == (void*)0)
        bits |= 0x0400u;
    }
    // nvml.dll: nvmlInit_v2 -> NVML_ERROR_NOT_SUPPORTED (9)
    static const u16 nvml[] = {'n', 'v', 'm', 'l', '.', 'd', 'l', 'l', 0};
    m = LoadLibraryW(nvml);
    if (m) {
      typedef i32 (GSTD *nvml_init_fn)(void);
      nvml_init_fn f = (nvml_init_fn)GetProcAddress(m, "nvmlInit_v2");
      if (f && f() == 9) bits |= 0x0800u;
    }
    // atiadlxx.dll: ADL2_Main_Control_Create -> ADL_ERR_NOT_SUPPORTED (-9)
    static const u16 adl[] = {'a', 't', 'i', 'a', 'd', 'l', 'x', 'x',
                              '.', 'd', 'l', 'l', 0};
    m = LoadLibraryW(adl);
    if (m) {
      typedef i32 (GSTD *adl_create_fn)(void*, i32);
      adl_create_fn f = (adl_create_fn)GetProcAddress(
          m, "ADL2_Main_Control_Create");
      if (f && f((void*)0, 1) == (i32)-9) bits |= 0x1000u;
    }
  }

  // ---- (f) thread stack guarantee round-trip ----
  {
    u32 want = 0x20000u;  // 128 KiB
    u32 got = 0;
    if (SetThreadStackGuarantee(&want) == 1 && GetThreadStackGuarantee(&got) == 1 &&
        got == 0x20000u)
      bits |= 0x2000u;
  }

  // ---- results through the VFS: C:\results.txt ----
  void* f = CreateFileW(
      (const u16[]){'C', ':', '\\', 'r', 'e', 's', 'u', 'l', 't', 's',
                    '.', 't', 'x', 't', 0},
      0x40000000u /*GENERIC_WRITE*/, 0, (void*)0, 2 /*CREATE_ALWAYS*/, 0,
      (void*)0);
  if (f != (void*)0 && f != (void*)(long long)-1) {
    u32 written = 0;
    WriteFile(f, &bits, 4, &written, (void*)0);
    CloseHandle(f);
  }
  return bits;
}
