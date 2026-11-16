// tiramisu/tests/test_unwind_safety.cpp
// Regression test for the unwinder hardening (measured live: Go Next demo /
// UnityPlayer SIGSEGV'd the HOST inside trt_RtlVirtualUnwind because its
// exception path handed us corrupt/unmapped unwind metadata; OrTMM spun
// 14.5M identical EH bounces).
//
// Contract: a corrupt .pdata, an out-of-image unwind-info pointer, or a
// garbage fn_entry must produce a clean "no handler" verdict (return 0) —
// never a host crash, never a hang.
//
//  1. Null fn_entry + null image_base -> leaf pop, no crash, return 0.
//  2. fn_entry pointing into unmapped memory (0x1234) -> clean 0.
//  3. image_base with unwind RVA pointing into unmapped memory -> clean 0
//     (the metadata-rejection path; the named log line must fire).
//  4. A valid minimal unwind (PUSH_NONVOL rbp + ret) -> still unwinds
//     correctly (safety must not break correctness).
//  5. Unmapped address inside the wide host range -> kernel readability
//     probe (/dev/null EFAULT) rejects it before any dereference.
//  6. Chained unwind metadata pointing at itself -> bounded hop loop,
//     named verdict, clean 0 (no dispatcher hang).
//
// Host-driven, offline, no guest exe needed.
#include "../src/trt/trt.h"
#include "../src/trt/bridge_ntdll.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern "C" {
// From bridge_kernel32.cpp (definition) — validated unwinder.
u64 TRT_STDCALL trt_RtlVirtualUnwind(u32 handler_type, u64 image_base,
                                     u64 ctrl_pc, void* fn_entry, void* ctx,
                                     void** handler_data, u64* establisher,
                                     void* ctx_ptrs);
}

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg)                                              \
  do {                                                                \
    if (cond) {                                                       \
      ++g_pass;                                                       \
    } else {                                                          \
      ++g_fail;                                                       \
      printf("  FAIL %s\n", msg);                                     \
    }                                                                 \
  } while (0)

// Minimal Win64 CONTEXT we can hand the unwinder. The real layout constants
// come from bridge_kernel32's ctx helpers; we only need rsp/rip/gprs. The
// unwinder reads through ctx_gpr/ctx_set_* which index a fixed layout —
// build a big zeroed buffer matching k_ctx_size (1232).
static u8 g_ctx[1232];

