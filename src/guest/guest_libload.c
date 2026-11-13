// tiramisu/src/guest/guest_libload.c
// The exe half of the DLL-loading proof:
//   1. a STATIC import from a DLL the game ships (the runtime must load
//      that DLL on demand, recursively, before binding the import)
//   2. an explicit LoadLibraryW + GetProcAddress round trip
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned long long u64;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

// from guestdll.dll (shipped with the game)
__attribute__((dllimport)) u32 GSTD dll_add(u32 a, u32 b);
// from kernel32.dll
__attribute__((dllimport)) void* GSTD LoadLibraryW(const u16* path);
__attribute__((dllimport)) void* GSTD GetProcAddress(void* handle,
                                                     const char* name);

u32 GSTD libload_boot(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  // 1. static cross-image import
  const u32 sum = dll_add(20, 22);
  bits |= (sum == 42) ? 0x1u : 0u;
  // 2. explicit load + lookup
  void* h = LoadLibraryW(
      (const u16[]){'g', 'u', 'e', 's', 't', 'd', 'l', 'l', '.', 'd', 'l', 'l', 0});
  bits |= (h != (void*)0) ? 0x2u : 0u;

  void* fn = GetProcAddress(h, "dll_add");
  bits |= (fn != (void*)0) ? 0x4u : 0u;

  if (fn) {
    const u32 sum2 = ((u32 (*)(u32, u32))fn)(1, 2);
    bits |= (sum2 == 3) ? 0x8u : 0u;
  }

  // the raw sum rides in the high byte so the host can see what came back
  return 0x6B000000u | bits | ((sum & 0xFFu) << 8);
}
