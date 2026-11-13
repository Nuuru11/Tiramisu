// tiramisu/src/guest/guest_comctl.c
// Proves the comctl32 bridge: the boot-time common-controls acceptance a
// dialog-based game runs. Returns a bitmask:
//   0x01 InitCommonControls() ran (no crash, returns nothing)
//   0x02 InitCommonControlsEx(ICC_BAR_CLASSES) -> TRUE
//   0x04 InitCommonControlsEx(ICC_STANDARD_CLASSES|ICC_DATE_CLASSES) -> TRUE
//   0x08 InitCommonControlsEx(NULL) -> FALSE (fail-visible)
// The dwICC flags must appear in the host log (checked host-side by the
// log observer in tests/test_comctl.cpp).
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef unsigned long long u64;
typedef int i32;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

int _fltused = 1;

__attribute__((dllimport)) void GSTD InitCommonControls(void);

// INITCOMMONCONTROLSEX (x64): dwSize u32, dwICC u32
struct iccx { u32 dwSize; u32 dwICC; };

__attribute__((dllimport)) i32 GSTD InitCommonControlsEx(const struct iccx* c);

u32 GSTD guest_comctl_main(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  // 1. the classic no-arg call
  InitCommonControls();
  bits |= 0x01u;

  // 2. the OrTMM-shaped call (ordinal #380 on x64 — the .def imports the
  //    NAME, the corpus PE imports the ORDINAL; both bind to the bridge)
  struct iccx bar;
  bar.dwSize = 8;
  bar.dwICC = 0x00000004u /*ICC_BAR_CLASSES*/;
  if (InitCommonControlsEx(&bar) == 1) bits |= 0x02u;

  // 3. multi-flag request
  struct iccx multi;
  multi.dwSize = 8;
  multi.dwICC = 0x00004000u /*ICC_STANDARD_CLASSES*/ |
                0x00000100u /*ICC_DATE_CLASSES*/;
  if (InitCommonControlsEx(&multi) == 1) bits |= 0x04u;

  // 4. NULL fails visibly (FALSE, never silent success)
  if (InitCommonControlsEx(0) == 0) bits |= 0x08u;

  return bits;
}
