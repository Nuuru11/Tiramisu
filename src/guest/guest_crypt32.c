// tiramisu/src/guest/guest_crypt32.c
// Proves the crypt32 bridge: no cert store on console — the open verbs
// fail with named codes, the chain verbs fail honestly, and the free
// verbs succeed (freeing nothing is not a lie).
// Returns a bitmask:
//   0x001 CertOpenSystemStoreW -> NULL (no cert store)
//   0x002 CertOpenSystemStoreA -> NULL
//   0x004 CertOpenStore -> NULL
//   0x008 CertCloseStore -> TRUE (nothing was open)
//   0x010 CertGetCertificateChain -> NULL (no engine)
//   0x020 CertVerifyCertificateChainPolicy -> FALSE (no chain)
//   0x040 CertFreeCertificateContext/Chain -> no crash (defensive frees)
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

__attribute__((dllimport)) void* GSTD CertOpenSystemStoreW(void* hprov,
                                                           const u16* name);
__attribute__((dllimport)) void* GSTD CertOpenSystemStoreA(void* hprov,
                                                           const char* name);
__attribute__((dllimport)) void* GSTD CertOpenStore(const void* provider,
                                                    u32 encoding, void* hprov,
                                                    u32 flags, const void* para);
__attribute__((dllimport)) i32 GSTD CertCloseStore(void* store, u32 flags);
__attribute__((dllimport)) void* GSTD CertGetCertificateChain(void* engine,
                                                              void* leaf,
                                                              void* time,
                                                              void* store,
                                                              void* para,
                                                              u32 flags,
                                                              void* rsv);
__attribute__((dllimport)) i32 GSTD CertVerifyCertificateChainPolicy(
    void* policy, void* chain, void* para, void* status);
__attribute__((dllimport)) void GSTD CertFreeCertificateContext(void* ctx);
__attribute__((dllimport)) void GSTD CertFreeCertificateChain(void* chain);

u32 GSTD guest_crypt32_main(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  // ---- 1/2/3. open verbs: no cert store exists ----
  if (CertOpenSystemStoreW(0, (const u16*)L"MY") == 0) bits |= 0x001u;
  if (CertOpenSystemStoreA(0, "MY") == 0) bits |= 0x002u;
  if (CertOpenStore((const void*)(u64)1, 0, 0, 0, 0) == 0) bits |= 0x004u;

  // ---- 4. close: nothing was ever open, still succeeds ----
  if (CertCloseStore(0, 0)) bits |= 0x008u;

  // ---- 5/6. chain verbs: honest failure ----
  if (CertGetCertificateChain(0, 0, 0, 0, 0, 0, 0) == 0) bits |= 0x010u;
  if (CertVerifyCertificateChainPolicy(0, 0, 0, 0) == 0) bits |= 0x020u;

  // ---- 7. defensive frees: no crash, no lie ----
  CertFreeCertificateContext(0);
  CertFreeCertificateChain(0);
  bits |= 0x040u;

  return bits;
}