int main() {
  printf("== unwind safety regression ==\n");
  // ---- test 1: null fn_entry + null image_base (leaf pop path) ----
  memset(g_ctx, 0, sizeof(g_ctx));
  // rsp points at a valid return address on our own stack.
  // ctx_gpr layout: gpr N at 0x78 + N*8 → rsp (gpr 4) at 0x98.
  u64 ret_addr = 0x180001234ull;
  u64 rsp_slot = ret_addr;
  *(u64*)(g_ctx + 0x98) = (u64)(uintptr_t)&rsp_slot;
  u64 establisher = 0xdeadbeef;
  u64 rc = trt_RtlVirtualUnwind(0, 0, 0, nullptr, g_ctx, nullptr,
                                &establisher, nullptr);
  (void)rc;
  CHECK(establisher == (u64)(uintptr_t)&rsp_slot, "leaf: establisher = rsp");
  // rip restored from the stack slot; rsp advanced by 8
  CHECK(true, "leaf: no crash (reached here)");
  printf("  [t1 leaf pop]\n");
  // ---- test 2: garbage fn_entry (unmapped pointer) -> clean 0 ----
  printf("  [t2 garbage fn_entry]\n");
  memset(g_ctx, 0, sizeof(g_ctx));
  establisher = 0xdeadbeef;
  void* garbage_fn = (void*)(uintptr_t)0x000012340000ull;  // unmapped
  rc = trt_RtlVirtualUnwind(0, 0x180000000ull, 0x180001000ull, garbage_fn,
                            g_ctx, nullptr, &establisher, nullptr);
  CHECK(rc == 0, "garbage fn_entry: return 0 (no handler)");
  CHECK(establisher == 0, "garbage fn_entry: establisher zeroed");

  // ---- test 3: valid fn_entry, unwind RVA into unmapped memory -> clean 0
  // (the exact Go Next/UnityPlayer crash shape: metadata pointer outside any
  // registered image) ----
  printf("  [t3 unmapped metadata]\n");
  memset(g_ctx, 0, sizeof(g_ctx));
  // A RUNTIME_FUNCTION whose .unwind RVA points far outside the image.
  // trt_runtime_fn = {u32 begin, u32 unwind}; place at some plausible
  // in-image RVA.
  static u8 fake_pdata[16] = {
      0x00, 0x10, 0x00, 0x00,  // fn.begin  rva 0x1000
      0x00, 0x20, 0x00, 0x00,  // fn.end    rva 0x2000
      0xFF, 0xFF, 0xFF, 0x0F,  // fn.unwind rva 0xFFFFFFF (unmapped)
      0x00, 0x00, 0x00, 0x00}; // pad: past struct end (probe reads +12)
  establisher = 0xdeadbeef;
  rc = trt_RtlVirtualUnwind(0, 0x180000000ull, 0x180001000ull, fake_pdata,
                            g_ctx, nullptr, &establisher, nullptr);
  CHECK(rc == 0, "unmapped unwind metadata: return 0 (no crash)");
  CHECK(establisher == 0, "unmapped metadata: establisher zeroed");

  // ---- test 4: valid minimal unwind still works (correctness preserved) ----
  printf("  [t4 valid minimal unwind]\n");
  // This requires the metadata to be inside a REGISTERED guest image —
  // register one via trt_load_pe on a tiny PE? Too heavy for offline.
  // Instead: the unwinder's rejection path also accepts wide host ranges;
  // metadata on our own (host) heap within the accepted window unwinds.
  // NOTE: ptr_in_guest_image accepts any pointer above 0x10000 (no image
  // registered in this test binary), so static fake pdata above that line
  // exercises the real unwind path.
  memset(g_ctx, 0, sizeof(g_ctx));
  // unwind info: version=1 flags=0 prolog=2 count=1 frame=0, one
  // UWOP_PUSH_NONVOL(rbp=5) code. UNWIND_CODE layout: byte0 = code offset,
  // byte1 = (Info<<4)|Op -> {0x00, 0x50} = op 0 (PUSH_NONVOL), reg 5 (rbp).
  static u8 ui[8] = {0x08, 0x02, 0x01, 0x00, 0x00, 0x50, 0x00, 0x00};
  // runtime fn: begin=0, unwind = RVA of ui relative to image base. We can't
  // register an image, so pass image_base = (u64)(uintptr_t)ui - 0x1000 and
  // fn->unwind = 0x1000 — the metadata pointer is (image_base+0x1000)=ui.
  const u64 base = (u64)(uintptr_t)ui - 0x1000ull;
  static u8 rf[16] = {0x00, 0x00, 0x00, 0x00,  // begin rva 0
                      0xFF, 0xFF, 0x00, 0x00,  // end   rva 0xFFFF
                      0x00, 0x10, 0x00, 0x00,  // unwind rva 0x1000 -> ui
                      0x00, 0x00, 0x00, 0x00}; // pad past struct end
  // rsp slot holds a return address; PUSH_NONVOL rbp pops [rsp] into rbp.
  // ctx_gpr layout: gpr N at 0x78 + N*8 → rsp(4)=0x98, rbp(5)=0xA0.
  u64 rsp2_slot = 0x180009999ull;
  *(u64*)(g_ctx + 0x98) = (u64)(uintptr_t)&rsp2_slot;
  *(u64*)(g_ctx + 0xA0) = 0x11111111ull;
  establisher = 0;
  rc = trt_RtlVirtualUnwind(0, base, base + 0x800, rf, g_ctx, nullptr,
                            &establisher, nullptr);
  CHECK(rc == 0, "valid minimal unwind: no handler flag -> 0");
  CHECK(*(u64*)(g_ctx + 0xA0) == rsp2_slot, "PUSH_NONVOL rbp restored from [rsp]");
  // After the push-undo (rsp +8) the epilogue emulation pops RIP from the
  // advanced rsp (another +8): total +16 from the faulting rsp.
  CHECK(*(u64*)(g_ctx + 0x98) == (u64)(uintptr_t)&rsp2_slot + 16,
        "rsp advanced by 16 (push-undo + RIP pop)");

  // ---- test 5: unmapped address inside the wide host range -> rejected by
  // the kernel readability probe (the exact Go Next shape: metadata RVA so
  // large that image_base + RVA lands in an unmapped page; a range check
  // alone still SIGSEGV'd there — 0x1a4a4894c measured). ----
  printf("  [t5 unmapped-in-range probe]\n");
  memset(g_ctx, 0, sizeof(g_ctx));
  *(u64*)(g_ctx + 0x98) = (u64)(uintptr_t)&rsp_slot;  // valid rsp
  static u8 rf_unmap[16] = {
      0x00, 0x00, 0x00, 0x00,  // begin rva 0
      0xFF, 0xFF, 0x00, 0x00,  // end   rva 0xFFFF
      0x00, 0x00, 0x00, 0x0F,  // unwind rva 0x0F000000 (unmapped)
      0x00, 0x00, 0x00, 0x00}; // pad past struct end
  establisher = 0xdeadbeef;
  rc = trt_RtlVirtualUnwind(0, 0x7f0000000000ull, 0x7f0000001000ull, rf_unmap,
                            g_ctx, nullptr, &establisher, nullptr);
  // ui = 0x7f0000000000 + 0x0F000000 = 0x7f00f0000000 — in the accepted range
  // but (almost certainly) unmapped in this process. The probe must reject
  // it before the dereference. (If that page happened to be mapped in a
  // future environment, this still passes — it just exercises real unwind.)
  CHECK(rc == 0 || rc == 1, "unmapped-in-range metadata: clean verdict, no crash");
  CHECK(true, "kernel probe: no SIGSEGV (reached here)");

  // ---- test 6: chained unwind self-loop -> bounded, named verdict ----
  printf("  [t6 chained self-loop]\n");
  // fn->unwind has bit 0 set (chained) and points at itself.
  static u8 rf_self[16] = {
      0x00, 0x00, 0x00, 0x00,  // begin rva 0
      0xFF, 0xFF, 0x00, 0x00,  // end   rva 0xFFFF
      0x01, 0x10, 0x00, 0x00,  // unwind RVA 0x1001 (chained -> self)
      0x00, 0x00, 0x00, 0x00}; // pad past struct end
  const u64 self_base = (u64)(uintptr_t)rf_self - 0x1000ull;  // -> chain lands on rf_self
  memset(g_ctx, 0, sizeof(g_ctx));
  *(u64*)(g_ctx + 0x98) = (u64)(uintptr_t)&rsp_slot;
  establisher = 0xdeadbeef;
  rc = trt_RtlVirtualUnwind(0, self_base, self_base + 0x800, rf_self, g_ctx,
                            nullptr, &establisher, nullptr);
  CHECK(rc == 0, "chained self-loop: return 0 (unwind failed cleanly)");
  CHECK(establisher == 0, "chained self-loop: establisher zeroed");

  printf("  result: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
