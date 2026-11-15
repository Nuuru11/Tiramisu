// tiramisu/src/trt/spirv_hlsl.cpp
// Binary SPIR-V (1.0..1.6) + GLSL.std.450 -> HLSL SM5 compiler.
#include "spirv_hlsl.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

// SPIR-V opcodes
enum spv_op : u16 {
  OP_NOP = 0,
  OP_UNDEF = 1,
  OP_SOURCE = 3,
  OP_NAME = 5,
  OP_MEMBER_NAME = 6,
  OP_EXT_INST_IMPORT = 11,
  OP_EXT_INST = 12,
  OP_MEMORY_MODEL = 14,
  OP_ENTRY_POINT = 15,
  OP_EXECUTION_MODE = 16,
  OP_CAPABILITY = 17,
  OP_TYPE_VOID = 19,
  OP_TYPE_BOOL = 20,
  OP_TYPE_INT = 21,
  OP_TYPE_FLOAT = 22,
  OP_TYPE_VECTOR = 23,
  OP_TYPE_MATRIX = 24,
  OP_TYPE_IMAGE = 25,
  OP_TYPE_SAMPLER = 26,
  OP_TYPE_SAMPLED_IMAGE = 27,
  OP_TYPE_ARRAY = 28,
  OP_TYPE_RUNTIME_ARRAY = 29,
  OP_TYPE_STRUCT = 30,
  OP_TYPE_POINTER = 32,
  OP_TYPE_FUNCTION = 33,
  OP_CONSTANT_TRUE = 41,
  OP_CONSTANT_FALSE = 42,
  OP_CONSTANT = 43,
  OP_CONSTANT_COMPOSITE = 44,
  OP_CONSTANT_NULL = 46,
  OP_SPEC_CONSTANT_TRUE = 48,
  OP_SPEC_CONSTANT_FALSE = 49,
  OP_SPEC_CONSTANT = 50,
  OP_FUNCTION = 54,
  OP_FUNCTION_END = 56,
  OP_FUNCTION_CALL = 57,
  OP_VARIABLE = 59,
  OP_LOAD = 61,
  OP_STORE = 62,
  OP_ACCESS_CHAIN = 65,
  OP_IN_BOUNDS_ACCESS_CHAIN = 66,
  OP_DECORATE = 71,
  OP_MEMBER_DECORATE = 72,
  OP_VECTOR_EXTRACT_DYNAMIC = 77,
  OP_VECTOR_INSERT_DYNAMIC = 78,
  OP_VECTOR_SHUFFLE = 79,
  OP_COMPOSITE_CONSTRUCT = 80,
  OP_COMPOSITE_EXTRACT = 81,
  OP_COMPOSITE_INSERT = 82,
  OP_COPY_OBJECT = 83,
  OP_TRANSPOSE = 84,
  OP_SAMPLED_IMAGE = 86,
  OP_IMAGE_SAMPLE_IMPLICIT_LOD = 87,
  OP_IMAGE_SAMPLE_EXPLICIT_LOD = 88,
  OP_IMAGE_SAMPLE_DREF_IMPLICIT_LOD = 89,
  OP_IMAGE_SAMPLE_DREF_EXPLICIT_LOD = 90,
  OP_IMAGE_SAMPLE_PROJ_IMPLICIT_LOD = 91,
  OP_IMAGE_FETCH = 95,
  OP_IMAGE_READ = 98,
  OP_IMAGE_WRITE = 99,
  OP_IMAGE = 100,
  OP_CONVERT_F_TO_U = 109,
  OP_CONVERT_F_TO_S = 110,
  OP_CONVERT_S_TO_F = 111,
  OP_CONVERT_U_TO_F = 112,
  OP_U_CONVERT = 113,
  OP_S_CONVERT = 114,
  OP_F_CONVERT = 115,
  OP_BITCAST = 124,
  OP_S_NEGATE = 126,
  OP_F_NEGATE = 127,
  OP_I_ADD = 128,
  OP_F_ADD = 129,
  OP_I_SUB = 130,
  OP_F_SUB = 131,
  OP_I_MUL = 132,
  OP_F_MUL = 133,
  OP_U_DIV = 134,
  OP_S_DIV = 135,
  OP_F_DIV = 136,
  OP_U_MOD = 137,
  OP_S_REM = 138,
  OP_F_MOD = 141,
  OP_VECTOR_TIMES_SCALAR = 142,
  OP_MATRIX_TIMES_SCALAR = 143,
  OP_VECTOR_TIMES_MATRIX = 144,
  OP_MATRIX_TIMES_VECTOR = 145,
  OP_MATRIX_TIMES_MATRIX = 146,
  OP_DOT = 148,
  OP_LOGICAL_EQUAL = 164,
  OP_LOGICAL_NOT_EQUAL = 165,
  OP_LOGICAL_OR = 166,
  OP_LOGICAL_AND = 167,
  OP_LOGICAL_NOT = 168,
  OP_SELECT = 169,
  OP_I_EQUAL = 170,
  OP_I_NOT_EQUAL = 171,
  OP_U_GREATER_THAN = 172,
  OP_S_GREATER_THAN = 173,
  OP_U_GREATER_THAN_EQUAL = 174,
  OP_S_GREATER_THAN_EQUAL = 175,
  OP_U_LESS_THAN = 176,
  OP_S_LESS_THAN = 177,
  OP_U_LESS_THAN_EQUAL = 178,
  OP_S_LESS_THAN_EQUAL = 179,
  OP_F_ORD_EQUAL = 180,
  OP_F_ORD_NOT_EQUAL = 182,
  OP_F_ORD_LESS_THAN = 184,
  OP_F_ORD_GREATER_THAN = 186,
  OP_F_ORD_LESS_THAN_EQUAL = 188,
  OP_F_ORD_GREATER_THAN_EQUAL = 190,
  OP_SHIFT_RIGHT_LOGICAL = 194,
  OP_SHIFT_RIGHT_ARITHMETIC = 195,
  OP_SHIFT_LEFT_LOGICAL = 196,
  OP_BITWISE_OR = 197,
  OP_BITWISE_XOR = 198,
  OP_BITWISE_AND = 199,
  OP_NOT = 200,
  OP_DPDX = 207,
  OP_DPDY = 208,
  OP_CONTROL_BARRIER = 224,
  OP_PHI = 245,
  OP_LOOP_MERGE = 246,
  OP_SELECTION_MERGE = 247,
  OP_LABEL = 248,
  OP_BRANCH = 249,
  OP_BRANCH_CONDITIONAL = 250,
  OP_SWITCH = 251,
  OP_KILL = 252,
  OP_RETURN = 253,
  OP_RETURN_VALUE = 254,
  OP_TERMINATE_INVOCATION = 4416,
};

// Decorations
enum spv_dec : u32 {
  DEC_BLOCK = 2,
  DEC_BUFFER_BLOCK = 3,
  DEC_ARRAY_STRIDE = 6,
  DEC_BUILTIN = 11,
  DEC_NON_WRITABLE = 24,
  DEC_LOCATION = 30,
  DEC_BINDING = 33,
  DEC_DESCRIPTOR_SET = 34,
  DEC_OFFSET = 35,
  DEC_SPEC_ID = 1,
};

// Storage classes
enum spv_sc : u32 {
  SC_UNIFORM_CONSTANT = 0,
  SC_INPUT = 1,
  SC_UNIFORM = 2,
  SC_OUTPUT = 3,
  SC_WORKGROUP = 4,
  SC_PRIVATE = 6,
  SC_FUNCTION = 7,
  SC_PUSH_CONSTANT = 9,
  SC_IMAGE = 11,
  SC_STORAGE_BUFFER = 12,
};

// BuiltIns
enum spv_builtin : u32 {
  BI_POSITION = 0,
  BI_POINT_SIZE = 1,
  BI_CLIP_DISTANCE = 3,
  BI_FRAG_COORD = 15,
  BI_FRONT_FACING = 17,
  BI_FRAG_DEPTH = 22,
  BI_NUM_WORKGROUPS = 24,
  BI_WORKGROUP_ID = 26,
  BI_LOCAL_INVOCATION_ID = 27,
  BI_GLOBAL_INVOCATION_ID = 28,
  BI_LOCAL_INVOCATION_INDEX = 29,
  BI_VERTEX_INDEX = 42,
  BI_INSTANCE_INDEX = 43,
};

struct spv_insn {
  u16 op = 0;
  std::vector<u32> w; // w[0] is first operand after opcode+count word
};

struct spv_type {
  enum kind_t {
    T_VOID, T_BOOL, T_INT, T_FLOAT, T_VEC, T_MAT,
    T_IMAGE, T_SAMPLER, T_SAMPLED_IMAGE, T_ARRAY, T_RUNTIME_ARRAY,
    T_STRUCT, T_POINTER, T_FUNC
  } kind = T_VOID;
  u32 width = 32;
  bool is_signed = true;
  u32 comp_type = 0;
  u32 count = 0;
  u32 dim = 1;       // 0=1D, 1=2D, 2=3D, 3=Cube
  bool arrayed = false;
  u32 sampled = 1;   // 1=sampled, 2=storage
  u32 storage_class = 0;
  std::vector<u32> members;
  u32 len_const_id = 0;
};

struct spv_dec_info {
  std::set<u32> flags;
  u32 builtin = 0xFFFFFFFFu;
  int location = -1;
  int binding = -1;
  int set = -1;
  u32 array_stride = 0;
  int spec_id = -1;
  struct member_dec {
    u32 offset = 0;
    bool has_offset = false;
    u32 builtin = 0xFFFFFFFFu;
    int location = -1;
  };
  std::map<u32, member_dec> members;
};

