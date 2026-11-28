// tiramisu/src/trt/bridge_msvcrt.h
// MSVCRT (msvcrt.dll) C-runtime bridge for MinGW / GCC / legacy Win64 PEs.
#ifndef TIRAMISU_BRIDGE_MSVCRT_H
#define TIRAMISU_BRIDGE_MSVCRT_H

#include "trt.h"
#include "thunk.h"

#ifdef __cplusplus
extern "C" {
#endif

int trt_bridge_register_msvcrt(void);
u16* TRT_GUEST_ABI trt_crt_wcsstr(const u16* haystack, const u16* needle);

#ifdef __cplusplus
}
#endif

#endif
