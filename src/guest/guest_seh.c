// tiramisu/src/guest/guest_seh.c
// STAGE 7: the SEH/unwind surface OrTMM's CRT startup runs before WinMain:
//   1. VirtualProtect a DATA page RW -> R -> RW (the .pdata-edit dance)
//   2. RtlCaptureContext + RtlLookupFunctionEntry (the host must answer
//      for a live RIP: the unwind-info registry)
// IsProcessorFeaturePresent(0x17) is checked in the host test directly.
//
// ABI note: dllimport DECLARATIONS only. The previous version did
//   extern void* __imp_VirtualProtect;  vp = (vp_t)&__imp_VirtualProtect;
// and clang folded the load+call into a DIRECT call to the IAT slot's
// address as code (E8 to the slot) — the guest executed its own IAT bytes
// and SIGSEGV'd before any bridge ran. dllimport gives the standard
// ff-15 indirect call the real loader binds.
typedef unsigned int u32;
typedef unsigned long long u64;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

__attribute__((dllimport)) int GSTD VirtualProtect(void* addr, u64 size,
                                                   u32 new_prot, u32* old);
__attribute__((dllimport)) void GSTD RtlCaptureContext(void* ctx);
__attribute__((dllimport)) void* GSTD RtlLookupFunctionEntry(u64 rip,
                                                             u64* base_out,
                                                             void* history);

/* the .pdata-edit dance protects a DATA page, never the active stack
   page: protecting the stack page read-only faults the very next call
   (the return-address push). PAGE_READONLY=0x02, PAGE_READWRITE=0x04. */
static volatile char g_page[8];

/* full CONTEXT storage: the host bridge writes the real x64 layout
   (sizeof(CONTEXT) = 0x4d0, Rip at offset 0xF8). */
static unsigned char ctx_buf[0x4D0];

unsigned long long guest_seh_main(void) {
  unsigned long long score = 0;
  unsigned long old = 0;
  // 1. VirtualProtect a data page RW -> R -> RW (the .pdata edit dance)
  if (VirtualProtect((void*)g_page, 1, 0x04 /*PAGE_READWRITE*/, &old)) score |= 1;
  if (VirtualProtect((void*)g_page, 1, 0x02 /*PAGE_READONLY*/, &old)) score |= 2;
  if (VirtualProtect((void*)g_page, 1, 0x04 /*PAGE_READWRITE*/, &old)) score |= 8;
  g_page[0] = 1;  // writable again?
  score |= g_page[0] ? 4 : 0;
  // 2. capture context + lookup function entry (host must answer)
  RtlCaptureContext(ctx_buf);
  const u64 rip = *(const u64*)(ctx_buf + 0xF8);
  u64 base = 0;
  void* fe = RtlLookupFunctionEntry(rip, &base, 0);
  score |= fe ? 16 : 0;
  return score;
}