struct spv_const_val {
  bool is_const = false;
  bool is_bool = false;
  bool b = false;
  u32 type_id = 0;
  std::vector<u32> raw_words;
  std::vector<u32> comp_ids;
};

struct spv_var {
  u32 type_id = 0;     // pointer type
  u32 pointee_id = 0;
  u32 storage_class = 0;
  std::string hlsl_name;
  u32 hlsl_reg = 0;
  u32 smp_reg = 0;
  bool is_ssbo = false;
  bool is_push = false;
  bool is_ubo = false;
};

struct spv_ptr_val {
  bool valid = false;
  u32 base_var = 0;
  bool is_ssbo = false;
  std::string ssbo_buf_name;
  std::string ssbo_byte_offset_expr;
  std::string hlsl_lvalue;
  u32 pointee_type = 0;
};

struct spv_ctx {
  u32 bound = 0;
  u32 glsl_ext_id = 0;
  std::vector<spv_insn> insns;
  std::map<u32, std::string> names;
  std::map<std::pair<u32, u32>, std::string> member_names;
  std::map<u32, spv_dec_info> decs;
  std::map<u32, spv_type> types;
  std::map<u32, spv_const_val> consts;
  std::map<u32, spv_var> vars;

  struct ep_info {
    spirv_stage stage = SPV_STAGE_VERTEX;
    u32 func_id = 0;
    std::string name;
    std::vector<u32> iface_ids;
  };
  std::vector<ep_info> eps;
  ep_info active_ep;
  u32 local_size[3] = {1, 1, 1};

  u32 next_b = 0;
  u32 next_t = 0;
  u32 next_s = 0;
  u32 next_u = 0;

  spirv_compile_result res;
  std::string structs_hlsl;
  std::string decls_hlsl;
  std::string helper_funcs_hlsl;
  std::string main_body_hlsl;

  std::map<u32, std::string> exprs;
  std::map<u32, u32> expr_types;
  std::map<u32, spv_ptr_val> ptrs;
  std::map<u32, std::string> phi_vars;

  u32 temp_counter = 0;
  std::map<u32, spirv_spec_entry> spec_overrides;

  std::string read_str(const std::vector<u32>& w, size_t idx, size_t* next_idx = nullptr) {
    std::string s;
    while (idx < w.size()) {
      u32 word = w[idx++];
      for (int b = 0; b < 4; ++b) {
        char ch = (char)((word >> (b * 8)) & 0xFFu);
        if (!ch) {
          if (next_idx) *next_idx = idx;
          return s;
        }
        s.push_back(ch);
      }
    }
    if (next_idx) *next_idx = idx;
    return s;
  }

  std::string get_name(u32 id, const char* prefix = "_v") {
    auto it = names.find(id);
    if (it != names.end() && !it->second.empty()) {
      std::string s = it->second;
      for (char& c : s) if (!isalnum((unsigned char)c) && c != '_') c = '_';
      if (isdigit((unsigned char)s[0])) s = "_" + s;
      // Avoid reserved HLSL keywords
      if (s == "in" || s == "out" || s == "main" || s == "sample" ||
          s == "point" || s == "line" || s == "triangle" || s == "linear" ||
          s == "half" || s == "fixed" || s == "vector" || s == "matrix") {
        s = "_" + s;
      }
      return s + "_" + std::to_string(id);
    }
    return std::string(prefix) + std::to_string(id);
  }

  u32 unwrap_struct_or_pointee(u32 tid) {
    auto it = types.find(tid);
    if (it != types.end() && it->second.kind == spv_type::T_POINTER) {
      return it->second.comp_type;
    }
    return tid;
  }

  u32 const_u32(u32 id, u32 def = 0) {
    auto it = consts.find(id);
    if (it == consts.end() || it->second.raw_words.empty()) return def;
    return it->second.raw_words[0];
  }

  u32 byte_size_of(u32 tid) {
    auto it = types.find(tid);
    if (it == types.end()) return 16;
    const auto& t = it->second;
    switch (t.kind) {
      case spv_type::T_BOOL:
      case spv_type::T_INT:
      case spv_type::T_FLOAT: return std::max(4u, t.width / 8u);
      case spv_type::T_VEC: return byte_size_of(t.comp_type) * t.count;
      case spv_type::T_MAT: return 16u * t.count;
      case spv_type::T_ARRAY: {
        u32 stride = decs[tid].array_stride;
        u32 elem = byte_size_of(t.comp_type);
        if (!stride) stride = (elem + 15u) & ~15u;
        return stride * std::max(1u, const_u32(t.len_const_id, 1u));
      }
      case spv_type::T_STRUCT: {
        u32 max_end = 0;
        const auto& md = decs[tid].members;
        for (u32 i = 0; i < (u32)t.members.size(); ++i) {
          u32 off = md.count(i) && md.at(i).has_offset ? md.at(i).offset : max_end;
          max_end = std::max(max_end, off + byte_size_of(t.members[i]));
        }
        return std::max(16u, (max_end + 15u) & ~15u);
      }
      default: return 16;
    }
  }

  std::string hlsl_type(u32 tid) {
    auto it = types.find(tid);
    if (it == types.end()) return "float4";
    const auto& t = it->second;
    switch (t.kind) {
      case spv_type::T_VOID: return "void";
      case spv_type::T_BOOL: return "bool";
      case spv_type::T_INT: return t.is_signed ? "int" : "uint";
      case spv_type::T_FLOAT: return (t.width == 64) ? "double" : "float";
      case spv_type::T_VEC: {
        std::string b = hlsl_type(t.comp_type);
        return b + std::to_string(t.count);
      }
      case spv_type::T_MAT: {
        // Column-major SPIR-V matC x vecR -> column_major floatRxC
        const auto& col = types[t.comp_type];
        u32 rows = col.count ? col.count : 4;
        return "column_major float" + std::to_string(t.count) + "x" + std::to_string(rows);
      }
      case spv_type::T_IMAGE:
        if (t.sampled == 2) return (t.dim == 2) ? "RWTexture3D<float4>" : "RWTexture2D<float4>";
        if (t.dim == 3) return "TextureCube";
        if (t.dim == 2) return "Texture3D";
        if (t.arrayed) return "Texture2DArray";
        return "Texture2D";
      case spv_type::T_SAMPLER: return "SamplerState";
      case spv_type::T_STRUCT: return get_name(tid, "_Struct");
      case spv_type::T_ARRAY: return hlsl_type(t.comp_type);
      default: return "float4";
    }
  }

  std::string hlsl_local_type(u32 tid) {
    std::string s = hlsl_type(tid);
    const size_t p = s.find("column_major ");
    if (p == 0) s.erase(0, 13);
    return s;
  }

  u32 scalar_comp_count(u32 tid) {
    auto it = types.find(tid);
    if (it == types.end()) return 1;
    if (it->second.kind == spv_type::T_VEC) return it->second.count;
    return 1;
  }

  bool is_float_type(u32 tid) {
    auto it = types.find(tid);
    if (it == types.end()) return false;
    if (it->second.kind == spv_type::T_FLOAT) return true;
    if (it->second.kind == spv_type::T_VEC || it->second.kind == spv_type::T_MAT)
      return is_float_type(it->second.comp_type);
    return false;
  }

  bool is_signed_int_type(u32 tid) {
    auto it = types.find(tid);
    if (it == types.end()) return false;
    if (it->second.kind == spv_type::T_INT) return it->second.is_signed;
    if (it->second.kind == spv_type::T_VEC) return is_signed_int_type(it->second.comp_type);
    return false;
  }

  std::string format_const(u32 id) {
    auto it = consts.find(id);
    if (it == consts.end()) return get_name(id);
    const auto& cv = it->second;
    const auto& ty = types[cv.type_id];
    if (cv.is_bool) return cv.b ? "true" : "false";
    if (ty.kind == spv_type::T_INT && !cv.raw_words.empty()) {
      if (ty.is_signed) return std::to_string((int32_t)cv.raw_words[0]);
      return std::to_string(cv.raw_words[0]) + "u";
    }
    if (ty.kind == spv_type::T_FLOAT && !cv.raw_words.empty()) {
      float f = 0.0f;
      memcpy(&f, &cv.raw_words[0], 4);
      char buf[64];
      snprintf(buf, sizeof(buf), "%.9g", f);
      std::string s(buf);
      if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
          s.find('E') == std::string::npos) {
        s += ".0";
      }
      return s;
    }
    if (ty.kind == spv_type::T_VEC && !cv.comp_ids.empty()) {
      std::string s = hlsl_local_type(cv.type_id) + "(";
      for (size_t i = 0; i < cv.comp_ids.size(); ++i) {
        if (i) s += ", ";
        s += format_const(cv.comp_ids[i]);
      }
      s += ")";
      return s;
    }
    if (ty.kind == spv_type::T_VEC || ty.kind == spv_type::T_MAT) {
      return "(" + hlsl_local_type(cv.type_id) + ")0";
    }
    return "0";
  }

  std::string get_expr(u32 id) {
    auto it = exprs.find(id);
    if (it != exprs.end()) return it->second;
    if (consts.count(id)) return format_const(id);
    if (phi_vars.count(id)) return phi_vars[id];
    return get_name(id);
  }
};

