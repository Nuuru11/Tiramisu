// tiramisu/src/guest/guest_dll.c
// A DLL the game ships with us - the shape of an engine helper DLL.
// It has a DllMain, an export, and it calls back into the host.
typedef unsigned int u32;
typedef unsigned short u16;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

// imported from the host (tiramisu_host.dll) - proves the DLL's own
// imports get bound inside the runtime
__attribute__((dllimport)) void GSTD TiramisuHostLogU32(u32 tag, u32 value);

// A global pointer: real DLLs have these, and they need a .reloc section -
// without one the image cannot move if its preferred base is taken.
static const u16* g_banner = (const u16*)L"guestdll";
u32 g_banner_len(void);
u32 g_banner_len(void) { return g_banner ? 11u : 0u; }

// DllMain: reason 1 == DLL_PROCESS_ATTACH
u32 GSTD dll_main(void* hinst, u32 reason, void* reserved) {
  (void)hinst; (void)reserved;
  if (reason == 1) TiramisuHostLogU32(0xD10, 0x1111);
  return 1;  // TRUE: load succeeds
}

// the export the exe imports statically
u32 GSTD dll_add(u32 a, u32 b) {
  const u32 sum = a + b;
  TiramisuHostLogU32(0xD11, sum);
  return sum;
}
