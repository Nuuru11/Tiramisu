// tiramisu/src/trt/bridge_user32.h
// user32 for the guest (bridge_user32.cpp).
#ifndef TIRAMISU_BRIDGE_USER32_H
#define TIRAMISU_BRIDGE_USER32_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register user32.dll as BUILTIN.
int trt_bridge_register_user32(void);

// ---- DC records (shared with bridge_gdi) ----
// GetDC hands out record handles; ReleaseDC frees them. bridge_gdi keys
// its per-DC state on these SAME records so SelectObject(hdc) from a
// user32 GetDC works and a fake DC fails visibly.
void* trt_user32_dc_alloc(void);
int   trt_user32_dc_release(void* hdc);
int   trt_user32_dc_is_valid(void* hdc);
void* trt_user32_dc_selected(void* hdc);
void  trt_user32_dc_set_selected(void* hdc, void* hobj);

// ---- Icon object table (shared with bridge_shell ExtractIconEx*) ----
void* trt_user32_icon_alloc(int w, int h);
int   trt_user32_icon_free(void* hicon);
int   trt_user32_icon_size(void* hicon, int* w_out, int* h_out);

#ifdef __cplusplus
}
#endif

#endif