static std::string builtin_semantic(u32 bi, spirv_stage st, bool is_out) {
  switch (bi) {
    case BI_POSITION: return "SV_Position";
    case BI_FRAG_COORD: return "SV_Position";
    case BI_FRONT_FACING: return "SV_IsFrontFace";
    case BI_FRAG_DEPTH: return "SV_Depth";
    case BI_VERTEX_INDEX: return "SV_VertexID";
    case BI_INSTANCE_INDEX: return "SV_InstanceID";
    case BI_GLOBAL_INVOCATION_ID: return "SV_DispatchThreadID";
    case BI_LOCAL_INVOCATION_ID: return "SV_GroupThreadID";
    case BI_LOCAL_INVOCATION_INDEX: return "SV_GroupIndex";
    case BI_WORKGROUP_ID: return "SV_GroupID";
    case BI_CLIP_DISTANCE: return "SV_ClipDistance0";
    default:
      if (st == SPV_STAGE_FRAGMENT && is_out) return "SV_Target0";
      return "TEXCOORD0";
  }
}

static bool has_builtin_in_type(spv_ctx& ctx, u32 tid, u32& out_bi) {
  auto& d = ctx.decs[tid];
  if (d.builtin != 0xFFFFFFFFu) { out_bi = d.builtin; return true; }
  const auto& t = ctx.types[tid];
  if (t.kind == spv_type::T_STRUCT) {
    for (const auto& kv : d.members) {
      if (kv.second.builtin != 0xFFFFFFFFu) { out_bi = kv.second.builtin; return true; }
    }
  }
  return false;
}

static void emit_cbuffer_fields(spv_ctx& ctx, u32 struct_tid, std::string& body) {
  const auto& st = ctx.types[struct_tid];
  const auto& dec = ctx.decs[struct_tid];
  for (u32 i = 0; i < (u32)st.members.size(); ++i) {
    u32 mtid = st.members[i];
    std::string mname;
    auto mnit = ctx.member_names.find({struct_tid, i});
    if (mnit != ctx.member_names.end() && !mnit->second.empty()) mname = mnit->second;
    else mname = "_m" + std::to_string(i);
    u32 off = dec.members.count(i) && dec.members.at(i).has_offset
                  ? dec.members.at(i).offset : (i * 16u);
    const u32 c_reg = off / 16u;
    const u32 c_sub = (off % 16u) / 4u;
    static const char* k_comp[4] = {"", ".y", ".z", ".w"};
    const auto& mt = ctx.types[mtid];
    std::string arr_sfx;
    std::string mty = ctx.hlsl_type(mtid);
    if (mt.kind == spv_type::T_ARRAY) {
      u32 acnt = std::max(1u, ctx.const_u32(mt.len_const_id, 1u));
      arr_sfx = "[" + std::to_string(acnt) + "]";
    }
    char po[64];
    snprintf(po, sizeof(po), " : packoffset(c%u%s);", c_reg, k_comp[c_sub & 3u]);
    body += "  " + mty + " " + mname + "_" + std::to_string(struct_tid) + "_" +
            std::to_string(i) + arr_sfx + po + "\n";
  }
}

static std::string cbuffer_member_expr(spv_ctx& ctx, u32 struct_tid, u32 mem_idx) {
  auto mnit = ctx.member_names.find({struct_tid, mem_idx});
  std::string mname = (mnit != ctx.member_names.end() && !mnit->second.empty())
                          ? mnit->second : ("_m" + std::to_string(mem_idx));
  return mname + "_" + std::to_string(struct_tid) + "_" + std::to_string(mem_idx);
}

static std::string ssbo_load_expr(spv_ctx& ctx, const spv_ptr_val& pv, u32 res_tid) {
  const auto& ty = ctx.types[res_tid];
  const u32 n = (ty.kind == spv_type::T_VEC) ? ty.count : 1u;
  std::string load_fn = (n == 4) ? "Load4" : (n == 3 ? "Load3" : (n == 2 ? "Load2" : "Load"));
  std::string raw = pv.ssbo_buf_name + "." + load_fn + "(" + pv.ssbo_byte_offset_expr + ")";
  if (ctx.is_float_type(res_tid)) return "asfloat(" + raw + ")";
  if (ctx.is_signed_int_type(res_tid)) return "asint(" + raw + ")";
  return raw;
}

static std::string ssbo_store_stmt(spv_ctx& ctx, const spv_ptr_val& pv, u32 val_id) {
  std::string vexpr = ctx.get_expr(val_id);
  u32 vtid = ctx.expr_types.count(val_id) ? ctx.expr_types[val_id]
             : (ctx.consts.count(val_id) ? ctx.consts[val_id].type_id : pv.pointee_type);
  const u32 n = ctx.scalar_comp_count(vtid);
  std::string st_fn = (n == 4) ? "Store4" : (n == 3 ? "Store3" : (n == 2 ? "Store2" : "Store"));
  return pv.ssbo_buf_name + "." + st_fn + "(" + pv.ssbo_byte_offset_expr +
         ", asuint(" + vexpr + "));\n";
}

static std::string emit_glsl_ext(spv_ctx& ctx, u32 ext_op, const std::vector<u32>& args) {
  auto a = [&](size_t i) -> std::string {
    return (i < args.size()) ? ctx.get_expr(args[i]) : "0.0";
  };
  switch (ext_op) {
    case 4:  return "abs(" + a(0) + ")";       // FAbs
    case 5:  return "abs(" + a(0) + ")";       // SAbs
    case 6:  return "sign(" + a(0) + ")";      // FSign
    case 8:  return "floor(" + a(0) + ")";     // Floor
    case 9:  return "ceil(" + a(0) + ")";      // Ceil
    case 10: return "frac(" + a(0) + ")";      // Fract
    case 13: return "sin(" + a(0) + ")";       // Sin
    case 14: return "cos(" + a(0) + ")";       // Cos
    case 15: return "tan(" + a(0) + ")";       // Tan
    case 16: return "asin(" + a(0) + ")";      // Asin
    case 17: return "acos(" + a(0) + ")";      // Acos
    case 18: return "atan(" + a(0) + ")";      // Atan
    case 25: return "atan2(" + a(0) + ", " + a(1) + ")"; // Atan2
    case 26: return "pow(" + a(0) + ", " + a(1) + ")";   // Pow
    case 27: return "exp(" + a(0) + ")";       // Exp
    case 28: return "log(" + a(0) + ")";       // Log
    case 29: return "exp2(" + a(0) + ")";      // Exp2
    case 30: return "log2(" + a(0) + ")";      // Log2
    case 31: return "sqrt(" + a(0) + ")";      // Sqrt
    case 32: return "rsqrt(" + a(0) + ")";     // InverseSqrt
    case 33: return "determinant(" + a(0) + ")"; // Determinant
    case 37: return "min(" + a(0) + ", " + a(1) + ")";   // FMin
    case 38: return "min(" + a(0) + ", " + a(1) + ")";   // UMin
    case 39: return "min(" + a(0) + ", " + a(1) + ")";   // SMin
    case 40: return "max(" + a(0) + ", " + a(1) + ")";   // FMax
    case 41: return "max(" + a(0) + ", " + a(1) + ")";   // UMax
    case 42: return "max(" + a(0) + ", " + a(1) + ")";   // SMax
    case 43: return "clamp(" + a(0) + ", " + a(1) + ", " + a(2) + ")"; // FClamp
    case 44: return "clamp(" + a(0) + ", " + a(1) + ", " + a(2) + ")"; // UClamp
    case 45: return "clamp(" + a(0) + ", " + a(1) + ", " + a(2) + ")"; // SClamp
    case 46: return "lerp(" + a(0) + ", " + a(1) + ", " + a(2) + ")";  // FMix
    case 48: return "step(" + a(0) + ", " + a(1) + ")";                // Step
    case 49: return "smoothstep(" + a(0) + ", " + a(1) + ", " + a(2) + ")"; // SmoothStep
    case 50: return "mad(" + a(0) + ", " + a(1) + ", " + a(2) + ")";   // Fma
    case 66: return "length(" + a(0) + ")";    // Length
    case 67: return "distance(" + a(0) + ", " + a(1) + ")"; // Distance
    case 68: return "cross(" + a(0) + ", " + a(1) + ")";    // Cross
    case 69: return "normalize(" + a(0) + ")"; // Normalize
    case 71: return "reflect(" + a(0) + ", " + a(1) + ")";  // Reflect
    case 72: return "refract(" + a(0) + ", " + a(1) + ", " + a(2) + ")"; // Refract
    default: return a(0);
  }
}

struct spv_block {
  u32 label_id = 0;
  std::vector<const spv_insn*> body;
  const spv_insn* merge = nullptr;
  const spv_insn* term = nullptr;
};

static std::string emit_block_chain(spv_ctx& ctx, const std::map<u32, spv_block>& blocks,
                                    u32 start_lbl, u32 stop_lbl,
                                    const std::map<std::pair<u32, u32>, std::vector<std::pair<u32, u32>>>& phi_edges,
                                    std::set<u32>& visited, std::string indent);

static void emit_phi_copies(spv_ctx& ctx, u32 from_lbl, u32 to_lbl,
                            const std::map<std::pair<u32, u32>, std::vector<std::pair<u32, u32>>>& phi_edges,
                            std::string& out, const std::string& indent) {
  auto it = phi_edges.find({to_lbl, from_lbl});
  if (it == phi_edges.end()) return;
  for (const auto& kv : it->second) {
    out += indent + ctx.phi_vars[kv.first] + " = " + ctx.get_expr(kv.second) + ";\n";
  }
}

