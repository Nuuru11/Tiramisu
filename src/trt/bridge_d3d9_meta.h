// tiramisu/src/trt/bridge_d3d9_meta.h
// Wave 2B D3D9 metadata helpers shared between bridge_d3d9.cpp and the host
// D3D11 backend (MojoShader cbuffer layout reflection + stream/RS accessors).
#ifndef TIRAMISU_BRIDGE_D3D9_META_H
#define TIRAMISU_BRIDGE_D3D9_META_H

#include "bridge_d3d9.h"

#ifdef __cplusplus
extern "C" {
#endif

struct d3d9_mojo_entry {
  u8 kind;         // 0 = float4, 1 = int4, 2 = bool
  u8 is_array;
  u16 reg_index;   // DX9 register index (cN / iN / bN)
  u16 cb_slot;     // index within uniforms_vec4[] / uniforms_ivec4[] / uniforms_bool[]
  u16 array_count; // 1 for scalar vec4/int4/bool; N for array
};

struct d3d9_mojo_def {
  u16 cb_slot;     // index within uniforms_vec4[] for constant overrides in arrays
  u16 pad;
  float val[4];
};

struct d3d9_mojo_layout {
  u32 is_ps;
  u32 f4_count;
  u32 i4_count;
  u32 b_count;
  u32 sampler_mask; // bit s set if sampler s is declared
  u32 entry_count;
  struct d3d9_mojo_entry entries[512];
  u32 def_count;
  struct d3d9_mojo_def defs[128];
};

// Inspect a MOJOSHADER_parseData* produced with MOJOSHADER_PROFILE_HLSL and
// populate exact b0 cbuffer offsets matching emit_HLSL_finalize(). Returns 1 on success.
int trt_d3d9_mojo_extract(const void* parsed, struct d3d9_mojo_layout* out);

// Query active D3D9 stream source binding (0..7). Returns 1 if stream < 8.
int trt_d3d9_get_stream(u32 stream, d3d9_handle* vb, u32* offset, u32* stride,
                        u32* freq);

// Query current D3DRS_* render state cached in bridge_d3d9.
u32 trt_d3d9_get_rs(u32 state);

// Return exact HLSL b0 cbuffer byte size (multiple of 16) for a MojoShader
// parsed shader, plus bitmask of declared samplers.
u32 trt_d3d9_mojo_cb_info(const void* parsed, u32* out_sampler_mask);

// Pack D3D9 float4/int4/bool constant register files into MojoShader HLSL b0 layout.
// cf: float[>=256*4], ci: int[>=32*4], cb: int[>=32]. Returns 1 on success.
int trt_d3d9_mojo_pack_cb(const void* parsed, const float* cf, const i32* ci,
                          const int* cb, void* dst_cb, u32 dst_bytes);

#ifdef __cplusplus
}
#endif

#endif
