// tiramisu/src/guest/guest_lib32.c
// M7 Phase 1 probe guest DLL (32-bit): exports one function, Lib32Add,
// which guest_hello32.exe binds as a sibling export (guest-VA binding, not
// a trap). Linked /DLL /NODEFAULTLIB /NOENTRY /MACHINE:X86 /BASE:
// 0x10000000 — the same preferred base as the EXE, so mapping both
// exercises guest-VA relocation.
typedef unsigned int u32;
__declspec(dllexport) u32 Lib32Add(u32 a, u32 b) { return a + b; }