static std::string emit_block_stmts(spv_ctx& ctx, const spv_block& blk,
                                    const std::string& indent) {
  std::string out;
  auto emit_assign = [&](u32 res_id, u32 res_tid, const std::string& rhs) {
    ctx.expr_types[res_id] = res_tid;
    std::string nm = "_t" + std::to_string(res_id);
    out += indent + ctx.hlsl_local_type(res_tid) + " " + nm + " = " + rhs + ";\n";
    ctx.exprs[res_id] = nm;
  };
  for (const spv_insn* ins : blk.body) {
    const auto& w = ins->w;
    switch (ins->op) {
      case OP_VARIABLE: {
        u32 pt = w[0], id = w[1];
        u32 pte = ctx.unwrap_struct_or_pointee(pt);
        std::string nm = ctx.get_name(id, "_loc");
        out += indent + ctx.hlsl_local_type(pte) + " " + nm;
        if (w.size() >= 4) out += " = " + ctx.get_expr(w[3]);
        else out += " = (" + ctx.hlsl_local_type(pte) + ")0";
        out += ";\n";
        ctx.ptrs[id] = {true, id, false, "", "", nm, pte};
        ctx.exprs[id] = nm;
      } break;
      case OP_ACCESS_CHAIN:
      case OP_IN_BOUNDS_ACCESS_CHAIN: {
        u32 pt = w[0], id = w[1], base = w[2];
        u32 pte = ctx.unwrap_struct_or_pointee(pt);
        spv_ptr_val pv = ctx.ptrs.count(base) ? ctx.ptrs[base] : spv_ptr_val{};
        if (!pv.valid && ctx.vars.count(base)) {
          const auto& gv = ctx.vars[base];
          pv.valid = true; pv.base_var = base;
          pv.is_ssbo = gv.is_ssbo;
          pv.ssbo_buf_name = gv.hlsl_name;
          pv.ssbo_byte_offset_expr = "0";
          pv.hlsl_lvalue = gv.hlsl_name;
          pv.pointee_type = gv.pointee_id;
        }
        u32 cur_tid = pv.pointee_type;
        for (size_t i = 3; i < w.size(); ++i) {
          u32 idx_id = w[i];
          const auto& ct = ctx.types[cur_tid];
          if (pv.is_ssbo) {
            std::string add = "0";
            if (ct.kind == spv_type::T_STRUCT) {
              u32 mi = ctx.const_u32(idx_id, 0);
              u32 moff = ctx.decs[cur_tid].members[mi].offset;
              add = std::to_string(moff);
              cur_tid = (mi < ct.members.size()) ? ct.members[mi] : cur_tid;
            } else if (ct.kind == spv_type::T_ARRAY || ct.kind == spv_type::T_RUNTIME_ARRAY) {
              u32 stride = ctx.decs[cur_tid].array_stride;
              if (!stride) stride = ctx.byte_size_of(ct.comp_type);
              add = "(" + ctx.get_expr(idx_id) + " * " + std::to_string(stride) + "u)";
              cur_tid = ct.comp_type;
            } else if (ct.kind == spv_type::T_VEC) {
              add = "(" + ctx.get_expr(idx_id) + " * 4u)";
              cur_tid = ct.comp_type;
            } else if (ct.kind == spv_type::T_MAT) {
              add = "(" + ctx.get_expr(idx_id) + " * 16u)";
              cur_tid = ct.comp_type;
            }
            pv.ssbo_byte_offset_expr = "(" + pv.ssbo_byte_offset_expr + " + " + add + ")";
          } else {
            if (ct.kind == spv_type::T_STRUCT) {
              u32 mi = ctx.const_u32(idx_id, 0);
              const auto& base_var = ctx.vars[pv.base_var];
              if ((base_var.is_ubo || base_var.is_push) && i == 3) {
                pv.hlsl_lvalue = cbuffer_member_expr(ctx, cur_tid, mi);
              } else {
                pv.hlsl_lvalue += "." + cbuffer_member_expr(ctx, cur_tid, mi);
              }
              cur_tid = (mi < ct.members.size()) ? ct.members[mi] : cur_tid;
            } else if (ct.kind == spv_type::T_ARRAY || ct.kind == spv_type::T_VEC || ct.kind == spv_type::T_MAT) {
              pv.hlsl_lvalue += "[" + ctx.get_expr(idx_id) + "]";
              cur_tid = ct.comp_type;
            }
          }
        }
        pv.pointee_type = pte;
        ctx.ptrs[id] = pv;
        ctx.exprs[id] = pv.hlsl_lvalue;
      } break;
      case OP_LOAD: {
        u32 rt = w[0], id = w[1], ptr = w[2];
        if (ctx.ptrs.count(ptr) && ctx.ptrs[ptr].is_ssbo) {
          emit_assign(id, rt, ssbo_load_expr(ctx, ctx.ptrs[ptr], rt));
        } else if (ctx.ptrs.count(ptr)) {
          ctx.exprs[id] = ctx.ptrs[ptr].hlsl_lvalue;
          ctx.expr_types[id] = rt;
        } else if (ctx.vars.count(ptr)) {
          ctx.exprs[id] = ctx.vars[ptr].hlsl_name;
          ctx.expr_types[id] = rt;
        }
      } break;
      case OP_STORE: {
        u32 ptr = w[0], val = w[1];
        if (ctx.ptrs.count(ptr) && ctx.ptrs[ptr].is_ssbo) {
          out += indent + ssbo_store_stmt(ctx, ctx.ptrs[ptr], val);
        } else if (ctx.ptrs.count(ptr)) {
          out += indent + ctx.ptrs[ptr].hlsl_lvalue + " = " + ctx.get_expr(val) + ";\n";
        } else if (ctx.vars.count(ptr)) {
          out += indent + ctx.vars[ptr].hlsl_name + " = " + ctx.get_expr(val) + ";\n";
        }
      } break;
      case OP_COPY_OBJECT:
        ctx.exprs[w[1]] = ctx.get_expr(w[2]);
        ctx.expr_types[w[1]] = w[0];
        break;
      case OP_COMPOSITE_CONSTRUCT: {
        u32 rt = w[0], id = w[1];
        std::string rhs = ctx.hlsl_local_type(rt) + "(";
        for (size_t i = 2; i < w.size(); ++i) {
          if (i > 2) rhs += ", ";
          rhs += ctx.get_expr(w[i]);
        }
        rhs += ")";
        emit_assign(id, rt, rhs);
      } break;
      case OP_COMPOSITE_EXTRACT: {
        u32 rt = w[0], id = w[1], comp = w[2];
        std::string e = ctx.get_expr(comp);
        static const char* k_sw = "xyzw";
        for (size_t i = 3; i < w.size(); ++i) {
          u32 idx = w[i];
          if (idx < 4 && (i == w.size() - 1)) e += std::string(".") + k_sw[idx];
          else e += "[" + std::to_string(idx) + "]";
        }
        ctx.exprs[id] = e;
        ctx.expr_types[id] = rt;
      } break;
      case OP_VECTOR_SHUFFLE: {
        u32 rt = w[0], id = w[1], v1 = w[2], v2 = w[3];
        u32 c1 = ctx.scalar_comp_count(ctx.expr_types.count(v1) ? ctx.expr_types[v1] : rt);
        std::string rhs = ctx.hlsl_local_type(rt) + "(";
        static const char* k_sw = "xyzw";
        for (size_t i = 4; i < w.size(); ++i) {
          if (i > 4) rhs += ", ";
          u32 c = w[i];
          if (c < c1) rhs += ctx.get_expr(v1) + "." + k_sw[c & 3u];
          else rhs += ctx.get_expr(v2) + "." + k_sw[(c - c1) & 3u];
        }
        rhs += ")";
        emit_assign(id, rt, rhs);
      } break;
      case OP_SAMPLED_IMAGE:
        ctx.exprs[w[1]] = ctx.get_expr(w[2]); // carries tex name; smp stored by convention _smp
        ctx.ptrs[w[1]].ssbo_buf_name = ctx.get_expr(w[3]); // sampler name
        break;
      case OP_IMAGE_SAMPLE_IMPLICIT_LOD:
      case OP_IMAGE_SAMPLE_EXPLICIT_LOD: {
        u32 rt = w[0], id = w[1], si = w[2], coord = w[3];
        std::string tex = ctx.get_expr(si);
        std::string smp = (ctx.ptrs.count(si) && !ctx.ptrs[si].ssbo_buf_name.empty())
                              ? ctx.ptrs[si].ssbo_buf_name : (tex + "_smp");
        std::string rhs = (ins->op == OP_IMAGE_SAMPLE_EXPLICIT_LOD && w.size() >= 6)
                              ? (tex + ".SampleLevel(" + smp + ", " + ctx.get_expr(coord) + ", " + ctx.get_expr(w[5]) + ")")
                              : (tex + ".Sample(" + smp + ", " + ctx.get_expr(coord) + ")");
        emit_assign(id, rt, rhs);
      } break;
      case OP_IMAGE_FETCH: {
        u32 rt = w[0], id = w[1], img = w[2], coord = w[3];
        emit_assign(id, rt, ctx.get_expr(img) + ".Load(int3(" + ctx.get_expr(coord) + ", 0))");
      } break;
      case OP_IMAGE_READ: {
        u32 rt = w[0], id = w[1], img = w[2], coord = w[3];
        emit_assign(id, rt, ctx.get_expr(img) + "[" + ctx.get_expr(coord) + "]");
      } break;
      case OP_IMAGE_WRITE:
        out += indent + ctx.get_expr(w[0]) + "[" + ctx.get_expr(w[1]) + "] = " +
               ctx.get_expr(w[2]) + ";\n";
        break;
      case OP_EXT_INST: {
        u32 rt = w[0], id = w[1], eop = w[3];
        std::vector<u32> args(w.begin() + 4, w.end());
        emit_assign(id, rt, emit_glsl_ext(ctx, eop, args));
      } break;
      case OP_F_ADD: case OP_I_ADD:
        emit_assign(w[1], w[0], "(" + ctx.get_expr(w[2]) + " + " + ctx.get_expr(w[3]) + ")"); break;
      case OP_F_SUB: case OP_I_SUB:
        emit_assign(w[1], w[0], "(" + ctx.get_expr(w[2]) + " - " + ctx.get_expr(w[3]) + ")"); break;
      case OP_F_MUL: case OP_I_MUL: case OP_VECTOR_TIMES_SCALAR: case OP_MATRIX_TIMES_SCALAR:
        emit_assign(w[1], w[0], "(" + ctx.get_expr(w[2]) + " * " + ctx.get_expr(w[3]) + ")"); break;
      case OP_F_DIV: case OP_S_DIV: case OP_U_DIV:
        emit_assign(w[1], w[0], "(" + ctx.get_expr(w[2]) + " / " + ctx.get_expr(w[3]) + ")"); break;
      case OP_S_NEGATE: case OP_F_NEGATE:
        emit_assign(w[1], w[0], "(-" + ctx.get_expr(w[2]) + ")"); break;
      case OP_MATRIX_TIMES_VECTOR:
      case OP_VECTOR_TIMES_MATRIX:
      case OP_MATRIX_TIMES_MATRIX:
        emit_assign(w[1], w[0], "mul(" + ctx.get_expr(w[2]) + ", " + ctx.get_expr(w[3]) + ")"); break;
      case OP_DOT:
        emit_assign(w[1], w[0], "dot(" + ctx.get_expr(w[2]) + ", " + ctx.get_expr(w[3]) + ")"); break;
      case OP_TRANSPOSE:
        emit_assign(w[1], w[0], "transpose(" + ctx.get_expr(w[2]) + ")"); break;
      case OP_SELECT:
        emit_assign(w[1], w[0], "(" + ctx.get_expr(w[2]) + " ? " + ctx.get_expr(w[3]) + " : " + ctx.get_expr(w[4]) + ")"); break;
      case OP_CONVERT_F_TO_U: case OP_CONVERT_F_TO_S: case OP_CONVERT_S_TO_F:
      case OP_CONVERT_U_TO_F: case OP_U_CONVERT: case OP_S_CONVERT: case OP_F_CONVERT:
        emit_assign(w[1], w[0], "((" + ctx.hlsl_local_type(w[0]) + ")" + ctx.get_expr(w[2]) + ")"); break;
      case OP_BITCAST: {
        std::string fn = ctx.is_float_type(w[0]) ? "asfloat" : (ctx.is_signed_int_type(w[0]) ? "asint" : "asuint");
        emit_assign(w[1], w[0], fn + "(" + ctx.get_expr(w[2]) + ")");
      } break;
      case OP_I_EQUAL: case OP_F_ORD_EQUAL: case OP_LOGICAL_EQUAL:
        emit_assign(w[1], w[0], "(" + ctx.get_expr(w[2]) + " == " + ctx.get_expr(w[3]) + ")"); break;
      case OP_I_NOT_EQUAL: case OP_F_ORD_NOT_EQUAL: case OP_LOGICAL_NOT_EQUAL:
        emit_assign(w[1], w[0], "(" + ctx.get_expr(w[2]) + " != " + ctx.get_expr(w[3]) + ")"); break;
      case OP_U_LESS_THAN: case OP_S_LESS_THAN: case OP_F_ORD_LESS_THAN:
        emit_assign(w[1], w[0], "(" + ctx.get_expr(w[2]) + " < " + ctx.get_expr(w[3]) + ")"); break;
      case OP_U_GREATER_THAN: case OP_S_GREATER_THAN: case OP_F_ORD_GREATER_THAN:
        emit_assign(w[1], w[0], "(" + ctx.get_expr(w[2]) + " > " + ctx.get_expr(w[3]) + ")"); break;
      case OP_U_LESS_THAN_EQUAL: case OP_S_LESS_THAN_EQUAL: case OP_F_ORD_LESS_THAN_EQUAL:
        emit_assign(w[1], w[0], "(" + ctx.get_expr(w[2]) + " <= " + ctx.get_expr(w[3]) + ")"); break;
      case OP_U_GREATER_THAN_EQUAL: case OP_S_GREATER_THAN_EQUAL: case OP_F_ORD_GREATER_THAN_EQUAL:
        emit_assign(w[1], w[0], "(" + ctx.get_expr(w[2]) + " >= " + ctx.get_expr(w[3]) + ")"); break;
      case OP_LOGICAL_AND:
        emit_assign(w[1], w[0], "(" + ctx.get_expr(w[2]) + " && " + ctx.get_expr(w[3]) + ")"); break;
      case OP_LOGICAL_OR:
        emit_assign(w[1], w[0], "(" + ctx.get_expr(w[2]) + " || " + ctx.get_expr(w[3]) + ")"); break;
      case OP_LOGICAL_NOT:
        emit_assign(w[1], w[0], "(!" + ctx.get_expr(w[2]) + ")"); break;
      case OP_BITWISE_AND:
        emit_assign(w[1], w[0], "(" + ctx.get_expr(w[2]) + " & " + ctx.get_expr(w[3]) + ")"); break;
      case OP_BITWISE_OR:
        emit_assign(w[1], w[0], "(" + ctx.get_expr(w[2]) + " | " + ctx.get_expr(w[3]) + ")"); break;
      case OP_BITWISE_XOR:
        emit_assign(w[1], w[0], "(" + ctx.get_expr(w[2]) + " ^ " + ctx.get_expr(w[3]) + ")"); break;
      case OP_SHIFT_LEFT_LOGICAL:
        emit_assign(w[1], w[0], "(" + ctx.get_expr(w[2]) + " << " + ctx.get_expr(w[3]) + ")"); break;
      case OP_SHIFT_RIGHT_LOGICAL: case OP_SHIFT_RIGHT_ARITHMETIC:
        emit_assign(w[1], w[0], "(" + ctx.get_expr(w[2]) + " >> " + ctx.get_expr(w[3]) + ")"); break;
      case OP_DPDX: emit_assign(w[1], w[0], "ddx(" + ctx.get_expr(w[2]) + ")"); break;
      case OP_DPDY: emit_assign(w[1], w[0], "ddy(" + ctx.get_expr(w[2]) + ")"); break;
      case OP_CONTROL_BARRIER:
        out += indent + "GroupMemoryBarrierWithGroupSync();\n"; break;
      default: break;
    }
  }
  return out;
}

