// tiramisu/src/trt/spirv_hlsl.h
// Self-contained binary SPIR-V (1.0..1.6, GLSL.std.450) to HLSL SM5 compiler
// with descriptor set/binding, push-constant, vertex input, and workgroup
// reflection for Vulkan -> D3D11 translation.
#ifndef TIRAMISU_SPIRV_HLSL_H
#define TIRAMISU_SPIRV_HLSL_H

#include "trt.h"

#include <string>
#include <vector>

enum spirv_stage : u32 {
  SPV_STAGE_VERTEX = 0,
  SPV_STAGE_TESSELLATION_CONTROL = 1,
  SPV_STAGE_TESSELLATION_EVAL = 2,
  SPV_STAGE_GEOMETRY = 3,
  SPV_STAGE_FRAGMENT = 4,
  SPV_STAGE_COMPUTE = 5,
  SPV_STAGE_ANY = 0xFFu,
};

enum spirv_binding_kind : u32 {
  SPV_BIND_CBV = 0,          // Uniform block -> cbuffer : register(bN)
  SPV_BIND_SRV_BUFFER = 1,   // Readonly storage buffer -> ByteAddressBuffer : register(tN)
  SPV_BIND_UAV_BUFFER = 2,   // Storage buffer -> RWByteAddressBuffer : register(uN)
  SPV_BIND_TEXTURE = 3,      // Sampled image -> Texture2D/Cube/3D : register(tN)
  SPV_BIND_SAMPLER = 4,      // Standalone sampler -> SamplerState : register(sN)
  SPV_BIND_COMBINED_IMG_SMP = 5, // OpTypeSampledImage -> tN + sN
  SPV_BIND_STORAGE_IMG = 6,  // Storage image -> RWTexture2D : register(uN)
};

struct spirv_binding_desc {
  u32 set = 0;
  u32 binding = 0;
  spirv_binding_kind kind = SPV_BIND_CBV;
  u32 hlsl_reg = 0;        // bN / tN / uN / sN (b13 reserved for push constants if used)
  u32 smp_reg = 0;         // when kind == SPV_BIND_COMBINED_IMG_SMP
  u32 byte_size = 0;
  std::string name;
};

struct spirv_io_desc {
  u32 location = 0;
  u32 builtin = 0xFFFFFFFFu;
  std::string name;
  std::string hlsl_type;
};

struct spirv_spec_entry {
  u32 spec_id = 0;
  u32 value_u32 = 0;
};

struct spirv_compile_result {
  bool ok = false;
  spirv_stage stage = SPV_STAGE_VERTEX;
  std::string entry_name = "main";
  std::string hlsl;
  std::string log;
  u32 local_size[3] = {1, 1, 1};
  u32 push_constant_bytes = 0;
  u32 push_constant_reg = 13;
  std::vector<spirv_io_desc> inputs;
  std::vector<spirv_io_desc> outputs;
  std::vector<spirv_binding_desc> bindings;
};

spirv_compile_result spirv_to_hlsl(const u32* words, u32 word_count,
                                   spirv_stage stage_filter = SPV_STAGE_ANY,
                                   const char* entry_name = nullptr,
                                   const spirv_spec_entry* spec_entries = nullptr,
                                   u32 spec_count = 0);

// ---- shader census -------------------------------------------------------
// What the corpus actually asks for, counted instead of guessed. A title that
// reaches the menu and goes black is usually one unsupported feature; without
// a census every such report is a fresh investigation. Every successful
// translation records its stage and the features it had to lower, so the
// worklist is a sorted list of counters rather than an anecdote.
struct spirv_census {
  u32 compiles = 0;
  u32 per_stage[6] = {0, 0, 0, 0, 0, 0};  // SPV_STAGE_* order
  u32 derivatives = 0;    // ddx/ddy/fwidth lowered from GLSL.std.450
  u32 atomics = 0;        // Interlocked* -> SM5 intrinsics
  u32 storage_images = 0; // RWTexture* (image load/store)
  u32 groupshared = 0;    // compute shared memory
  u32 tessellation = 0;   // tess control/eval path emitted
};
void spirv_census_reset();
spirv_census spirv_census_snapshot();
std::string spirv_census_json();
// Write the census as JSON so a corpus run can be diffed between builds.
bool spirv_census_write(const char* path);

#endif
