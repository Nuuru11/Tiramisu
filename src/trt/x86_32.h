// tiramisu/src/trt/x86_32.h
// Clean-room x86-32 interpreter (MIT, no GPL code). Executes 32-bit guest
// instructions against a flat guest-VA memory window; calls outside the
// window are routed to a named trap callback so Win32 imports can be bridged.
#ifndef TIRAMISU_X86_32_H
#define TIRAMISU_X86_32_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register indices: eax ecx edx ebx esp ebp esi edi
enum { X86_EAX = 0, X86_ECX, X86_EDX, X86_EBX, X86_ESP,
       X86_EBP, X86_ESI, X86_EDI, X86_NREGS };

typedef struct trt_x86_ctx {
  u32 r[X86_NREGS];
  u32 eip;
  u32 eflags;   // bit0 CF, bit6 ZF, bit7 SF, bit11 OF
  u8* mem;       // host bytes backing [mem_va, mem_va + mem_size)
  u32 mem_va;
  u32 mem_size;
  // Called when EIP is outside the mapped window (a trap/import thunk).
  // Return 0 to continue, nonzero to stop with *stop_code set.
  int (*trap)(struct trt_x86_ctx* c, u32 va, void* user);
  void* trap_user;
  u32 stop_code;
} trt_x86_ctx;

// Execute from ctx->eip until HLT, a trap stop, or max_instructions.
// Returns 0 on HLT, 1 on trap stop, 2 on invalid/fault (stop_code set).
int trt_x86_run(trt_x86_ctx* c, u64 max_instructions);

// Helpers used by the guest-memory bridge thunks.
int trt_x86_mem_read(const trt_x86_ctx* c, u32 va, u32 len, void* out);
int trt_x86_mem_write(trt_x86_ctx* c, u32 va, u32 len, const void* in);

#ifdef __cplusplus
}
#endif
#endif