static std::string emit_block_chain(
    spv_ctx& ctx, const std::map<u32, spv_block>& blocks, u32 start_lbl, u32 stop_lbl,
    const std::map<std::pair<u32, u32>, std::vector<std::pair<u32, u32>>>& phi_edges,
    std::set<u32>& visited, std::string indent) {
  std::string out;
  u32 cur = start_lbl;
  for (int guard = 0; guard < 1024; ++guard) {
    if (!cur || cur == stop_lbl || visited.count(cur)) break;
    auto it = blocks.find(cur);
    if (it == blocks.end()) break;
    visited.insert(cur);
    const spv_block& blk = it->second;
    out += emit_block_stmts(ctx, blk, indent);
    if (!blk.term) break;
    if (blk.term->op == OP_RETURN || blk.term->op == OP_RETURN_VALUE) {
      break;
    }
    if (blk.term->op == OP_KILL || blk.term->op == OP_TERMINATE_INVOCATION) {
      out += indent + "discard;\n";
      break;
    }
    if (blk.term->op == OP_BRANCH) {
      u32 tgt = blk.term->w[0];
      emit_phi_copies(ctx, cur, tgt, phi_edges, out, indent);
      if (tgt == stop_lbl) break;
      cur = tgt;
      continue;
    }
    if (blk.term->op == OP_BRANCH_CONDITIONAL) {
      u32 cond_id = blk.term->w[0], t_lbl = blk.term->w[1], f_lbl = blk.term->w[2];
      if (blk.merge && blk.merge->op == OP_LOOP_MERGE) {
        u32 l_merge = blk.merge->w[0];
        out += indent + "for (uint _iter = 0; _iter < 65536u; ++_iter) {\n";
        std::string b_indent = indent + "  ";
        if (f_lbl == l_merge) {
          out += b_indent + "if (!(" + ctx.get_expr(cond_id) + ")) break;\n";
          std::set<u32> loop_vis;
          out += emit_block_chain(ctx, blocks, t_lbl, cur, phi_edges, loop_vis, b_indent);
        } else {
          out += b_indent + "if (" + ctx.get_expr(cond_id) + ") break;\n";
          std::set<u32> loop_vis;
          out += emit_block_chain(ctx, blocks, f_lbl, cur, phi_edges, loop_vis, b_indent);
        }
        out += indent + "}\n";
        cur = l_merge;
        continue;
      }
      u32 merge_lbl = (blk.merge && blk.merge->op == OP_SELECTION_MERGE) ? blk.merge->w[0] : stop_lbl;
      out += indent + "if (" + ctx.get_expr(cond_id) + ") {\n";
      emit_phi_copies(ctx, cur, t_lbl, phi_edges, out, indent + "  ");
      if (t_lbl != merge_lbl) {
        out += emit_block_chain(ctx, blocks, t_lbl, merge_lbl, phi_edges, visited, indent + "  ");
      }
      if (f_lbl != merge_lbl) {
        out += indent + "} else {\n";
        emit_phi_copies(ctx, cur, f_lbl, phi_edges, out, indent + "  ");
        out += emit_block_chain(ctx, blocks, f_lbl, merge_lbl, phi_edges, visited, indent + "  ");
      }
      out += indent + "}\n";
      cur = merge_lbl;
      continue;
    }
    if (blk.term->op == OP_SWITCH && blk.term->w.size() >= 2) {
      u32 sel_id = blk.term->w[0], def_lbl = blk.term->w[1];
      u32 merge_lbl = (blk.merge && blk.merge->op == OP_SELECTION_MERGE) ? blk.merge->w[0] : stop_lbl;
      out += indent + "switch (" + ctx.get_expr(sel_id) + ") {\n";
      for (size_t k = 2; k + 1 < blk.term->w.size(); k += 2) {
        u32 lit = blk.term->w[k], clbl = blk.term->w[k + 1];
        out += indent + "  case " + std::to_string((int32_t)lit) + ": {\n";
        emit_phi_copies(ctx, cur, clbl, phi_edges, out, indent + "    ");
        if (clbl != merge_lbl) {
          out += emit_block_chain(ctx, blocks, clbl, merge_lbl, phi_edges, visited, indent + "    ");
        }
        out += indent + "  } break;\n";
      }
      if (def_lbl != merge_lbl) {
        out += indent + "  default: {\n";
        emit_phi_copies(ctx, cur, def_lbl, phi_edges, out, indent + "    ");
        out += emit_block_chain(ctx, blocks, def_lbl, merge_lbl, phi_edges, visited, indent + "    ");
        out += indent + "  } break;\n";
      }
      out += indent + "}\n";
      cur = merge_lbl;
      continue;
    }
    break;
  }
  return out;
}

}  // namespace

