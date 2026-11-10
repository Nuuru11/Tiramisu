// tiramisu/src/guest/guest_hello.c
// Tiramisu smoke guest: freestanding PE64 with an import table against
// tiramisu_host.dll. Built with clang --target=x86_64-pc-windows-msvc,
// linked /NODEFAULTLIB /ENTRY:guest_main. No CRT, no TEB use, no SEH —
// so the offline test can execute it on a Linux host through the same
// loader the console runs.
typedef unsigned int  u32;
typedef unsigned long long u64;

#ifdef _WIN32
#define TRT_GUEST_STDCALL __stdcall
#else
#define TRT_GUEST_STDCALL
#endif

__attribute__((dllimport)) u32 TRT_GUEST_STDCALL TiramisuHostQueryBuild(void);
__attribute__((dllimport)) void TRT_GUEST_STDCALL TiramisuHostLogU32(u32 tag, u32 value);

// absolute data pointers live in guest_data.c (separate TU: no folding, so
// the base-relocation pass has real DIR64 work — values 3+2=5 are only read
// correctly if relocations were applied)
extern u32* const g_ptr_table[2];

// PE entry point signature (ignored args); returns a checksum in eax.
u32 TRT_GUEST_STDCALL guest_main(u64 a, u64 b, u64 c) {
  (void)c;
  const u32 build = TiramisuHostQueryBuild();
  TiramisuHostLogU32(0x1DEAu, build);
  // touch heap-ish math so the compiler keeps the call
  u32 x = build ^ 0x1DEAu;
  x += (u32)(a & 0xF) + (u32)(b & 0xF);
  x += g_ptr_table[1][0] + g_ptr_table[0][1];  // 5 when relocs are correct
  for (volatile int i = 0; i < 16; ++i) x = x * 16777619u + 1u;
  return x;
}
