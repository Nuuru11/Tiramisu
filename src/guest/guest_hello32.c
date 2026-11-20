// tiramisu/src/guest/guest_hello32.c
// M7 Phase 1 probe guest (32-bit): freestanding PE32 with a static import
// table against kernel32.dll (GetTickCount64, GetLastError — cdecl decls,
// so the import names are undecorated, like real PE32 files) and
// guest_lib32.dll (Lib32Add), plus a delay-loaded import of
// guest_delay32.dll (Delay32Fn). Built with clang --target=i686-pc-windows-
// msvc, linked /NODEFAULTLIB /ENTRY:guest32_main /MACHINE:X86 /BASE:
// 0x10000000 /DELAYLOAD:guest_delay32.dll. No CRT, no TEB use, no SEH.
// LOAD-ONLY in Phase 1: the gate test maps and binds it, never executes
// it (there is no x86 engine until Phase 2).
typedef unsigned int u32;
typedef unsigned long long u64;

__attribute__((dllimport)) u64 GetTickCount64(void);
__attribute__((dllimport)) u32 GetLastError(void);
__attribute__((dllimport)) u32 Lib32Add(u32 a, u32 b);
__attribute__((dllimport)) u32 Delay32Fn(void);

extern u32* const g_ptr32_table[2];

u32 guest32_main(u32 a, u32 b, u32 c) {
  (void)c;
  const u64 t = GetTickCount64();
  const u32 e = GetLastError();
  const u32 s = Lib32Add(3u, 2u);
  const u32 d = Delay32Fn();
  u32 x = (u32)t ^ e ^ s ^ d;
  x += (a & 0xF) + (b & 0xF);
  x += g_ptr32_table[1][0] + g_ptr32_table[0][1];
  for (volatile int i = 0; i < 16; ++i) x = x * 16777619u + 1u;
  return x;
}
