// tiramisu/src/trt/bridge_res.h
// PE resource directory (.rsrc) surface — Master-Prompt Sprint-1 item 1J.
#ifndef TIRAMISU_BRIDGE_RES_H
#define TIRAMISU_BRIDGE_RES_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register the resource surface onto kernel32 (merge semantics: adds the
// FindResource*/LoadResource/LockResource/LoadStringW family without
// disturbing existing registrations). Call after trt_bridge_register_kernel32.
int trt_bridge_register_res(void);

// exported for tests (and any bridge that needs to read game resources)
void* TRT_STDCALL trt_FindResourceExW(void* hmod, const u16* type_w,
                                      const u16* name_w, u16 lang);
void* TRT_STDCALL trt_FindResourceW(void* hmod, const u16* name_w,
                                    const u16* type_w);
void* TRT_STDCALL trt_FindResourceExA(void* hmod, const char* type_a,
                                      const char* name_a, u16 lang);
void* TRT_STDCALL trt_FindResourceA(void* hmod, const char* name_a,
                                    const char* type_a);
u32 TRT_STDCALL trt_SizeofResource(void* hmod, void* hsrc);
void* TRT_STDCALL trt_LoadResource(void* hmod, void* hsrc);
void* TRT_STDCALL trt_LockResource(void* hglob);
int TRT_STDCALL trt_UnlockResource(void* hglob);
int TRT_STDCALL trt_FreeResource(void* hglob);
int TRT_STDCALL trt_EnumResourceNamesW(void* hmod, const u16* type_w,
                                       void* callback, i64 param);
int TRT_STDCALL trt_LoadStringW(void* inst, u32 id, u16* buf, int cch);

#ifdef __cplusplus
}
#endif

#endif
