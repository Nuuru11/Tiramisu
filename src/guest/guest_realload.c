// tiramisu/src/guest/guest_realload.c
// The load path a REAL PC game walks, not a skeleton:
//
//   1. api-ms-win-core-synch-l1-2-0.dll!WaitOnAddress
//      -> contract name, not a file. Must redirect to the DLL that
//         implements it (kernel32) and still reach a real function.
//   2. api-ms-win-core-winrt-l1-1-0.dll!WindowsCreateString
//      -> contract name -> combase.dll (console real).
//   3. advapi32.dll!RegOpenKeyExW
//      -> no console equivalent yet: must BIND (game still boots) and
//         fail visibly at the call site (never a silent success).
//   4. d3dcompiler_47.dll!D3DCompile (DELAY-loaded)
//      -> delay import: bound eagerly, unresolved -> fail-visible stub,
//         the load must NOT die because an optional DLL is missing.
//
// Returns a bitmask the host test asserts exactly.
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned long long u64;
typedef long long i64;
typedef int i32;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

// api-set contract: kernel32 family
__attribute__((dllimport)) i32 GSTD WaitOnAddress(const void* addr, u32 ms,
                                                  u32 flags);
__attribute__((dllimport)) void GSTD WakeByAddress(const void* addr);
__attribute__((dllimport)) void GSTD WakeByAddressAll(const void* addr);
// api-set contract: combase family
__attribute__((dllimport)) void* GSTD WindowsCreateString(const u16* src,
                                                           u32 len);
// advapi32: registry (STUB today)
__attribute__((dllimport)) i32 GSTD RegOpenKeyExW(void* key, const u16* sub,
                                                   u32 opts, void* sam,
                                                   void** out);
// d3dcompiler_47: DELAY-loaded
__attribute__((dllimport)) void* GSTD D3DCompile(const void* src, u64 len,
                                                  const void* name,
                                                  void* defines, void* include,
                                                  void* pch, u32 flags,
                                                  u32* code, void** errors);

u32 GSTD real_load_boot(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  // 1. contract -> kernel32: the call must reach the real function
  const u32 r1 = (u32)WaitOnAddress((const void*)0x1234, 0, 1);
  bits |= (r1 == 0) ? 0x1u : 0u;      // WAIT_OK

  // 2. contract -> combase: the call must reach the real function and
  //    return its argument (the fake echoes the pointer back)
  static const u16 src[] = {'h', 'i', 0};
  void* s = WindowsCreateString(src, 2);
  bits |= (s == (void*)src) ? 0x2u : 0u;

  // 3. advapi32: the registry bridge answers with a NAMED error for a bad
  //    handle - a failure, not a silent success
  void* hkey = (void*)0;
  const i32 r3 = RegOpenKeyExW((void*)0, (const u16[]){'S', 'o', 'f', 't', 0},
                               0, (void*)0, &hkey);
  bits |= (r3 == 6 /*ERROR_INVALID_HANDLE*/) ? 0x4u : 0u;

  // 4. delay import: must not kill the load; calling it must be visible
  u32 code_len = 0;
  void* errs = (void*)0;
  void* r4 = D3DCompile(src, 4, (void*)0, (void*)0, (void*)0, (void*)0, 0,
                        &code_len, &errs);
  bits |= (r4 == (void*)0) ? 0x8u : 0u;

  return 0x5A000000u | bits;
}