spirv_compile_result spirv_to_hlsl(const u32* words, u32 word_count,
                                   spirv_stage stage_filter,
                                   const char* entry_name,
                                   const spirv_spec_entry* spec_entries,
                                   u32 spec_count) {
  spirv_compile_result err_res;
  if (!words || word_count < 5) {
    err_res.log = "SPIR-V buffer too small";
    return err_res;
  }
  if (words[0] != 0x07230203u) {
    err_res.log = "SPIR-V invalid magic";
    return err_res;
  }
  if (words[3] == 0 || words[3] > 2000000u) {
    err_res.log = "SPIR-V invalid ID bound";
    return err_res;
  }

  spv_ctx ctx;
  ctx.bound = words[3];
  for (u32 i = 0; i < spec_count; ++i) {
    ctx.spec_overrides[spec_entries[i].spec_id] = spec_entries[i];
  }

  // Decode instruction stream
  u32 pos = 5;
  while (pos < word_count) {
    u32 hdr = words[pos];
    u16 cnt = (u16)(hdr >> 16);
    u16 op = (u16)(hdr & 0xFFFFu);
    if (cnt == 0 || pos + cnt > word_count) {
      err_res.log = "SPIR-V truncated instruction";
      return err_res;
    }
    spv_insn ins;
    ins.op = op;
    for (u16 k = 1; k < cnt; ++k) ins.w.push_back(words[pos + k]);
    ctx.insns.push_back(std::move(ins));
    pos += cnt;
  }

  // Pass 1: metadata, types, constants, decorations, globals
  for (const auto& ins : ctx.insns) {
    const auto& w = ins.w;
    switch (ins.op) {
      case OP_NAME:
        if (w.size() >= 2) ctx.names[w[0]] = ctx.read_str(w, 1);
        break;
      case OP_MEMBER_NAME:
        if (w.size() >= 3) ctx.member_names[{w[0], w[1]}] = ctx.read_str(w, 2);
        break;
      case OP_EXT_INST_IMPORT:
        if (w.size() >= 2 && ctx.read_str(w, 1) == "GLSL.std.450") ctx.glsl_ext_id = w[0];
        break;
      case OP_ENTRY_POINT:
        if (w.size() >= 3) {
          spv_ctx::ep_info ep;
          ep.stage = (spirv_stage)w[0];
          ep.func_id = w[1];
          size_t ni = 2;
          ep.name = ctx.read_str(w, 2, &ni);
          for (size_t k = ni; k < w.size(); ++k) ep.iface_ids.push_back(w[k]);
          ctx.eps.push_back(std::move(ep));
        }
        break;
      case OP_EXECUTION_MODE:
        if (w.size() >= 5 && w[1] == 17 /*LocalSize*/) {
          ctx.local_size[0] = std::max(1u, w[2]);
          ctx.local_size[1] = std::max(1u, w[3]);
          ctx.local_size[2] = std::max(1u, w[4]);
        }
        break;
      case OP_DECORATE:
        if (w.size() >= 2) {
          auto& d = ctx.decs[w[0]];
          d.flags.insert(w[1]);
          if (w[1] == DEC_BUILTIN && w.size() >= 3) d.builtin = w[2];
          else if (w[1] == DEC_LOCATION && w.size() >= 3) d.location = (int)w[2];
          else if (w[1] == DEC_BINDING && w.size() >= 3) d.binding = (int)w[2];
          else if (w[1] == DEC_DESCRIPTOR_SET && w.size() >= 3) d.set = (int)w[2];
          else if (w[1] == DEC_ARRAY_STRIDE && w.size() >= 3) d.array_stride = w[2];
          else if (w[1] == DEC_SPEC_ID && w.size() >= 3) d.spec_id = (int)w[2];
        }
        break;
      case OP_MEMBER_DECORATE:
        if (w.size() >= 3) {
          auto& md = ctx.decs[w[0]].members[w[1]];
          if (w[2] == DEC_OFFSET && w.size() >= 4) { md.offset = w[3]; md.has_offset = true; }
          else if (w[2] == DEC_BUILTIN && w.size() >= 4) md.builtin = w[3];
          else if (w[2] == DEC_LOCATION && w.size() >= 4) md.location = (int)w[3];
        }
        break;
      case OP_TYPE_VOID: ctx.types[w[0]].kind = spv_type::T_VOID; break;
      case OP_TYPE_BOOL: ctx.types[w[0]].kind = spv_type::T_BOOL; break;
      case OP_TYPE_INT:
        ctx.types[w[0]] = {spv_type::T_INT, w[1], w[2] != 0}; break;
      case OP_TYPE_FLOAT:
        ctx.types[w[0]] = {spv_type::T_FLOAT, w[1], true}; break;
      case OP_TYPE_VECTOR: {
        spv_type t; t.kind = spv_type::T_VEC; t.comp_type = w[1]; t.count = w[2];
        ctx.types[w[0]] = t;
      } break;
      case OP_TYPE_MATRIX: {
        spv_type t; t.kind = spv_type::T_MAT; t.comp_type = w[1]; t.count = w[2];
        ctx.types[w[0]] = t;
      } break;
      case OP_TYPE_IMAGE: {
        spv_type t; t.kind = spv_type::T_IMAGE; t.comp_type = w[1];
        t.dim = w[2]; t.arrayed = (w[4] != 0); t.sampled = w[6];
        ctx.types[w[0]] = t;
      } break;
      case OP_TYPE_SAMPLER: ctx.types[w[0]].kind = spv_type::T_SAMPLER; break;
      case OP_TYPE_SAMPLED_IMAGE: {
        spv_type t; t.kind = spv_type::T_SAMPLED_IMAGE; t.comp_type = w[1];
        ctx.types[w[0]] = t;
      } break;
      case OP_TYPE_ARRAY: {
        spv_type t; t.kind = spv_type::T_ARRAY; t.comp_type = w[1]; t.len_const_id = w[2];
        ctx.types[w[0]] = t;
      } break;
      case OP_TYPE_RUNTIME_ARRAY: {
        spv_type t; t.kind = spv_type::T_RUNTIME_ARRAY; t.comp_type = w[1];
        ctx.types[w[0]] = t;
      } break;
      case OP_TYPE_STRUCT: {
        spv_type t; t.kind = spv_type::T_STRUCT;
        for (size_t i = 1; i < w.size(); ++i) t.members.push_back(w[i]);
        ctx.types[w[0]] = t;
      } break;
      case OP_TYPE_POINTER: {
        spv_type t; t.kind = spv_type::T_POINTER; t.storage_class = w[1]; t.comp_type = w[2];
        ctx.types[w[0]] = t;
      } break;
      case OP_CONSTANT_TRUE:
      case OP_SPEC_CONSTANT_TRUE: {
        spv_const_val c{true, true, true, w[0], {1}, {}};
        int sid = ctx.decs[w[1]].spec_id;
        if (sid >= 0 && ctx.spec_overrides.count((u32)sid)) c.b = (ctx.spec_overrides[(u32)sid].value_u32 != 0);
        ctx.consts[w[1]] = c;
      } break;
      case OP_CONSTANT_FALSE:
      case OP_SPEC_CONSTANT_FALSE: {
        spv_const_val c{true, true, false, w[0], {0}, {}};
        int sid = ctx.decs[w[1]].spec_id;
        if (sid >= 0 && ctx.spec_overrides.count((u32)sid)) c.b = (ctx.spec_overrides[(u32)sid].value_u32 != 0);
        ctx.consts[w[1]] = c;
      } break;
      case OP_CONSTANT:
      case OP_SPEC_CONSTANT: {
        spv_const_val c; c.is_const = true; c.type_id = w[0];
        for (size_t i = 2; i < w.size(); ++i) c.raw_words.push_back(w[i]);
        int sid = ctx.decs[w[1]].spec_id;
        if (sid >= 0 && ctx.spec_overrides.count((u32)sid) && !c.raw_words.empty()) {
          c.raw_words[0] = ctx.spec_overrides[(u32)sid].value_u32;
        }
        ctx.consts[w[1]] = std::move(c);
      } break;
      case OP_CONSTANT_COMPOSITE: {
        spv_const_val c; c.is_const = true; c.type_id = w[0];
        for (size_t i = 2; i < w.size(); ++i) c.comp_ids.push_back(w[i]);
        ctx.consts[w[1]] = std::move(c);
      } break;
      case OP_CONSTANT_NULL:
      case OP_UNDEF: {
        spv_const_val c; c.is_const = true; c.type_id = w[0];
        ctx.consts[w[1]] = c;
      } break;
      default: break;
    }
  }

  // Pick entry point
  bool ep_found = false;
  for (const auto& ep : ctx.eps) {
    if (stage_filter != SPV_STAGE_ANY && ep.stage != stage_filter) continue;
    if (entry_name && entry_name[0] && ep.name != entry_name) continue;
    ctx.active_ep = ep;
    ep_found = true;
    break;
  }
  if (!ep_found && !ctx.eps.empty()) {
    ctx.active_ep = ctx.eps[0];
    ep_found = true;
  }
  if (!ep_found) {
    err_res.log = "SPIR-V no matching OpEntryPoint";
    return err_res;
  }

  ctx.res.stage = ctx.active_ep.stage;
  ctx.res.entry_name = "main";
  memcpy(ctx.res.local_size, ctx.local_size, sizeof(ctx.local_size));

  // Classify global OpVariables (only top-level ones outside functions)
  std::string vs_in_struct, vs_out_struct;
  std::vector<std::string> vs_in_copies, vs_out_copies, cs_in_copies;
  bool in_func = false;
  for (const auto& ins : ctx.insns) {
    if (ins.op == OP_FUNCTION) { in_func = true; continue; }
    if (ins.op == OP_FUNCTION_END) { in_func = false; continue; }
    if (in_func || ins.op != OP_VARIABLE) continue;
    const auto& w = ins.w;
    u32 pt = w[0], id = w[1], sc = w[2];
    u32 pte = ctx.unwrap_struct_or_pointee(pt);
    const auto& dec = ctx.decs[id];
    const auto& pty = ctx.types[pte];

    spv_var gv;
    gv.type_id = pt; gv.pointee_id = pte; gv.storage_class = sc;
    gv.hlsl_name = ctx.get_name(id, "_g");

    if (sc == SC_UNIFORM_CONSTANT) {
      u32 set = (dec.set >= 0) ? (u32)dec.set : 0u;
      u32 bind = (dec.binding >= 0) ? (u32)dec.binding : 0u;
      if (pty.kind == spv_type::T_SAMPLED_IMAGE) {
        u32 tr = ctx.next_t++, sr = ctx.next_s++;
        u32 img_tid = pty.comp_type;
        std::string itype = ctx.hlsl_type(img_tid);
        char dbuf[256];
        snprintf(dbuf, sizeof(dbuf), "%s %s : register(t%u);\nSamplerState %s_smp : register(s%u);\n",
                 itype.c_str(), gv.hlsl_name.c_str(), tr, gv.hlsl_name.c_str(), sr);
        ctx.decls_hlsl += dbuf;
        gv.hlsl_reg = tr; gv.smp_reg = sr;
        ctx.res.bindings.push_back({set, bind, SPV_BIND_COMBINED_IMG_SMP, tr, sr, 0, gv.hlsl_name});
      } else if (pty.kind == spv_type::T_IMAGE && pty.sampled == 2) {
        u32 ur = ctx.next_u++;
        char dbuf[256];
        snprintf(dbuf, sizeof(dbuf), "%s %s : register(u%u);\n",
                 ctx.hlsl_type(pte).c_str(), gv.hlsl_name.c_str(), ur);
        ctx.decls_hlsl += dbuf;
        gv.hlsl_reg = ur;
        ctx.res.bindings.push_back({set, bind, SPV_BIND_STORAGE_IMG, ur, 0, 0, gv.hlsl_name});
      } else if (pty.kind == spv_type::T_IMAGE) {
        u32 tr = ctx.next_t++;
        char dbuf[256];
        snprintf(dbuf, sizeof(dbuf), "%s %s : register(t%u);\n",
                 ctx.hlsl_type(pte).c_str(), gv.hlsl_name.c_str(), tr);
        ctx.decls_hlsl += dbuf;
        gv.hlsl_reg = tr;
        ctx.res.bindings.push_back({set, bind, SPV_BIND_TEXTURE, tr, 0, 0, gv.hlsl_name});
      } else if (pty.kind == spv_type::T_SAMPLER) {
        u32 sr = ctx.next_s++;
        char dbuf[256];
        snprintf(dbuf, sizeof(dbuf), "SamplerState %s : register(s%u);\n",
                 gv.hlsl_name.c_str(), sr);
        ctx.decls_hlsl += dbuf;
        gv.hlsl_reg = sr;
        ctx.res.bindings.push_back({set, bind, SPV_BIND_SAMPLER, sr, 0, 0, gv.hlsl_name});
      }
    } else if (sc == SC_PUSH_CONSTANT) {
      gv.is_push = true;
      gv.hlsl_reg = ctx.res.push_constant_reg;
      u32 bsz = ctx.byte_size_of(pte);
      ctx.res.push_constant_bytes = std::max(ctx.res.push_constant_bytes, bsz);
      std::string body;
      emit_cbuffer_fields(ctx, pte, body);
      char hdr[128];
      snprintf(hdr, sizeof(hdr), "cbuffer %s_cb : register(b%u) {\n",
               gv.hlsl_name.c_str(), gv.hlsl_reg);
      ctx.decls_hlsl += hdr + body + "};\n";
    } else if (sc == SC_UNIFORM || sc == SC_STORAGE_BUFFER) {
      const auto& pdec = ctx.decs[pte];
      const bool is_ssbo = (sc == SC_STORAGE_BUFFER) || pdec.flags.count(DEC_BUFFER_BLOCK) != 0;
      u32 set = (dec.set >= 0) ? (u32)dec.set : 0u;
      u32 bind = (dec.binding >= 0) ? (u32)dec.binding : 0u;
      if (is_ssbo) {
        gv.is_ssbo = true;
        const bool rw = (ctx.active_ep.stage == SPV_STAGE_COMPUTE) && !dec.flags.count(DEC_NON_WRITABLE);
        if (rw) {
          u32 ur = ctx.next_u++;
          char dbuf[256];
          snprintf(dbuf, sizeof(dbuf), "RWByteAddressBuffer %s : register(u%u);\n",
                   gv.hlsl_name.c_str(), ur);
          ctx.decls_hlsl += dbuf;
          gv.hlsl_reg = ur;
          ctx.res.bindings.push_back({set, bind, SPV_BIND_UAV_BUFFER, ur, 0, 0, gv.hlsl_name});
        } else {
          u32 tr = ctx.next_t++;
          char dbuf[256];
          snprintf(dbuf, sizeof(dbuf), "ByteAddressBuffer %s : register(t%u);\n",
                   gv.hlsl_name.c_str(), tr);
          ctx.decls_hlsl += dbuf;
          gv.hlsl_reg = tr;
          ctx.res.bindings.push_back({set, bind, SPV_BIND_SRV_BUFFER, tr, 0, 0, gv.hlsl_name});
        }
      } else {
        gv.is_ubo = true;
        u32 br = ctx.next_b++;
        if (br == ctx.res.push_constant_reg) br = ctx.next_b++;
        gv.hlsl_reg = br;
        u32 bsz = ctx.byte_size_of(pte);
        std::string body;
        emit_cbuffer_fields(ctx, pte, body);
        char hdr[128];
        snprintf(hdr, sizeof(hdr), "cbuffer %s_cb : register(b%u) {\n",
                 gv.hlsl_name.c_str(), br);
        ctx.decls_hlsl += hdr + body + "};\n";
        ctx.res.bindings.push_back({set, bind, SPV_BIND_CBV, br, 0, bsz, gv.hlsl_name});
      }
    } else if (sc == SC_WORKGROUP) {
      ctx.decls_hlsl += "groupshared " + ctx.hlsl_local_type(pte) + " " + gv.hlsl_name + ";\n";
    } else if (sc == SC_PRIVATE) {
      ctx.decls_hlsl += "static " + ctx.hlsl_local_type(pte) + " " + gv.hlsl_name + ";\n";
    } else if (sc == SC_INPUT) {
      u32 bi = 0xFFFFFFFFu;
      bool is_bi = (dec.builtin != 0xFFFFFFFFu) || has_builtin_in_type(ctx, pte, bi);
      if (dec.builtin != 0xFFFFFFFFu) bi = dec.builtin;
      if (is_bi) {
        if (ctx.active_ep.stage == SPV_STAGE_COMPUTE) {
          std::string sem = builtin_semantic(bi, SPV_STAGE_COMPUTE, false);
          std::string fty = (bi == BI_LOCAL_INVOCATION_INDEX) ? "uint" : "uint3";
          vs_in_struct += "  " + fty + " " + gv.hlsl_name + " : " + sem + ";\n";
          ctx.decls_hlsl += "static " + fty + " " + gv.hlsl_name + ";\n";
          cs_in_copies.push_back("  " + gv.hlsl_name + " = _in." + gv.hlsl_name + ";\n");
        } else {
          std::string sem = builtin_semantic(bi, ctx.active_ep.stage, false);
          std::string hty = (bi == BI_VERTEX_INDEX || bi == BI_INSTANCE_INDEX) ? "uint"
                          : (bi == BI_FRONT_FACING ? "bool" : "float4");
          vs_in_struct += "  " + hty + " " + gv.hlsl_name + " : " + sem + ";\n";
          ctx.decls_hlsl += "static " + hty + " " + gv.hlsl_name + ";\n";
          vs_in_copies.push_back("  " + gv.hlsl_name + " = _in." + gv.hlsl_name + ";\n");
          ctx.res.inputs.push_back({0, bi, gv.hlsl_name, hty});
        }
      } else {
        u32 loc = (dec.location >= 0) ? (u32)dec.location : 0u;
        std::string hty = ctx.hlsl_local_type(pte);
        std::string sem = "TEXCOORD" + std::to_string(loc);
        vs_in_struct += "  " + hty + " " + gv.hlsl_name + " : " + sem + ";\n";
        ctx.decls_hlsl += "static " + hty + " " + gv.hlsl_name + ";\n";
        vs_in_copies.push_back("  " + gv.hlsl_name + " = _in." + gv.hlsl_name + ";\n");
        ctx.res.inputs.push_back({loc, 0xFFFFFFFFu, gv.hlsl_name, hty});
      }
    } else if (sc == SC_OUTPUT) {
      u32 bi = 0xFFFFFFFFu;
      bool is_bi = (dec.builtin != 0xFFFFFFFFu) || has_builtin_in_type(ctx, pte, bi);
      if (dec.builtin != 0xFFFFFFFFu) bi = dec.builtin;
      if (is_bi && pty.kind == spv_type::T_STRUCT) {
        // Per-vertex gl_PerVertex block -> promote members to globals and copy Position to SV_Position
        std::string sdef = "struct " + ctx.hlsl_local_type(pte) + " {\n";
        for (u32 mi = 0; mi < (u32)pty.members.size(); ++mi) {
          sdef += "  " + ctx.hlsl_local_type(pty.members[mi]) + " " +
                  cbuffer_member_expr(ctx, pte, mi) + ";\n";
        }
        sdef += "};\n";
        ctx.structs_hlsl += sdef;
        ctx.decls_hlsl += "static " + ctx.hlsl_local_type(pte) + " " + gv.hlsl_name + ";\n";
        std::string pos_field = cbuffer_member_expr(ctx, pte, 0);
        vs_out_struct += "  float4 gl_Position : SV_Position;\n";
        vs_out_copies.push_back("  _out.gl_Position = " + gv.hlsl_name + "." + pos_field + ";\n");
        vs_out_copies.push_back("  _out.gl_Position.y = -_out.gl_Position.y;\n");
        ctx.res.outputs.push_back({0, BI_POSITION, gv.hlsl_name, "float4"});
      } else if (is_bi) {
        std::string sem = builtin_semantic(bi, ctx.active_ep.stage, true);
        std::string hty = (bi == BI_FRAG_DEPTH) ? "float" : "float4";
        vs_out_struct += "  " + hty + " " + gv.hlsl_name + " : " + sem + ";\n";
        ctx.decls_hlsl += "static " + hty + " " + gv.hlsl_name + ";\n";
        vs_out_copies.push_back("  _out." + gv.hlsl_name + " = " + gv.hlsl_name + ";\n");
        if (bi == BI_POSITION) {
          vs_out_copies.push_back("  _out." + gv.hlsl_name + ".y = -_out." + gv.hlsl_name + ".y;\n");
        }
        ctx.res.outputs.push_back({0, bi, gv.hlsl_name, hty});
      } else {
        u32 loc = (dec.location >= 0) ? (u32)dec.location : 0u;
        std::string hty = ctx.hlsl_local_type(pte);
        std::string sem = (ctx.active_ep.stage == SPV_STAGE_FRAGMENT)
                              ? ("SV_Target" + std::to_string(loc))
                              : ("TEXCOORD" + std::to_string(loc));
        vs_out_struct += "  " + hty + " " + gv.hlsl_name + " : " + sem + ";\n";
        ctx.decls_hlsl += "static " + hty + " " + gv.hlsl_name + ";\n";
        vs_out_copies.push_back("  _out." + gv.hlsl_name + " = " + gv.hlsl_name + ";\n");
        ctx.res.outputs.push_back({loc, 0xFFFFFFFFu, gv.hlsl_name, hty});
      }
    }
    ctx.vars[id] = std::move(gv);
  }

  // Emit non-block user structs referenced by non-interface variables if needed
  for (const auto& kv : ctx.types) {
    if (kv.second.kind != spv_type::T_STRUCT) continue;
    const auto& d = ctx.decs[kv.first];
    if (d.flags.count(DEC_BLOCK) || d.flags.count(DEC_BUFFER_BLOCK)) continue;
    u32 bi = 0xFFFFFFFFu;
    if (has_builtin_in_type(ctx, kv.first, bi)) continue;
    std::string sdef = "struct " + ctx.get_name(kv.first, "_Struct") + " {\n";
    for (u32 mi = 0; mi < (u32)kv.second.members.size(); ++mi) {
      sdef += "  " + ctx.hlsl_local_type(kv.second.members[mi]) + " " +
              cbuffer_member_expr(ctx, kv.first, mi) + ";\n";
    }
    sdef += "};\n";
    ctx.structs_hlsl += sdef;
  }

  // Pass 2: compile active entry point function
  std::map<u32, spv_block> blocks;
  std::map<std::pair<u32, u32>, std::vector<std::pair<u32, u32>>> phi_edges;
  u32 entry_lbl = 0;
  bool in_target_fn = false;
  u32 cur_lbl = 0;
  const spv_insn* pending_merge = nullptr;
  for (const auto& ins : ctx.insns) {
    if (ins.op == OP_FUNCTION) {
      in_target_fn = (!ins.w.empty() && ins.w.size() >= 2 && ins.w[1] == ctx.active_ep.func_id);
      continue;
    }
    if (!in_target_fn) continue;
    if (ins.op == OP_FUNCTION_END) { in_target_fn = false; continue; }
    if (ins.op == OP_LABEL) {
      cur_lbl = ins.w[0];
      if (!entry_lbl) entry_lbl = cur_lbl;
      blocks[cur_lbl].label_id = cur_lbl;
      pending_merge = nullptr;
      continue;
    }
    if (!cur_lbl) continue;
    if (ins.op == OP_PHI) {
      u32 pt = ins.w[0], pid = ins.w[1];
      std::string pv = "_phi_" + std::to_string(pid);
      ctx.phi_vars[pid] = pv;
      ctx.main_body_hlsl += "  " + ctx.hlsl_local_type(pt) + " " + pv + " = (" +
                            ctx.hlsl_local_type(pt) + ")0;\n";
      for (size_t k = 2; k + 1 < ins.w.size(); k += 2) {
        phi_edges[{cur_lbl, ins.w[k + 1]}].push_back({pid, ins.w[k]});
      }
      continue;
    }
    if (ins.op == OP_SELECTION_MERGE || ins.op == OP_LOOP_MERGE) {
      pending_merge = &ins;
      continue;
    }
    if (ins.op == OP_BRANCH || ins.op == OP_BRANCH_CONDITIONAL || ins.op == OP_SWITCH ||
        ins.op == OP_RETURN || ins.op == OP_RETURN_VALUE ||
        ins.op == OP_KILL || ins.op == OP_TERMINATE_INVOCATION) {
      blocks[cur_lbl].merge = pending_merge;
      blocks[cur_lbl].term = &ins;
      pending_merge = nullptr;
      continue;
    }
    blocks[cur_lbl].body.push_back(&ins);
  }

  std::set<u32> visited;
  std::string emitted_body = emit_block_chain(ctx, blocks, entry_lbl, 0, phi_edges, visited, "  ");

  // Assemble complete HLSL SM5 shader
  std::string hlsl = "// Generated by Tiramisu SPIR-V -> HLSL SM5\n";
  hlsl += ctx.structs_hlsl;
  hlsl += ctx.decls_hlsl;

  if (ctx.active_ep.stage == SPV_STAGE_COMPUTE) {
    if (!vs_in_struct.empty()) {
      hlsl += "struct CSIn {\n" + vs_in_struct + "};\n";
    }
    char nt[96];
    snprintf(nt, sizeof(nt), "\n[numthreads(%u, %u, %u)]\n",
             ctx.local_size[0], ctx.local_size[1], ctx.local_size[2]);
    hlsl += nt;
    hlsl += vs_in_struct.empty() ? "void main() {\n" : "void main(CSIn _in) {\n";
    for (const auto& s : cs_in_copies) hlsl += s;
    hlsl += ctx.main_body_hlsl;
    hlsl += emitted_body;
    hlsl += "}\n";
  } else {
    if (!vs_in_struct.empty()) hlsl += "struct VSIn {\n" + vs_in_struct + "};\n";
    if (!vs_out_struct.empty()) hlsl += "struct VSOut {\n" + vs_out_struct + "};\n";
    hlsl += vs_out_struct.empty() ? "\nvoid main(" : "\nVSOut main(";
    hlsl += vs_in_struct.empty() ? ") {\n" : "VSIn _in) {\n";
    if (!vs_out_struct.empty()) hlsl += "  VSOut _out = (VSOut)0;\n";
    for (const auto& s : vs_in_copies) hlsl += s;
    hlsl += ctx.main_body_hlsl;
    hlsl += emitted_body;
    for (const auto& s : vs_out_copies) hlsl += s;
    if (!vs_out_struct.empty()) hlsl += "  return _out;\n";
    hlsl += "}\n";
  }

  ctx.res.hlsl = std::move(hlsl);
  ctx.res.ok = true;
  ctx.res.log = "SPIR-V -> HLSL SM5 ok";
  return ctx.res;
}
