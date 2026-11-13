// tiramisu/src/trt/bridge_imm32.cpp
// IMM32 bridge. See bridge_imm32.h.
//
// The console has no IME: ImmGetContext returns NULL (no input-context
// record exists — the honest answer), every associate/release/set call is
// a defensive no-op that names itself in the log. Games guard on the NULL
// context and skip their IME path.
#include "bridge_imm32.h"
#include "thunk.h"
#include "trt.h"

#include <stdio.h>
#include <string.h>

namespace {
namespace immlog {
static void line(const char* s) { trt_core_log(s); }
}
#define log_line(s_) immlog::line(s_)
}  // namespace

extern "C" {

void* TRT_GUEST_ABI imm_ImmGetContext(void* hwnd) {
  char line[128];
  snprintf(line, sizeof(line),
           "imm32: ImmGetContext -> NULL (no IME on console; %s)", hwnd ? "hwnd" : "null hwnd");
  log_line(line);
  return nullptr;
}

int TRT_GUEST_ABI imm_ImmAssociateContext(void* hwnd, void* ctx) {
  char line[128];
  snprintf(line, sizeof(line),
           "imm32: ImmAssociateContext -> NULL (no IME: nothing associated)");
  log_line(line);
  (void)hwnd; (void)ctx;
  return 0;  // the previous context — there never was one
}

int TRT_GUEST_ABI imm_ImmAssociateContextEx(void* hwnd, void* ctx, u32 flags) {
  char line[128];
  snprintf(line, sizeof(line),
           "imm32: ImmAssociateContextEx -> FALSE (no IME on console)");
  log_line(line);
  (void)hwnd; (void)ctx; (void)flags;
  return 0;
}

int TRT_GUEST_ABI imm_ImmReleaseContext(void* hwnd, void* ctx) {
  char line[128];
  snprintf(line, sizeof(line),
           "imm32: ImmReleaseContext -> TRUE (no IME context to release)");
  log_line(line);
  (void)hwnd; (void)ctx;
  return 1;  // nothing held: releasing nothing succeeds
}

int TRT_GUEST_ABI imm_ImmGetCompositionStringW(void* ctx, u32 index,
                                               void* buf, u32 len) {
  char line[160];
  snprintf(line, sizeof(line),
           "imm32: ImmGetCompositionStringW -> -1 (ERROR_INVALID_PARAMETER: "
           "no IME composition exists; index=0x%x)", index);
  log_line(line);
  (void)ctx; (void)buf; (void)len;
  return -1;  // the real API's failure code for an empty/absent string
}

int TRT_GUEST_ABI imm_ImmSetCompositionStringW(void* ctx, u32 index,
                                               void* comp, u32 clen, void* read,
                                               u32 rlen) {
  char line[128];
  snprintf(line, sizeof(line),
           "imm32: ImmSetCompositionStringW -> FALSE (no IME on console)");
  log_line(line);
  (void)ctx; (void)index; (void)comp; (void)clen; (void)read; (void)rlen;
  return 0;
}

int TRT_GUEST_ABI imm_ImmNotifyIME(void* ctx, u32 action, u32 index, u32 value) {
  char line[128];
  snprintf(line, sizeof(line),
           "imm32: ImmNotifyIME -> FALSE (no IME on console)");
  log_line(line);
  (void)ctx; (void)action; (void)index; (void)value;
  return 0;
}

int TRT_GUEST_ABI imm_ImmGetConversionStatus(void* ctx, u32* conv,
                                             u32* sentence) {
  char line[128];
  snprintf(line, sizeof(line),
           "imm32: ImmGetConversionStatus -> FALSE (no IME on console)");
  log_line(line);
  (void)ctx; (void)conv; (void)sentence;
  return 0;
}

int TRT_GUEST_ABI imm_ImmSetOpenStatus(void* ctx, int open) {
  char line[128];
  snprintf(line, sizeof(line),
           "imm32: ImmSetOpenStatus -> FALSE (no IME on console)");
  log_line(line);
  (void)ctx; (void)open;
  return 0;
}

int trt_bridge_register_imm32(void) {
  static const trt_named_fn k_fns[] = {
      {"ImmGetContext", (void*)&imm_ImmGetContext},
      {"ImmAssociateContext", (void*)&imm_ImmAssociateContext},
      {"ImmAssociateContextEx", (void*)&imm_ImmAssociateContextEx},
      {"ImmReleaseContext", (void*)&imm_ImmReleaseContext},
      {"ImmGetCompositionStringW", (void*)&imm_ImmGetCompositionStringW},
      {"ImmSetCompositionStringW", (void*)&imm_ImmSetCompositionStringW},
      {"ImmNotifyIME", (void*)&imm_ImmNotifyIME},
      {"ImmGetConversionStatus", (void*)&imm_ImmGetConversionStatus},
      {"ImmSetOpenStatus", (void*)&imm_ImmSetOpenStatus},
  };
  trt_module_register("imm32.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

}  // extern "C"
