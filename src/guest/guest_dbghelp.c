// tiramisu/src/guest/guest_dbghelp.c
// Proves the dbghelp/dbgcore bridge: SymInitialize creates a real
// symbol-search record, SymFromAddr honestly fails (no PDBs on console),
// and MiniDumpWriteDump writes a REAL text crash report into the game dir
// through the VFS and returns TRUE.
// Returns a bitmask:
//   0x001 SymInitialize -> TRUE (record created)
//   0x002 SymFromAddr -> FALSE (honest: no PDBs)
//   0x004 SymLoadModuleEx -> returns the base
//   0x008 MiniDumpWriteDump -> TRUE (text crash report written)
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

__attribute__((dllimport)) i32 GSTD SymInitialize(void* proc,
                                                  const u16* path, int invade);
__attribute__((dllimport)) i32 GSTD SymFromAddr(void* proc, u64 addr,
                                                u64* displacement,
                                                void* symbol);
__attribute__((dllimport)) u64 GSTD SymLoadModuleEx(void* proc, void* file,
                                                    const u16* name,
                                                    const u16* image, u64 base,
                                                    u32 size, void* data,
                                                    u32 flags);
__attribute__((dllimport)) i32 GSTD MiniDumpWriteDump(void* proc, u32 pid,
                                                      void* file_h, u32 type,
                                                      void* exc, void* user,
                                                      void* cb);

u32 GSTD guest_dbghelp_main(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  // ---- 1. SymInitialize: the search record exists ----
  if (SymInitialize((void*)(u64)-1, 0, 0)) bits |= 0x001u;

  // ---- 2. SymFromAddr: no PDBs on console -> honest FALSE ----
  u64 disp = 0;
  char sym[128];
  if (SymFromAddr((void*)(u64)-1, 0x180001000ull, &disp, sym) == 0)
    bits |= 0x002u;

  // ---- 3. SymLoadModuleEx: registers the base, no symbols attach ----
  if (SymLoadModuleEx((void*)(u64)-1, 0, (const u16*)L"guest.dll",
                      (const u16*)L"guest.dll", 0x180000000ull, 0x10000, 0,
                      0) == 0x180000000ull)
    bits |= 0x004u;

  // ---- 4. MiniDumpWriteDump: a REAL text crash report in the game dir ----
  // exception struct shape: EXCEPTION_POINTERS { ExceptionRecord*; ContextRecord* }
  // EXCEPTION_RECORD { code; flags; record*; addr; ... }
  u64 exc_record[2] = {0xC0000005ull, 0x180001234ull};  // code + address
  void* exc_pointers[2] = {exc_record, 0};
  if (MiniDumpWriteDump((void*)(u64)-1, 1, 0, 2 /*MiniDumpNormal*/,
                        exc_pointers, 0, 0))
    bits |= 0x008u;

  return bits;
}
