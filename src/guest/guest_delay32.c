// tiramisu/src/guest/guest_delay32.c
// M7 Phase 1 probe guest DLL (32-bit): exports Delay32Fn, which
// guest_hello32.exe imports through /DELAYLOAD (delay-import directory
// binding). Linked /DLL /NODEFAULTLIB /NOENTRY /MACHINE:X86 /BASE:
// 0x10000000.
typedef unsigned int u32;
__declspec(dllexport) u32 Delay32Fn(void) { return 7; }
