// tiramisu/src/guest/guest_imm32.c
// Proves the imm32 bridge: the console has no IME, so ImmGetContext
// answers NULL and the composition verbs answer honestly. A game guarding
// on the NULL context skips its IME path — the designed behavior.
// Returns a bitmask:
//   0x001 ImmGetContext -> NULL (no IME)
//   0x002 ImmAssociateContext -> 0 (no previous context)
//   0x004 ImmAssociateContextEx -> FALSE
//   0x008 ImmGetCompositionStringW -> -1 (no composition exists)
//   0x010 ImmReleaseContext -> TRUE (nothing held)
//   0x020 ImmNotifyIME -> FALSE
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

__attribute__((dllimport)) void* GSTD ImmGetContext(void* hwnd);
__attribute__((dllimport)) i32 GSTD ImmAssociateContext(void* hwnd, void* ctx);
__attribute__((dllimport)) i32 GSTD ImmAssociateContextEx(void* hwnd,
                                                          void* ctx, u32 flags);
__attribute__((dllimport)) i32 GSTD ImmGetCompositionStringW(void* ctx,
                                                             u32 index,
                                                             void* buf,
                                                             u32 len);
__attribute__((dllimport)) i32 GSTD ImmReleaseContext(void* hwnd, void* ctx);
__attribute__((dllimport)) i32 GSTD ImmNotifyIME(void* ctx, u32 action,
                                                 u32 index, u32 value);
__attribute__((dllimport)) i32 GSTD ImmGetConversionStatus(void* ctx,
                                                           u32* conv,
                                                           u32* sentence);

u32 GSTD guest_imm32_main(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  // ---- 1. no IME context exists ----
  if (ImmGetContext((void*)0x1234) == 0) bits |= 0x001u;

  // ---- 2/3. associate: honest no-op answers ----
  if (ImmAssociateContext((void*)0x1234, (void*)0x5678) == 0) bits |= 0x002u;
  if (ImmAssociateContextEx((void*)0x1234, 0, 0) == 0) bits |= 0x004u;

  // ---- 4. composition: no composition exists -> -1 ----
  char buf[8];
  if (ImmGetCompositionStringW(0, 8 /*GCS_COMPSTR*/, buf, sizeof(buf)) == -1)
    bits |= 0x008u;

  // ---- 5/6. release/no-op verbs ----
  if (ImmReleaseContext((void*)0x1234, 0)) bits |= 0x010u;
  if (ImmNotifyIME(0, 0, 0, 0) == 0) bits |= 0x020u;
  u32 conv = 0, sentence = 0;
  (void)ImmGetConversionStatus(0, &conv, &sentence);

  return bits;
}
