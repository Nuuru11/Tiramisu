// tiramisu/src/trt/glsl_hlsl.h
// Self-contained GLSL (110..460 core/compat & ESSL 100/300/310) to HLSL SM5
// source-to-source compiler with active uniform, attribute, varying, UBO, and
// sampler reflection.
#ifndef TIRAMISU_GLSL_HLSL_H
#define TIRAMISU_GLSL_HLSL_H

#include "trt.h"

#include <string>
#include <vector>

enum glsl_stage : u32 {
  GLSL_STAGE_VERTEX = 0,
  GLSL_STAGE_FRAGMENT = 1,
  GLSL_STAGE_COMPUTE = 2,
};

enum glsl_val_kind : u32 {
  GLSL_T_UNKNOWN = 0,
  GLSL_T_FLOAT = 1,
  GLSL_T_VEC2 = 2,
  GLSL_T_VEC3 = 3,
  GLSL_T_VEC4 = 4,
  GLSL_T_INT = 5,
  GLSL_T_IVEC2 = 6,
  GLSL_T_IVEC3 = 7,
  GLSL_T_IVEC4 = 8,
  GLSL_T_UINT = 9,
  GLSL_T_UVEC2 = 10,
  GLSL_T_UVEC3 = 11,
  GLSL_T_UVEC4 = 12,
  GLSL_T_BOOL = 13,
  GLSL_T_BVEC2 = 14,
  GLSL_T_BVEC3 = 15,
  GLSL_T_BVEC4 = 16,
  GLSL_T_MAT2 = 17,
  GLSL_T_MAT3 = 18,
  GLSL_T_MAT4 = 19,
  GLSL_T_SAMPLER_2D = 20,
  GLSL_T_SAMPLER_CUBE = 21,
  GLSL_T_SAMPLER_3D = 22,
  GLSL_T_SAMPLER_2D_SHADOW = 23,
  GLSL_T_SAMPLER_2D_ARRAY = 24,
  GLSL_T_IMAGE_2D = 25,
};

struct glsl_var_desc {
  std::string name;
  glsl_val_kind kind = GLSL_T_UNKNOWN;
  int location = -1;     // attrib loc / frag output / explicit location
  u32 array_len = 1;     // >= 1
  u32 cb_offset = 0;     // byte offset within stage cbuffer b0
  u32 cb_bytes = 0;      // aligned byte footprint in cbuffer
  int tex_slot = -1;     // tN / sN slot for samplers or uN for images
  bool is_builtin = false;
};

struct glsl_ubo_desc {
  std::string block_name;
  std::string instance_name;
  u32 binding = 0;       // register(b<binding+1>)
  u32 size_bytes = 0;
};

struct glsl_compile_result {
  bool ok = false;
  glsl_stage stage = GLSL_STAGE_VERTEX;
  int version = 120;
  bool is_es = false;
  std::string hlsl;
  std::string log;
  u32 cb0_bytes = 0;
  u32 local_size[3] = {1, 1, 1};
  bool uses_frag_coord = false;
  bool uses_front_facing = false;
  bool writes_frag_depth = false;
  std::vector<glsl_var_desc> inputs;
  std::vector<glsl_var_desc> outputs;
  std::vector<glsl_var_desc> uniforms;
  std::vector<glsl_ubo_desc> ubos;
};

// Parse and compile one GLSL shader stage to HLSL SM5 (`vs_5_0`/`ps_5_0`/`cs_5_0`).
// `explicit_attrib_locs` / `explicit_frag_locs` carry program-level bindings
// registered before glLinkProgram().
struct glsl_link_bindings {
  struct pair { const char* name; int loc; };
  std::vector<pair> attrib_locs;
  std::vector<pair> frag_locs;
  // When compiling a fragment shader linked to a vertex shader, map varying names
  // to consistent TEXCOORD<idx> slots.
  std::vector<pair> varying_locs;
};

glsl_compile_result glsl_to_hlsl(const char* source, u32 source_len,
                                 glsl_stage stage,
                                 const glsl_link_bindings* link = nullptr);

#endif
