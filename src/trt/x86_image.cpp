// tiramisu/src/trt/x86_image.cpp
// Bridge between trt_image (PE32 mapping) and the clean-room interpreter.
#include "x86_32.h"
#include "images.h"
#include "pe.h"
#include "thunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

struct trap_ctx {
  char last[160];
};

int image_trap(trt_x86_ctx* c, u32 va, void* user) {
  trap_ctx* t = (trap_ctx*)user;
  if (trt_pe32_is_trap(va)) {
    int idx = (int)((va - TRT_PE32_TRAP_BASE) >> 2);
    char mod[64] = "?", sym[96] = "?";
    u16 ord = 0;
    u32 trap_va = 0;
    trt_pe32_trap_info(idx, mod, sizeof(mod), sym, sizeof(sym), &ord,
                       &trap_va);
    snprintf(t->last, sizeof(t->last), "%s!%s", mod, sym);
    char line[256];
    snprintf(line, sizeof(line),
             "x86-32: NAMED trap %s!%s -> 0 (no 32-bit bridge call yet)",
             mod, sym);
    trt_core_log(line);
    // cdecl/stdcall: a 32-bit return value lives in EAX; leave it zero.
    c->r[X86_EAX] = 0;
    c->eip += 4;  // the trap VA is one 4-byte call target
    return 0;
  }
  snprintf(t->last, sizeof(t->last), "out-of-window 0x%08x", va);
  char line[224];
  snprintf(line, sizeof(line), "x86-32: NAMED fault at 0x%08x", va);
  trt_core_log(line);
  return 0x80000003u;
}

}  // namespace

int trt_x86_run_image(trt_image* img, u32 entry_va, u32 stack_size,
                      u64 max_instructions, u32* out_eax,
                      char* trap_out, size_t trap_cap) {
  if (!img || !img->is_pe32 || !img->base || stack_size < 4096) return 2;
  const u32 isz = (u32)img->size_of_image;
  if (!isz || isz > 64u * 1024u * 1024u) return 2;

  // One flat guest window: [guest_base, guest_base + image + stack).
  const size_t total = (size_t)isz + stack_size;
  u8* mem = (u8*)calloc(total, 1);
  if (!mem) return 2;
  memcpy(mem, img->base, isz);
  const u32 stack_va = img->guest_base + isz;

  trt_x86_ctx c{};
  c.mem = mem;
  c.mem_va = img->guest_base;
  c.mem_size = (u32)total;
  c.eip = entry_va;
  c.r[X86_ESP] = stack_va + stack_size - 4;
  c.r[X86_EBP] = c.r[X86_ESP];

  trap_ctx t{};
  c.trap = &image_trap;
  c.trap_user = &t;

  int rc = trt_x86_run(&c, max_instructions);

  // Copy back data writes (the stack region is scratch; image pages are not).
  memcpy(img->base, mem, isz);
  if (out_eax) *out_eax = c.r[X86_EAX];
  if (trap_out && trap_cap) {
    snprintf(trap_out, trap_cap, "%s", t.last);
  }
  free(mem);
  return rc;
}
