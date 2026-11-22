// tiramisu/src/trt/spirv_hlsl.cpp
// Binary SPIR-V (1.0..1.6) + GLSL.std.450 -> HLSL SM5 compiler.
#include "spirv_hlsl.h"

#include <algorithm>
#include <cmath>
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
  OP_SPEC_CONSTANT_COMPOSITE = 51,
  OP_SPEC_CONSTANT_OP = 52,
  OP_FUNCTION = 54,
  OP_FUNCTION_PARAMETER = 55,
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
  OP_IMAGE_SAMPLE_PROJ_EXPLICIT_LOD = 92,
  OP_IMAGE_FETCH = 95,
  OP_IMAGE_GATHER = 96,
  OP_IMAGE_DREF_GATHER = 97,
  OP_IMAGE_READ = 98,
  OP_IMAGE_WRITE = 99,
  OP_IMAGE = 100,
  OP_IMAGE_QUERY_SIZE_LOD = 103,
  OP_IMAGE_QUERY_SIZE = 104,
  OP_IMAGE_QUERY_LEVELS = 106,
  OP_CONVERT_F_TO_U = 109,
  OP_CONVERT_F_TO_S = 110,
  OP_CONVERT_S_TO_F = 111,
  OP_CONVERT_U_TO_F = 112,
  OP_U_CONVERT = 113,
  OP_S_CONVERT = 114,
  OP_F_CONVERT = 115,
  OP_QUANTIZE_TO_F16 = 116,
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
  OP_S_MOD = 139,
  OP_F_REM = 140,
  OP_F_MOD = 141,
  OP_VECTOR_TIMES_SCALAR = 142,
  OP_MATRIX_TIMES_SCALAR = 143,
  OP_VECTOR_TIMES_MATRIX = 144,
  OP_MATRIX_TIMES_VECTOR = 145,
  OP_MATRIX_TIMES_MATRIX = 146,
  OP_OUTER_PRODUCT = 147,
  OP_DOT = 148,
  OP_ANY = 154,
  OP_ALL = 155,
  OP_IS_NAN = 156,
  OP_IS_INF = 157,
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
  OP_F_UNORD_EQUAL = 181,
  OP_F_ORD_NOT_EQUAL = 182,
  OP_F_UNORD_NOT_EQUAL = 183,
  OP_F_ORD_LESS_THAN = 184,
  OP_F_UNORD_LESS_THAN = 185,
  OP_F_ORD_GREATER_THAN = 186,
  OP_F_UNORD_GREATER_THAN = 187,
  OP_F_ORD_LESS_THAN_EQUAL = 188,
  OP_F_UNORD_LESS_THAN_EQUAL = 189,
  OP_F_ORD_GREATER_THAN_EQUAL = 190,
  OP_F_UNORD_GREATER_THAN_EQUAL = 191,
  OP_SHIFT_RIGHT_LOGICAL = 194,
  OP_SHIFT_RIGHT_ARITHMETIC = 195,
  OP_SHIFT_LEFT_LOGICAL = 196,
  OP_BITWISE_OR = 197,
  OP_BITWISE_XOR = 198,
  OP_BITWISE_AND = 199,
  OP_NOT = 200,
  OP_BIT_FIELD_INSERT = 201,
  OP_BIT_FIELD_S_EXTRACT = 202,
  OP_BIT_FIELD_U_EXTRACT = 203,
  OP_BIT_REVERSE = 204,
  OP_BIT_COUNT = 205,
  OP_DPDX = 207,
  OP_DPDY = 208,
  OP_DPDX_FINE = 209,
  OP_DPDY_FINE = 210,
  OP_FWIDTH = 211,
  OP_DPDX_COARSE = 212,
  OP_DPDY_COARSE = 213,
  OP_FWIDTH_FINE = 214,
  OP_FWIDTH_COARSE = 215,
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
  std::set<u32> declared_temps;

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
        // Column-major SPIR-V matC x vecR -> row_major floatCxR
        const auto& col = types[t.comp_type];
        u32 rows = col.count ? col.count : 4;
        return "row_major float" + std::to_string(t.count) + "x" + std::to_string(rows);
      }
      case spv_type::T_IMAGE:
        if (t.sampled == 2) {
          if (t.dim == 2) return "RWTexture3D<float4>";
          if (t.arrayed || t.dim == 3) return "RWTexture2DArray<float4>";
          return "RWTexture2D<float4>";
        }
        if (t.dim == 3) return "TextureCube";
        if (t.dim == 2) return "Texture3D";
        if (t.arrayed) return "Texture2DArray";
        return "Texture2D";
      case spv_type::T_SAMPLER: return "SamplerState";
      case spv_type::T_STRUCT: return get_name(tid, "_Struct");
      case spv_type::T_ARRAY:
      case spv_type::T_RUNTIME_ARRAY: return hlsl_type(t.comp_type);
      default: return "float4";
    }
  }

  std::string hlsl_local_type(u32 tid) {
    std::string s = hlsl_type(tid);
    const size_t p = s.find("column_major ");
    if (p == 0) s.erase(0, 13);
    const size_t p2 = s.find("row_major ");
    if (p2 == 0) s.erase(0, 10);
    return s;
  }

  std::string hlsl_array_suffix(u32 tid, u32* out_elem_tid = nullptr) {
    std::string sfx;
    u32 cur = tid;
    while (types.count(cur) &&
           (types[cur].kind == spv_type::T_ARRAY ||
            types[cur].kind == spv_type::T_RUNTIME_ARRAY)) {
      u32 acnt = std::max(1u, const_u32(types[cur].len_const_id, 1u));
      sfx += "[" + std::to_string(acnt) + "]";
      cur = types[cur].comp_type;
    }
    if (out_elem_tid) *out_elem_tid = cur;
    return sfx;
  }

  std::string hlsl_var_decl(u32 tid, const std::string& name, bool zero_init = false) {
    u32 elem_tid = tid;
    std::string sfx = hlsl_array_suffix(tid, &elem_tid);
    std::string base = hlsl_local_type(elem_tid);
    std::string decl = base + " " + name + sfx;
    if (zero_init && sfx.empty()) {
      if (!types.count(elem_tid) || types[elem_tid].kind != spv_type::T_STRUCT) {
        decl += " = (" + base + ")0";
      }
    }
    return decl;
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
      if (std::isinf(f)) return (f > 0.0f) ? "asfloat(0x7F800000u)" : "asfloat(0xFF800000u)";
      if (std::isnan(f)) return "asfloat(0x7FC00000u)";
      char buf[64];
      snprintf(buf, sizeof(buf), "%.9g", f);
      std::string s(buf);
      if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
          s.find('E') == std::string::npos) {
        s += ".0";
      }
      return s;
    }
    if ((ty.kind == spv_type::T_VEC || ty.kind == spv_type::T_MAT) && !cv.comp_ids.empty()) {
      std::string s = hlsl_local_type(cv.type_id) + "(";
      for (size_t i = 0; i < cv.comp_ids.size(); ++i) {
        if (i) s += ", ";
        s += format_const(cv.comp_ids[i]);
      }
      s += ")";
      return s;
    }
    if (ty.kind == spv_type::T_STRUCT) {
      std::string zname = "_zero_" + get_name(cv.type_id, "_Struct");
      if (!declared_temps.count(cv.type_id ^ 0xA5A5A5A5u)) {
        declared_temps.insert(cv.type_id ^ 0xA5A5A5A5u);
        decls_hlsl += "static const " + hlsl_local_type(cv.type_id) + " " + zname + ";\n";
      }
      return zname;
    }
    if (ty.kind == spv_type::T_ARRAY) {
      std::string zname = "_zero_arr_" + std::to_string(cv.type_id);
      if (!declared_temps.count(cv.type_id ^ 0x5A5A5A5Au)) {
        declared_temps.insert(cv.type_id ^ 0x5A5A5A5Au);
        decls_hlsl += "static const " + hlsl_var_decl(cv.type_id, zname, false) + ";\n";
      }
      return zname;
    }
    if (ty.kind == spv_type::T_VEC || ty.kind == spv_type::T_MAT) {
      return "((" + hlsl_local_type(cv.type_id) + ")0)";
    }
    if (ty.kind == spv_type::T_FLOAT) return "0.0";
    if (ty.kind == spv_type::T_INT && !ty.is_signed) return "0u";
    if (ty.kind == spv_type::T_BOOL) return "false";
    return "0";
  }

  std::string format_array_init(u32 id) {
    auto it = consts.find(id);
    if (it == consts.end() || it->second.comp_ids.empty()) return "";
    const auto& cv = it->second;
    const auto& ty = types[cv.type_id];
    if (ty.kind != spv_type::T_ARRAY) return format_const(id);
    std::string s = "{ ";
    for (size_t i = 0; i < cv.comp_ids.size(); ++i) {
      if (i) s += ", ";
      u32 cid = cv.comp_ids[i];
      if (consts.count(cid) && types[consts[cid].type_id].kind == spv_type::T_ARRAY) {
        std::string sub = format_array_init(cid);
        s += sub.empty() ? "0" : sub;
      } else {
        s += format_const(cid);
      }
    }
    s += " }";
    return s;
  }

  std::string get_expr(u32 id) {
    auto it = exprs.find(id);
    if (it != exprs.end()) return it->second;
    if (consts.count(id)) return format_const(id);
    if (phi_vars.count(id)) return phi_vars[id];
    if (vars.count(id)) return vars[id].hlsl_name;
    if (ptrs.count(id)) return ptrs[id].hlsl_lvalue;
    if (expr_types.count(id) && types.count(expr_types[id])) {
      u32 et = expr_types[id];
      const auto& ty = types[et];
      if (ty.kind != spv_type::T_VOID && ty.kind != spv_type::T_ARRAY &&
          ty.kind != spv_type::T_RUNTIME_ARRAY && ty.kind != spv_type::T_IMAGE &&
          ty.kind != spv_type::T_SAMPLER && ty.kind != spv_type::T_SAMPLED_IMAGE) {
        if (ty.kind == spv_type::T_STRUCT) {
          std::string zname = "_zero_" + get_name(et, "_Struct");
          if (!declared_temps.count(et ^ 0xA5A5A5A5u)) {
            declared_temps.insert(et ^ 0xA5A5A5A5u);
            decls_hlsl += "static const " + hlsl_local_type(et) + " " + zname + ";\n";
          }
          return zname;
        }
        return "((" + hlsl_local_type(et) + ")0)";
      }
    }
    return "0";
  }

  // .217 DIAG/FIX: X4000 -- struct-typed function locals (OpVariable) are
  // declared WITHOUT an initializer (hlsl_var_decl skips zero-init for
  // structs), and D3DCompile rejects any read of a struct that is not
  // completely initialized on all paths (SPIR-V permits undef reads, HLSL
  // does not). Return the name of a file-scope static const zero of the
  // struct type (HLSL zero-initializes globals), creating it once.
  std::string zero_const_for(u32 tid) {
    std::string zname = "_zero_" + get_name(tid, "_Struct");
    if (!declared_temps.count(tid ^ 0xA5A5A5A5u)) {
      declared_temps.insert(tid ^ 0xA5A5A5A5u);
      decls_hlsl += "static const " + hlsl_local_type(tid) + " " + zname + ";\n";
    }
    return zname;
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
    u32 elem_tid = mtid;
    std::string arr_sfx = ctx.hlsl_array_suffix(mtid, &elem_tid);
    std::string mty = ctx.hlsl_type(elem_tid);
    if (mt.kind == spv_type::T_ARRAY && c_sub != 0) {
      u32 acnt = std::max(1u, ctx.const_u32(mt.len_const_id, 1u));
      for (u32 ai = 0; ai < acnt; ++ai) {
        u32 elem_sub = (c_sub + ai) % 4u;
        u32 elem_reg = c_reg + (c_sub + ai) / 4u;
        char epo[64];
        snprintf(epo, sizeof(epo), " : packoffset(c%u%s);", elem_reg, k_comp[elem_sub]);
        body += "  " + mty + " " + mname + "_" + std::to_string(struct_tid) + "_" +
                std::to_string(i) + "_" + std::to_string(ai) + epo + "\n";
      }
      continue;
    }
    const char* comp_sfx = k_comp[c_sub & 3u];
    char po[64];
    snprintf(po, sizeof(po), " : packoffset(c%u%s);", c_reg, comp_sfx);
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
  if (!ctx.types.count(vtid)) {
    return pv.ssbo_buf_name + ".Store(" + pv.ssbo_byte_offset_expr + ", asuint(" + vexpr + "));\n";
  }
  const auto& ty = ctx.types[vtid];
  if (ty.kind == spv_type::T_STRUCT) {
    std::string out;
    const auto& dec = ctx.decs[vtid];
    for (u32 i = 0; i < (u32)ty.members.size(); ++i) {
      u32 mtid = ty.members[i];
      u32 moff = dec.members.count(i) && dec.members.at(i).has_offset ? dec.members.at(i).offset : (i * 16u);
      std::string off = "(" + pv.ssbo_byte_offset_expr + " + " + std::to_string(moff) + "u)";
      std::string mfield = vexpr + "." + cbuffer_member_expr(ctx, vtid, i);
      u32 mn = ctx.scalar_comp_count(mtid);
      std::string mst = (mn == 4) ? "Store4" : (mn == 3 ? "Store3" : (mn == 2 ? "Store2" : "Store"));
      out += pv.ssbo_buf_name + "." + mst + "(" + off + ", asuint(" + mfield + "));\n";
    }
    return out;
  }
  if (ty.kind == spv_type::T_MAT) {
    std::string out;
    u32 cols = ty.count;
    u32 rows = ctx.types.count(ty.comp_type) ? ctx.types[ty.comp_type].count : 4u;
    std::string mst = (rows == 4) ? "Store4" : (rows == 3 ? "Store3" : (rows == 2 ? "Store2" : "Store"));
    for (u32 c = 0; c < cols; ++c) {
      std::string off = "(" + pv.ssbo_byte_offset_expr + " + " + std::to_string(c * 16u) + "u)";
      out += pv.ssbo_buf_name + "." + mst + "(" + off + ", asuint(" + vexpr + "[" + std::to_string(c) + "]));\n";
    }
    return out;
  }
  if (ty.kind == spv_type::T_ARRAY) {
    std::string out;
    u32 len = std::max(1u, ctx.const_u32(ty.len_const_id, 1u));
    u32 stride = ctx.decs[vtid].array_stride ? ctx.decs[vtid].array_stride : ctx.byte_size_of(ty.comp_type);
    u32 en = ctx.scalar_comp_count(ty.comp_type);
    std::string est = (en == 4) ? "Store4" : (en == 3 ? "Store3" : (en == 2 ? "Store2" : "Store"));
    for (u32 i = 0; i < len; ++i) {
      std::string off = "(" + pv.ssbo_byte_offset_expr + " + " + std::to_string(i * stride) + "u)";
      out += pv.ssbo_buf_name + "." + est + "(" + off + ", asuint(" + vexpr + "[" + std::to_string(i) + "]));\n";
    }
    return out;
  }
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
    case 1:  return "round(" + a(0) + ")";     // Round
    case 2:  return "round(" + a(0) + ")";     // RoundEven
    case 3:  return "trunc(" + a(0) + ")";     // Trunc
    case 4:  return "abs(" + a(0) + ")";       // FAbs
    case 5:  return "abs(" + a(0) + ")";       // SAbs
    case 6:  return "sign(" + a(0) + ")";      // FSign
    case 7:  return "sign(" + a(0) + ")";      // SSign
    case 8:  return "floor(" + a(0) + ")";     // Floor
    case 9:  return "ceil(" + a(0) + ")";      // Ceil
    case 10: return "frac(" + a(0) + ")";      // Fract
    case 11: return "radians(" + a(0) + ")";   // Radians
    case 12: return "degrees(" + a(0) + ")";   // Degrees
    case 13: return "sin(" + a(0) + ")";       // Sin
    case 14: return "cos(" + a(0) + ")";       // Cos
    case 15: return "tan(" + a(0) + ")";       // Tan
    case 16: return "asin(" + a(0) + ")";      // Asin
    case 17: return "acos(" + a(0) + ")";      // Acos
    case 18: return "atan(" + a(0) + ")";      // Atan
    case 19: return "sinh(" + a(0) + ")";      // Sinh
    case 20: return "cosh(" + a(0) + ")";      // Cosh
    case 21: return "tanh(" + a(0) + ")";      // Tanh
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
    case 58: return "((f32tof16((" + a(0) + ").x) & 0xFFFFu) | ((f32tof16((" + a(0) + ").y) & 0xFFFFu) << 16u))"; // PackHalf2x16
    case 62: return "float2(f16tof32((uint)(" + a(0) + ") & 0xFFFFu), f16tof32(((uint)(" + a(0) + ") >> 16u) & 0xFFFFu))"; // UnpackHalf2x16
    case 66: return "length(" + a(0) + ")";    // Length
    case 67: return "distance(" + a(0) + ", " + a(1) + ")"; // Distance
    case 68: return "cross(" + a(0) + ", " + a(1) + ")";    // Cross
    case 69: return "normalize(" + a(0) + ")"; // Normalize
    case 70: return "faceforward(" + a(0) + ", " + a(1) + ", " + a(2) + ")"; // FaceForward
    case 71: return "reflect(" + a(0) + ", " + a(1) + ")";  // Reflect
    case 72: return "refract(" + a(0) + ", " + a(1) + ", " + a(2) + ")"; // Refract
    case 73: return "firstbitlow(" + a(0) + ")";  // FindILsb
    case 74: return "firstbithigh(" + a(0) + ")"; // FindSMsb
    case 75: return "firstbithigh(" + a(0) + ")"; // FindUMsb
    case 79: return "min(" + a(0) + ", " + a(1) + ")";   // NMin
    case 80: return "max(" + a(0) + ", " + a(1) + ")";   // NMax
    case 81: return "clamp(" + a(0) + ", " + a(1) + ", " + a(2) + ")"; // NClamp
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
                                    std::set<u32>& visited, std::string indent,
                                    u32 loop_hdr = 0, u32 loop_merge = 0);

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
    const bool is_arr = ctx.types.count(res_tid) &&
                        (ctx.types[res_tid].kind == spv_type::T_ARRAY ||
                         ctx.types[res_tid].kind == spv_type::T_RUNTIME_ARRAY);
    if (is_arr) {
      ctx.exprs[res_id] = rhs;
      return;
    }
    std::string nm = "_t" + std::to_string(res_id);
    if (!ctx.declared_temps.count(res_id)) {
      ctx.declared_temps.insert(res_id);
      ctx.main_body_hlsl += "  " + ctx.hlsl_var_decl(res_tid, nm, true) + ";\n";
    }
    out += indent + nm + " = " + rhs + ";\n";
    ctx.exprs[res_id] = nm;
  };
  for (const spv_insn* ins : blk.body) {
    const auto& w = ins->w;
    switch (ins->op) {
      case OP_VARIABLE: {
        u32 pt = w[0], id = w[1];
        u32 pte = ctx.unwrap_struct_or_pointee(pt);
        std::string nm = ctx.get_name(id, "_loc");
        const bool is_arr = ctx.types.count(pte) &&
                            (ctx.types[pte].kind == spv_type::T_ARRAY ||
                             ctx.types[pte].kind == spv_type::T_RUNTIME_ARRAY);
        if (!ctx.declared_temps.count(id)) {
          ctx.declared_temps.insert(id);
          if (is_arr) {
            std::string ainit = (w.size() >= 4) ? ctx.format_array_init(w[3]) : "";
            ctx.decls_hlsl += "static " + ctx.hlsl_var_decl(pte, nm, false) +
                              (ainit.empty() ? "" : (" = " + ainit)) + ";\n";
          } else {
            ctx.main_body_hlsl += "  " + ctx.hlsl_var_decl(pte, nm, true) + ";\n";
            // .217: struct locals are NOT zero-initialized by hlsl_var_decl
            // (it skips structs), so a read before every member is stored
            // fails D3DCompile with X4000. Assign the zero const at
            // declaration time so the variable is definitely initialized.
            if (ctx.types.count(pte) && ctx.types[pte].kind == spv_type::T_STRUCT) {
              ctx.main_body_hlsl += "  " + nm + " = " + ctx.zero_const_for(pte) + ";\n";
            }
          }
        }
        if (!is_arr && w.size() >= 4) {
          out += indent + nm + " = " + ctx.get_expr(w[3]) + ";\n";
        }
        ctx.ptrs[id] = {true, id, false, "", "", nm, pte};
        ctx.exprs[id] = nm;
        ctx.expr_types[id] = pte;
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
        const bool is_unif_const =
            ctx.vars.count(pv.base_var) &&
            ctx.vars[pv.base_var].storage_class == SC_UNIFORM_CONSTANT;
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
          } else if (is_unif_const) {
            // Descriptor arrays of textures/samplers/storage-images collapse
            // to the bound HLSL resource name in SM5.0 (do not emit tex[i],
            // which HLSL parses as Texture2D::operator[](uint2) texel load).
            if (ct.kind == spv_type::T_ARRAY || ct.kind == spv_type::T_RUNTIME_ARRAY) {
              cur_tid = ct.comp_type;
            }
          } else {
            if (ct.kind == spv_type::T_STRUCT) {
              u32 mi = ctx.const_u32(idx_id, 0);
              const bool is_root_cb =
                  ctx.vars.count(pv.base_var) &&
                  (ctx.vars[pv.base_var].is_ubo || ctx.vars[pv.base_var].is_push) &&
                  base == pv.base_var && i == 3;
              if (is_root_cb) {
                pv.hlsl_lvalue = cbuffer_member_expr(ctx, cur_tid, mi);
              } else {
                pv.hlsl_lvalue += "." + cbuffer_member_expr(ctx, cur_tid, mi);
              }
              cur_tid = (mi < ct.members.size()) ? ct.members[mi] : cur_tid;
            } else if (ct.kind == spv_type::T_ARRAY || ct.kind == spv_type::T_RUNTIME_ARRAY ||
                       ct.kind == spv_type::T_VEC || ct.kind == spv_type::T_MAT) {
              pv.hlsl_lvalue += "[" + ctx.get_expr(idx_id) + "]";
              cur_tid = ct.comp_type;
            }
          }
        }
        pv.pointee_type = pte;
        ctx.ptrs[id] = pv;
        ctx.exprs[id] = pv.hlsl_lvalue;
        ctx.expr_types[id] = pte;
      } break;
      case OP_LOAD: {
        u32 rt = w[0], id = w[1], ptr = w[2];
        if (ctx.ptrs.count(ptr) && ctx.ptrs[ptr].is_ssbo) {
          emit_assign(id, rt, ssbo_load_expr(ctx, ctx.ptrs[ptr], rt));
        } else if (ctx.ptrs.count(ptr)) {
          const auto& pv = ctx.ptrs[ptr];
          const bool is_res = ctx.types.count(rt) &&
                              (ctx.types[rt].kind == spv_type::T_IMAGE ||
                               ctx.types[rt].kind == spv_type::T_SAMPLER ||
                               ctx.types[rt].kind == spv_type::T_SAMPLED_IMAGE ||
                               ctx.types[rt].kind == spv_type::T_ARRAY);
          if (is_res) {
            ctx.exprs[id] = pv.hlsl_lvalue;
            ctx.expr_types[id] = rt;
          } else {
            emit_assign(id, rt, pv.hlsl_lvalue);
          }
        } else if (ctx.vars.count(ptr)) {
          const bool is_res = ctx.types.count(rt) &&
                              (ctx.types[rt].kind == spv_type::T_IMAGE ||
                               ctx.types[rt].kind == spv_type::T_SAMPLER ||
                               ctx.types[rt].kind == spv_type::T_SAMPLED_IMAGE ||
                               ctx.types[rt].kind == spv_type::T_ARRAY);
          if (is_res) {
            ctx.exprs[id] = ctx.vars[ptr].hlsl_name;
            ctx.expr_types[id] = rt;
          } else {
            emit_assign(id, rt, ctx.vars[ptr].hlsl_name);
          }
        } else {
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
      case OP_VECTOR_EXTRACT_DYNAMIC: {
        u32 rt = w[0], id = w[1], vec = w[2], idx = w[3];
        emit_assign(id, rt, ctx.get_expr(vec) + "[" + ctx.get_expr(idx) + "]");
      } break;
      case OP_VECTOR_INSERT_DYNAMIC: {
        u32 rt = w[0], id = w[1], vec = w[2], comp = w[3], idx = w[4];
        emit_assign(id, rt, ctx.get_expr(vec));
        out += indent + ctx.exprs[id] + "[" + ctx.get_expr(idx) + "] = " + ctx.get_expr(comp) + ";\n";
      } break;
      case OP_COMPOSITE_CONSTRUCT: {
        u32 rt = w[0], id = w[1];
        if (ctx.types.count(rt) && ctx.types[rt].kind == spv_type::T_STRUCT) {
          std::string nm = "_t" + std::to_string(id);
          if (!ctx.declared_temps.count(id)) {
            ctx.declared_temps.insert(id);
            ctx.main_body_hlsl += "  " + ctx.hlsl_var_decl(rt, nm, false) + ";\n";
          }
          ctx.exprs[id] = nm;
          ctx.expr_types[id] = rt;
          for (size_t i = 2; i < w.size() && (i - 2) < ctx.types[rt].members.size(); ++i) {
            u32 mi = (u32)(i - 2);
            out += indent + nm + "." + cbuffer_member_expr(ctx, rt, mi) +
                   " = " + ctx.get_expr(w[i]) + ";\n";
          }
        } else if (ctx.types.count(rt) && ctx.types[rt].kind == spv_type::T_ARRAY) {
          std::string nm = "_t" + std::to_string(id);
          if (!ctx.declared_temps.count(id)) {
            ctx.declared_temps.insert(id);
            ctx.decls_hlsl += "static " + ctx.hlsl_var_decl(rt, nm, false) + ";\n";
          }
          for (size_t i = 2; i < w.size(); ++i) {
            out += indent + nm + "[" + std::to_string(i - 2) + "] = " + ctx.get_expr(w[i]) + ";\n";
          }
          ctx.exprs[id] = nm;
          ctx.expr_types[id] = rt;
        } else {
          std::string rhs = ctx.hlsl_local_type(rt) + "(";
          for (size_t i = 2; i < w.size(); ++i) {
            if (i > 2) rhs += ", ";
            rhs += ctx.get_expr(w[i]);
          }
          rhs += ")";
          emit_assign(id, rt, rhs);
        }
      } break;
      case OP_COMPOSITE_EXTRACT: {
        u32 rt = w[0], id = w[1], comp = w[2];
        std::string e = ctx.get_expr(comp);
        u32 cur_tid = ctx.expr_types.count(comp)
                          ? ctx.expr_types[comp]
                          : (ctx.consts.count(comp) ? ctx.consts[comp].type_id : 0);
        static const char* k_sw = "xyzw";
        for (size_t i = 3; i < w.size(); ++i) {
          u32 idx = w[i];
          if (ctx.types.count(cur_tid)) {
            const auto& ct = ctx.types[cur_tid];
            if (ct.kind == spv_type::T_STRUCT) {
              e += "." + cbuffer_member_expr(ctx, cur_tid, idx);
              cur_tid = (idx < ct.members.size()) ? ct.members[idx] : 0;
              continue;
            } else if (ct.kind == spv_type::T_VEC) {
              if (e == "0") {
                cur_tid = ct.comp_type;
                continue;
              }
              e += std::string(".") + k_sw[idx & 3u];
              cur_tid = ct.comp_type;
              continue;
            } else if (ct.kind == spv_type::T_ARRAY || ct.kind == spv_type::T_MAT) {
              e += "[" + std::to_string(idx) + "]";
              cur_tid = ct.comp_type;
              continue;
            } else if (ct.kind == spv_type::T_FLOAT || ct.kind == spv_type::T_INT ||
                       ct.kind == spv_type::T_BOOL) {
              continue;
            }
          }
          if (e != "0" && ctx.scalar_comp_count(cur_tid) > 1 && idx < 4 && (i == w.size() - 1)) {
            e += std::string(".") + k_sw[idx & 3u];
          } else if (e != "0" && ctx.scalar_comp_count(cur_tid) > 1) {
            e += "[" + std::to_string(idx) + "]";
          }
        }
        emit_assign(id, rt, e);
      } break;
      case OP_COMPOSITE_INSERT: {
        u32 rt = w[0], id = w[1], val = w[2], comp = w[3];
        emit_assign(id, rt, ctx.get_expr(comp));
        std::string lhs = ctx.exprs[id];
        u32 cur_tid = rt;
        static const char* k_sw = "xyzw";
        for (size_t i = 4; i < w.size(); ++i) {
          u32 idx = w[i];
          if (ctx.types.count(cur_tid)) {
            const auto& ct = ctx.types[cur_tid];
            if (ct.kind == spv_type::T_STRUCT) {
              lhs += "." + cbuffer_member_expr(ctx, cur_tid, idx);
              cur_tid = (idx < ct.members.size()) ? ct.members[idx] : 0;
              continue;
            } else if (ct.kind == spv_type::T_VEC) {
              lhs += std::string(".") + k_sw[idx & 3u];
              cur_tid = ct.comp_type;
              continue;
            } else if (ct.kind == spv_type::T_ARRAY || ct.kind == spv_type::T_MAT) {
              lhs += "[" + std::to_string(idx) + "]";
              cur_tid = ct.comp_type;
              continue;
            } else if (ct.kind == spv_type::T_FLOAT || ct.kind == spv_type::T_INT ||
                       ct.kind == spv_type::T_BOOL) {
              continue;
            }
          }
          if (lhs != "0" && ctx.scalar_comp_count(cur_tid) > 1 && idx < 4 && (i == w.size() - 1)) {
            lhs += std::string(".") + k_sw[idx & 3u];
          } else if (lhs != "0" && ctx.scalar_comp_count(cur_tid) > 1) {
            lhs += "[" + std::to_string(idx) + "]";
          }
        }
        out += indent + lhs + " = " + ctx.get_expr(val) + ";\n";
      } break;
      case OP_VECTOR_SHUFFLE: {
        u32 rt = w[0], id = w[1], v1 = w[2], v2 = w[3];
        u32 c1 = ctx.scalar_comp_count(ctx.expr_types.count(v1) ? ctx.expr_types[v1] : rt);
        u32 c2 = ctx.scalar_comp_count(ctx.expr_types.count(v2) ? ctx.expr_types[v2] : rt);
        std::string rhs = ctx.hlsl_local_type(rt) + "(";
        static const char* k_sw = "xyzw";
        for (size_t i = 4; i < w.size(); ++i) {
          if (i > 4) rhs += ", ";
          u32 c = w[i];
          if (c == 0xFFFFFFFFu) {
            rhs += "0";
          } else if (c < c1) {
            std::string e1 = ctx.get_expr(v1);
            if (c1 <= 1 || e1 == "0") rhs += e1;
            else rhs += e1 + "." + k_sw[c & 3u];
          } else {
            std::string e2 = ctx.get_expr(v2);
            if (c2 <= 1 || e2 == "0") rhs += e2;
            else rhs += e2 + "." + k_sw[(c - c1) & 3u];
          }
        }
        rhs += ")";
        emit_assign(id, rt, rhs);
      } break;
      case OP_SAMPLED_IMAGE:
        ctx.exprs[w[1]] = ctx.get_expr(w[2]); // carries tex name; smp stored by convention _smp
        ctx.expr_types[w[1]] = w[0];
        ctx.ptrs[w[1]].ssbo_buf_name = ctx.get_expr(w[3]); // sampler name
        break;
      case OP_IMAGE:
        ctx.exprs[w[1]] = ctx.get_expr(w[2]);
        ctx.expr_types[w[1]] = w[0];
        break;
      case OP_IMAGE_SAMPLE_IMPLICIT_LOD:
      case OP_IMAGE_SAMPLE_EXPLICIT_LOD: {
        u32 rt = w[0], id = w[1], si = w[2], coord = w[3];
        std::string tex = ctx.get_expr(si);
        std::string smp = (ctx.ptrs.count(si) && !ctx.ptrs[si].ssbo_buf_name.empty())
                              ? ctx.ptrs[si].ssbo_buf_name : (tex + "_smp");
        std::string rhs;
        if (ins->op == OP_IMAGE_SAMPLE_EXPLICIT_LOD && w.size() >= 6) {
          if (w[4] & 0x2u /*Lod*/) {
            rhs = tex + ".SampleLevel(" + smp + ", " + ctx.get_expr(coord) + ", " + ctx.get_expr(w[5]) + ")";
          } else if ((w[4] & 0x4u /*Grad*/) && w.size() >= 7) {
            rhs = tex + ".SampleGrad(" + smp + ", " + ctx.get_expr(coord) + ", " + ctx.get_expr(w[5]) + ", " + ctx.get_expr(w[6]) + ")";
          } else {
            rhs = tex + ".SampleLevel(" + smp + ", " + ctx.get_expr(coord) + ", 0.0)";
          }
        } else if (ctx.active_ep.stage != SPV_STAGE_FRAGMENT) {
          rhs = tex + ".SampleLevel(" + smp + ", " + ctx.get_expr(coord) + ", 0.0)";
        } else {
          rhs = tex + ".Sample(" + smp + ", " + ctx.get_expr(coord) + ")";
        }
        emit_assign(id, rt, rhs);
      } break;
      case OP_IMAGE_SAMPLE_DREF_IMPLICIT_LOD:
      case OP_IMAGE_SAMPLE_DREF_EXPLICIT_LOD: {
        u32 rt = w[0], id = w[1], si = w[2], coord = w[3], dref = w[4];
        std::string tex = ctx.get_expr(si);
        std::string smp = (ctx.ptrs.count(si) && !ctx.ptrs[si].ssbo_buf_name.empty())
                              ? ctx.ptrs[si].ssbo_buf_name : (tex + "_smp");
        std::string s = tex + ".SampleLevel(" + smp + ", " + ctx.get_expr(coord) + ", 0.0).x";
        emit_assign(id, rt, "((" + s + " <= " + ctx.get_expr(dref) + ") ? 1.0 : 0.0)");
      } break;
      case OP_IMAGE_SAMPLE_PROJ_IMPLICIT_LOD:
      case OP_IMAGE_SAMPLE_PROJ_EXPLICIT_LOD: {
        u32 rt = w[0], id = w[1], si = w[2], coord = w[3];
        std::string tex = ctx.get_expr(si);
        std::string smp = (ctx.ptrs.count(si) && !ctx.ptrs[si].ssbo_buf_name.empty())
                              ? ctx.ptrs[si].ssbo_buf_name : (tex + "_smp");
        std::string c = ctx.get_expr(coord);
        emit_assign(id, rt, tex + ".SampleLevel(" + smp + ", (" + c + ").xy / (" + c + ").w, 0.0)");
      } break;
      case OP_IMAGE_GATHER:
      case OP_IMAGE_DREF_GATHER: {
        u32 rt = w[0], id = w[1], si = w[2], coord = w[3];
        std::string tex = ctx.get_expr(si);
        std::string smp = (ctx.ptrs.count(si) && !ctx.ptrs[si].ssbo_buf_name.empty())
                              ? ctx.ptrs[si].ssbo_buf_name : (tex + "_smp");
        emit_assign(id, rt, tex + ".Gather(" + smp + ", " + ctx.get_expr(coord) + ")");
      } break;
      case OP_IMAGE_FETCH: {
        u32 rt = w[0], id = w[1], img = w[2], coord = w[3];
        u32 c_tid = ctx.expr_types.count(coord) ? ctx.expr_types[coord]
                  : (ctx.consts.count(coord) ? ctx.consts[coord].type_id : 0);
        u32 c_cnt = ctx.scalar_comp_count(c_tid);
        std::string lod = (w.size() >= 6 && (w[4] & 0x2u)) ? ("(int)(" + ctx.get_expr(w[5]) + ")") : "0";
        std::string larg = (c_cnt >= 3)
                               ? ("int4((int3)(" + ctx.get_expr(coord) + "), " + lod + ")")
                               : ("int3((int2)(" + ctx.get_expr(coord) + "), " + lod + ")");
        emit_assign(id, rt, "((" + ctx.hlsl_local_type(rt) + ")" + ctx.get_expr(img) + ".Load(" + larg + "))");
      } break;
      case OP_IMAGE_READ: {
        u32 rt = w[0], id = w[1], img = w[2], coord = w[3];
        emit_assign(id, rt, "((" + ctx.hlsl_local_type(rt) + ")" + ctx.get_expr(img) + "[" + ctx.get_expr(coord) + "])");
      } break;
      case OP_IMAGE_WRITE:
        out += indent + ctx.get_expr(w[0]) + "[" + ctx.get_expr(w[1]) + "] = (float4)(" +
               ctx.get_expr(w[2]) + ");\n";
        break;
      case OP_IMAGE_QUERY_SIZE_LOD:
      case OP_IMAGE_QUERY_SIZE: {
        u32 rt = w[0], id = w[1], img = w[2];
        u32 cnt = ctx.scalar_comp_count(rt);
        std::string qw = "_qw" + std::to_string(id), qh = "_qh" + std::to_string(id);
        if (cnt >= 3) {
          std::string qd = "_qd" + std::to_string(id);
          out += indent + "uint " + qw + " = 1u, " + qh + " = 1u, " + qd + " = 1u;\n";
          out += indent + ctx.get_expr(img) + ".GetDimensions(" + qw + ", " + qh + ", " + qd + ");\n";
          emit_assign(id, rt, ctx.hlsl_local_type(rt) + "(" + qw + ", " + qh + ", " + qd + ")");
        } else if (cnt == 2) {
          out += indent + "uint " + qw + " = 1u, " + qh + " = 1u;\n";
          out += indent + ctx.get_expr(img) + ".GetDimensions(" + qw + ", " + qh + ");\n";
          emit_assign(id, rt, ctx.hlsl_local_type(rt) + "(" + qw + ", " + qh + ")");
        } else {
          emit_assign(id, rt, "(" + ctx.hlsl_local_type(rt) + ")1");
        }
      } break;
      case OP_IMAGE_QUERY_LEVELS: {
        u32 rt = w[0], id = w[1];
        emit_assign(id, rt, "(" + ctx.hlsl_local_type(rt) + ")1");
      } break;
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
      case OP_U_MOD: case OP_S_REM: case OP_S_MOD:
        emit_assign(w[1], w[0], "(" + ctx.get_expr(w[2]) + " % " + ctx.get_expr(w[3]) + ")"); break;
      case OP_F_REM: case OP_F_MOD:
        emit_assign(w[1], w[0], "fmod(" + ctx.get_expr(w[2]) + ", " + ctx.get_expr(w[3]) + ")"); break;
      case OP_S_NEGATE: case OP_F_NEGATE:
        emit_assign(w[1], w[0], "(-" + ctx.get_expr(w[2]) + ")"); break;
      case OP_MATRIX_TIMES_VECTOR: {
        std::string m = ctx.get_expr(w[2]);
        std::string v = ctx.get_expr(w[3]);
        u32 v_tid = ctx.expr_types.count(w[3]) ? ctx.expr_types[w[3]]
                    : (ctx.consts.count(w[3]) ? ctx.consts[w[3]].type_id : w[0]);
        if (v == "0" && ctx.types.count(v_tid)) {
          v = "((" + ctx.hlsl_local_type(v_tid) + ")0)";
        }
        emit_assign(w[1], w[0], "mul(" + m + ", " + v + ")");
      } break;
      case OP_VECTOR_TIMES_MATRIX: {
        std::string v = ctx.get_expr(w[2]);
        std::string m = ctx.get_expr(w[3]);
        u32 v_tid = ctx.expr_types.count(w[2]) ? ctx.expr_types[w[2]]
                    : (ctx.consts.count(w[2]) ? ctx.consts[w[2]].type_id : w[0]);
        if (v == "0" && ctx.types.count(v_tid)) {
          v = "((" + ctx.hlsl_local_type(v_tid) + ")0)";
        }
        emit_assign(w[1], w[0], "mul(" + v + ", " + m + ")");
      } break;
      case OP_MATRIX_TIMES_MATRIX:
        emit_assign(w[1], w[0], "mul(" + ctx.get_expr(w[2]) + ", " + ctx.get_expr(w[3]) + ")"); break;
      case OP_DOT:
        emit_assign(w[1], w[0], "dot(" + ctx.get_expr(w[2]) + ", " + ctx.get_expr(w[3]) + ")"); break;
      case OP_TRANSPOSE:
        emit_assign(w[1], w[0], "transpose(" + ctx.get_expr(w[2]) + ")"); break;
      case OP_ANY:
        emit_assign(w[1], w[0], "any(" + ctx.get_expr(w[2]) + ")"); break;
      case OP_ALL:
        emit_assign(w[1], w[0], "all(" + ctx.get_expr(w[2]) + ")"); break;
      case OP_IS_NAN:
        emit_assign(w[1], w[0], "isnan(" + ctx.get_expr(w[2]) + ")"); break;
      case OP_IS_INF:
        emit_assign(w[1], w[0], "isinf(" + ctx.get_expr(w[2]) + ")"); break;
      case OP_SELECT: {
        u32 rt = w[0], id = w[1], cond = w[2], tv = w[3], fv = w[4];
        emit_assign(id, rt, "(" + ctx.get_expr(cond) + " ? " + ctx.get_expr(tv) + " : " + ctx.get_expr(fv) + ")");
      } break;
      case OP_CONVERT_F_TO_U: case OP_CONVERT_F_TO_S: case OP_CONVERT_S_TO_F:
      case OP_CONVERT_U_TO_F: case OP_U_CONVERT: case OP_S_CONVERT: case OP_F_CONVERT:
        emit_assign(w[1], w[0], "((" + ctx.hlsl_local_type(w[0]) + ")" + ctx.get_expr(w[2]) + ")"); break;
      case OP_QUANTIZE_TO_F16:
        emit_assign(w[1], w[0], "f16tof32(f32tof16(" + ctx.get_expr(w[2]) + "))"); break;
      case OP_BITCAST: {
        std::string fn = ctx.is_float_type(w[0]) ? "asfloat" : (ctx.is_signed_int_type(w[0]) ? "asint" : "asuint");
        emit_assign(w[1], w[0], fn + "(" + ctx.get_expr(w[2]) + ")");
      } break;
      case OP_I_EQUAL: case OP_F_ORD_EQUAL: case OP_F_UNORD_EQUAL: case OP_LOGICAL_EQUAL:
        emit_assign(w[1], w[0], "(" + ctx.get_expr(w[2]) + " == " + ctx.get_expr(w[3]) + ")"); break;
      case OP_I_NOT_EQUAL: case OP_F_ORD_NOT_EQUAL: case OP_F_UNORD_NOT_EQUAL: case OP_LOGICAL_NOT_EQUAL:
        emit_assign(w[1], w[0], "(" + ctx.get_expr(w[2]) + " != " + ctx.get_expr(w[3]) + ")"); break;
      case OP_U_LESS_THAN: case OP_S_LESS_THAN: case OP_F_ORD_LESS_THAN: case OP_F_UNORD_LESS_THAN:
        emit_assign(w[1], w[0], "(" + ctx.get_expr(w[2]) + " < " + ctx.get_expr(w[3]) + ")"); break;
      case OP_U_GREATER_THAN: case OP_S_GREATER_THAN: case OP_F_ORD_GREATER_THAN: case OP_F_UNORD_GREATER_THAN:
        emit_assign(w[1], w[0], "(" + ctx.get_expr(w[2]) + " > " + ctx.get_expr(w[3]) + ")"); break;
      case OP_U_LESS_THAN_EQUAL: case OP_S_LESS_THAN_EQUAL: case OP_F_ORD_LESS_THAN_EQUAL: case OP_F_UNORD_LESS_THAN_EQUAL:
        emit_assign(w[1], w[0], "(" + ctx.get_expr(w[2]) + " <= " + ctx.get_expr(w[3]) + ")"); break;
      case OP_U_GREATER_THAN_EQUAL: case OP_S_GREATER_THAN_EQUAL: case OP_F_ORD_GREATER_THAN_EQUAL: case OP_F_UNORD_GREATER_THAN_EQUAL:
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
      case OP_NOT:
        emit_assign(w[1], w[0], "(~" + ctx.get_expr(w[2]) + ")"); break;
      case OP_SHIFT_LEFT_LOGICAL:
        emit_assign(w[1], w[0], "(" + ctx.get_expr(w[2]) + " << " + ctx.get_expr(w[3]) + ")"); break;
      case OP_SHIFT_RIGHT_LOGICAL: case OP_SHIFT_RIGHT_ARITHMETIC:
        emit_assign(w[1], w[0], "(" + ctx.get_expr(w[2]) + " >> " + ctx.get_expr(w[3]) + ")"); break;
      case OP_BIT_FIELD_U_EXTRACT: {
        std::string b = ctx.get_expr(w[2]), o = ctx.get_expr(w[3]), c = ctx.get_expr(w[4]);
        emit_assign(w[1], w[0], "((" + ctx.hlsl_local_type(w[0]) + ")(((" + b + ") >> (" + o + ")) & (((1u << (" + c + ")) - 1u))))");
      } break;
      case OP_BIT_FIELD_S_EXTRACT: {
        std::string b = ctx.get_expr(w[2]), o = ctx.get_expr(w[3]), c = ctx.get_expr(w[4]);
        emit_assign(w[1], w[0], "((" + ctx.hlsl_local_type(w[0]) + ")(((int)(" + b + ") << (32 - ((int)(" + o + ") + (int)(" + c + ")))) >> (32 - (int)(" + c + "))))");
      } break;
      case OP_BIT_FIELD_INSERT: {
        std::string b = ctx.get_expr(w[2]), ins_v = ctx.get_expr(w[3]), o = ctx.get_expr(w[4]), c = ctx.get_expr(w[5]);
        std::string mask = "(((1u << (" + c + ")) - 1u) << (" + o + "))";
        emit_assign(w[1], w[0], "((" + ctx.hlsl_local_type(w[0]) + ")(((" + b + ") & ~" + mask + ") | (((" + ins_v + ") << (" + o + ")) & " + mask + ")))");
      } break;
      case OP_BIT_REVERSE:
        emit_assign(w[1], w[0], "reversebits(" + ctx.get_expr(w[2]) + ")"); break;
      case OP_BIT_COUNT:
        emit_assign(w[1], w[0], "((" + ctx.hlsl_local_type(w[0]) + ")countbits(" + ctx.get_expr(w[2]) + "))"); break;
      case OP_DPDX: case OP_DPDX_FINE: case OP_DPDX_COARSE:
        emit_assign(w[1], w[0], "ddx(" + ctx.get_expr(w[2]) + ")"); break;
      case OP_DPDY: case OP_DPDY_FINE: case OP_DPDY_COARSE:
        emit_assign(w[1], w[0], "ddy(" + ctx.get_expr(w[2]) + ")"); break;
      case OP_FWIDTH: case OP_FWIDTH_FINE: case OP_FWIDTH_COARSE:
        emit_assign(w[1], w[0], "fwidth(" + ctx.get_expr(w[2]) + ")"); break;
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
    std::set<u32>& visited, std::string indent, u32 loop_hdr, u32 loop_merge) {
  std::string out;
  u32 cur = start_lbl;
  for (int guard = 0; guard < 1024; ++guard) {
    if (!cur || cur == stop_lbl || visited.count(cur)) break;
    auto it = blocks.find(cur);
    if (it == blocks.end()) break;
    visited.insert(cur);
    const spv_block& blk = it->second;

    if (blk.merge && blk.merge->op == OP_LOOP_MERGE) {
      u32 l_merge = blk.merge->w[0];
      u32 l_cont = (blk.merge->w.size() >= 2) ? blk.merge->w[1] : 0;
      out += indent + "for (uint _iter = 0; _iter < 65536u; ++_iter) {\n";
      std::string b_indent = indent + "  ";
      out += emit_block_stmts(ctx, blk, b_indent);
      std::set<u32> loop_vis;
      loop_vis.insert(cur);
      if (blk.term && blk.term->op == OP_BRANCH) {
        u32 tgt = blk.term->w[0];
        emit_phi_copies(ctx, cur, tgt, phi_edges, out, b_indent);
        if (tgt == l_merge) {
          out += b_indent + "break;\n";
        } else if (tgt != cur) {
          out += emit_block_chain(ctx, blocks, tgt, cur, phi_edges, loop_vis, b_indent, cur, l_merge);
        }
      } else if (blk.term && blk.term->op == OP_BRANCH_CONDITIONAL) {
        u32 cond_id = blk.term->w[0], t_lbl = blk.term->w[1], f_lbl = blk.term->w[2];
        if (f_lbl == l_merge) {
          out += b_indent + "if (!(" + ctx.get_expr(cond_id) + ")) {\n";
          emit_phi_copies(ctx, cur, l_merge, phi_edges, out, b_indent + "  ");
          out += b_indent + "  break;\n";
          out += b_indent + "}\n";
          emit_phi_copies(ctx, cur, t_lbl, phi_edges, out, b_indent);
          out += emit_block_chain(ctx, blocks, t_lbl, cur, phi_edges, loop_vis, b_indent, cur, l_merge);
        } else if (t_lbl == l_merge) {
          out += b_indent + "if (" + ctx.get_expr(cond_id) + ") {\n";
          emit_phi_copies(ctx, cur, l_merge, phi_edges, out, b_indent + "  ");
          out += b_indent + "  break;\n";
          out += b_indent + "}\n";
          emit_phi_copies(ctx, cur, f_lbl, phi_edges, out, b_indent);
          out += emit_block_chain(ctx, blocks, f_lbl, cur, phi_edges, loop_vis, b_indent, cur, l_merge);
        } else {
          u32 sub_stop = (l_cont && l_cont != cur) ? l_cont : cur;
          out += b_indent + "if (" + ctx.get_expr(cond_id) + ") {\n";
          emit_phi_copies(ctx, cur, t_lbl, phi_edges, out, b_indent + "  ");
          out += emit_block_chain(ctx, blocks, t_lbl, sub_stop, phi_edges, loop_vis, b_indent + "  ", cur, l_merge);
          out += b_indent + "} else {\n";
          emit_phi_copies(ctx, cur, f_lbl, phi_edges, out, b_indent + "  ");
          out += emit_block_chain(ctx, blocks, f_lbl, sub_stop, phi_edges, loop_vis, b_indent + "  ", cur, l_merge);
          out += b_indent + "}\n";
          if (sub_stop != cur) {
            out += emit_block_chain(ctx, blocks, sub_stop, cur, phi_edges, loop_vis, b_indent, cur, l_merge);
          }
        }
      }
      out += indent + "}\n";
      cur = l_merge;
      continue;
    }

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
      if (loop_merge && tgt == loop_merge) {
        out += indent + "break;\n";
        break;
      }
      if (loop_hdr && tgt == loop_hdr) {
        break;
      }
      if (tgt == stop_lbl) break;
      cur = tgt;
      continue;
    }
    if (blk.term->op == OP_BRANCH_CONDITIONAL) {
      u32 cond_id = blk.term->w[0], t_lbl = blk.term->w[1], f_lbl = blk.term->w[2];
      u32 merge_lbl = (blk.merge && blk.merge->op == OP_SELECTION_MERGE) ? blk.merge->w[0] : stop_lbl;
      out += indent + "if (" + ctx.get_expr(cond_id) + ") {\n";
      emit_phi_copies(ctx, cur, t_lbl, phi_edges, out, indent + "  ");
      if (loop_merge && t_lbl == loop_merge) {
        out += indent + "  break;\n";
      } else if (loop_hdr && t_lbl == loop_hdr) {
        out += indent + "  continue;\n";
      } else if (t_lbl != merge_lbl) {
        out += emit_block_chain(ctx, blocks, t_lbl, merge_lbl, phi_edges, visited, indent + "  ", loop_hdr, loop_merge);
      }
      if (f_lbl != merge_lbl || (loop_merge && f_lbl == loop_merge) || (loop_hdr && f_lbl == loop_hdr)) {
        out += indent + "} else {\n";
        emit_phi_copies(ctx, cur, f_lbl, phi_edges, out, indent + "  ");
        if (loop_merge && f_lbl == loop_merge) {
          out += indent + "  break;\n";
        } else if (loop_hdr && f_lbl == loop_hdr) {
          out += indent + "  continue;\n";
        } else if (f_lbl != merge_lbl) {
          out += emit_block_chain(ctx, blocks, f_lbl, merge_lbl, phi_edges, visited, indent + "  ", loop_hdr, loop_merge);
        }
      }
      out += indent + "}\n";
      if ((loop_merge && merge_lbl == loop_merge) || (loop_hdr && merge_lbl == loop_hdr)) break;
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
          out += emit_block_chain(ctx, blocks, clbl, merge_lbl, phi_edges, visited, indent + "    ", loop_hdr, loop_merge);
        }
        out += indent + "  } break;\n";
      }
      if (def_lbl != merge_lbl) {
        out += indent + "  default: {\n";
        emit_phi_copies(ctx, cur, def_lbl, phi_edges, out, indent + "    ");
        out += emit_block_chain(ctx, blocks, def_lbl, merge_lbl, phi_edges, visited, indent + "    ", loop_hdr, loop_merge);
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
        ctx.expr_types[w[1]] = w[0];
      } break;
      case OP_CONSTANT_FALSE:
      case OP_SPEC_CONSTANT_FALSE: {
        spv_const_val c{true, true, false, w[0], {0}, {}};
        int sid = ctx.decs[w[1]].spec_id;
        if (sid >= 0 && ctx.spec_overrides.count((u32)sid)) c.b = (ctx.spec_overrides[(u32)sid].value_u32 != 0);
        ctx.consts[w[1]] = c;
        ctx.expr_types[w[1]] = w[0];
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
        ctx.expr_types[w[1]] = w[0];
      } break;
      case OP_CONSTANT_COMPOSITE:
      case OP_SPEC_CONSTANT_COMPOSITE: {
        spv_const_val c; c.is_const = true; c.type_id = w[0];
        for (size_t i = 2; i < w.size(); ++i) c.comp_ids.push_back(w[i]);
        ctx.consts[w[1]] = std::move(c);
        ctx.expr_types[w[1]] = w[0];
      } break;
      case OP_SPEC_CONSTANT_OP: {
        if (w.size() >= 3) {
          u32 rt = w[0], id = w[1], sop = w[2];
          u32 a = (w.size() >= 4) ? ctx.const_u32(w[3], 0) : 0;
          u32 b = (w.size() >= 5) ? ctx.const_u32(w[4], 0) : 0;
          u32 c_v = (w.size() >= 6) ? ctx.const_u32(w[5], 0) : 0;
          u32 res_u = 0;
          bool is_b = ctx.types.count(rt) && ctx.types[rt].kind == spv_type::T_BOOL;
          switch (sop) {
            case OP_I_ADD: res_u = a + b; break;
            case OP_I_SUB: res_u = a - b; break;
            case OP_I_MUL: res_u = a * b; break;
            case OP_U_DIV: res_u = b ? (a / b) : 0; break;
            case OP_S_DIV: res_u = b ? (u32)((int32_t)a / (int32_t)b) : 0; break;
            case OP_U_MOD: res_u = b ? (a % b) : 0; break;
            case OP_S_REM: case OP_S_MOD: res_u = b ? (u32)((int32_t)a % (int32_t)b) : 0; break;
            case OP_S_NEGATE: res_u = (u32)(-(int32_t)a); break;
            case OP_NOT: res_u = ~a; break;
            case OP_BITWISE_OR: res_u = a | b; break;
            case OP_BITWISE_XOR: res_u = a ^ b; break;
            case OP_BITWISE_AND: res_u = a & b; break;
            case OP_SHIFT_LEFT_LOGICAL: res_u = a << (b & 31u); break;
            case OP_SHIFT_RIGHT_LOGICAL: res_u = a >> (b & 31u); break;
            case OP_SHIFT_RIGHT_ARITHMETIC: res_u = (u32)((int32_t)a >> (b & 31u)); break;
            case OP_LOGICAL_OR: res_u = (a || b) ? 1u : 0u; break;
            case OP_LOGICAL_AND: res_u = (a && b) ? 1u : 0u; break;
            case OP_LOGICAL_NOT: res_u = (!a) ? 1u : 0u; break;
            case OP_LOGICAL_EQUAL: case OP_I_EQUAL: res_u = (a == b) ? 1u : 0u; break;
            case OP_LOGICAL_NOT_EQUAL: case OP_I_NOT_EQUAL: res_u = (a != b) ? 1u : 0u; break;
            case OP_U_LESS_THAN: res_u = (a < b) ? 1u : 0u; break;
            case OP_S_LESS_THAN: res_u = ((int32_t)a < (int32_t)b) ? 1u : 0u; break;
            case OP_U_GREATER_THAN: res_u = (a > b) ? 1u : 0u; break;
            case OP_S_GREATER_THAN: res_u = ((int32_t)a > (int32_t)b) ? 1u : 0u; break;
            case OP_U_LESS_THAN_EQUAL: res_u = (a <= b) ? 1u : 0u; break;
            case OP_S_LESS_THAN_EQUAL: res_u = ((int32_t)a <= (int32_t)b) ? 1u : 0u; break;
            case OP_U_GREATER_THAN_EQUAL: res_u = (a >= b) ? 1u : 0u; break;
            case OP_S_GREATER_THAN_EQUAL: res_u = ((int32_t)a >= (int32_t)b) ? 1u : 0u; break;
            case OP_SELECT: res_u = a ? b : c_v; break;
            default: res_u = a; break;
          }
          spv_const_val cv;
          cv.is_const = true;
          cv.is_bool = is_b;
          cv.b = (res_u != 0);
          cv.type_id = rt;
          cv.raw_words.push_back(res_u);
          ctx.consts[id] = std::move(cv);
          ctx.expr_types[id] = rt;
        }
      } break;
      case OP_CONSTANT_NULL:
      case OP_UNDEF: {
        spv_const_val c; c.is_const = true; c.type_id = w[0];
        ctx.consts[w[1]] = c;
        ctx.expr_types[w[1]] = w[0];
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

  auto type_location_slots = [](spv_ctx& c, u32 tid) -> u32 {
    auto self_fn = [&](auto& self, u32 t) -> u32 {
      if (!c.types.count(t)) return 1u;
      const auto& ty = c.types[t];
      if (ty.kind == spv_type::T_MAT) return std::max(1u, ty.count);
      if (ty.kind == spv_type::T_ARRAY) {
        u32 len = std::max(1u, c.const_u32(ty.len_const_id, 1u));
        return len * self(self, ty.comp_type);
      }
      return 1u;
    };
    return self_fn(self_fn, tid);
  };

  // Classify global OpVariables (only top-level ones outside functions)
  std::string vs_in_user_struct, vs_in_sys_struct, vs_out_struct;
  std::set<std::string> used_in_sems, used_out_sems;
  std::map<std::string, std::string> in_sem_to_member;
  std::vector<std::string> vs_in_copies, vs_out_copies, cs_in_copies;

  auto add_vs_in = [&](const std::string& line, const std::string& sem) {
    if (sem.rfind("SV_", 0) == 0) {
      vs_in_sys_struct += line;
    } else {
      vs_in_user_struct += line;
    }
  };
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
      u32 elem_pte = pte;
      while (ctx.types.count(elem_pte) &&
             (ctx.types[elem_pte].kind == spv_type::T_ARRAY ||
              ctx.types[elem_pte].kind == spv_type::T_RUNTIME_ARRAY)) {
        elem_pte = ctx.types[elem_pte].comp_type;
      }
      const auto& elem_pty = ctx.types[elem_pte];
      if (elem_pty.kind == spv_type::T_SAMPLED_IMAGE) {
        u32 tr = ctx.next_t++, sr = ctx.next_s++;
        u32 img_tid = elem_pty.comp_type;
        std::string itype = ctx.hlsl_type(img_tid);
        char dbuf[256];
        snprintf(dbuf, sizeof(dbuf), "%s %s : register(t%u);\nSamplerState %s_smp : register(s%u);\n",
                 itype.c_str(), gv.hlsl_name.c_str(), tr, gv.hlsl_name.c_str(), sr);
        ctx.decls_hlsl += dbuf;
        gv.hlsl_reg = tr; gv.smp_reg = sr;
        ctx.res.bindings.push_back({set, bind, SPV_BIND_COMBINED_IMG_SMP, tr, sr, 0, gv.hlsl_name});
      } else if (elem_pty.kind == spv_type::T_IMAGE && elem_pty.sampled == 2) {
        u32 ur = ctx.next_u++;
        char dbuf[256];
        snprintf(dbuf, sizeof(dbuf), "%s %s : register(u%u);\n",
                 ctx.hlsl_type(elem_pte).c_str(), gv.hlsl_name.c_str(), ur);
        ctx.decls_hlsl += dbuf;
        gv.hlsl_reg = ur;
        ctx.res.bindings.push_back({set, bind, SPV_BIND_STORAGE_IMG, ur, 0, 0, gv.hlsl_name});
      } else if (elem_pty.kind == spv_type::T_IMAGE) {
        u32 tr = ctx.next_t++;
        char dbuf[256];
        snprintf(dbuf, sizeof(dbuf), "%s %s : register(t%u);\n",
                 ctx.hlsl_type(elem_pte).c_str(), gv.hlsl_name.c_str(), tr);
        ctx.decls_hlsl += dbuf;
        gv.hlsl_reg = tr;
        ctx.res.bindings.push_back({set, bind, SPV_BIND_TEXTURE, tr, 0, 0, gv.hlsl_name});
      } else if (elem_pty.kind == spv_type::T_SAMPLER) {
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
      ctx.decls_hlsl += "groupshared " + ctx.hlsl_var_decl(pte, gv.hlsl_name, false) + ";\n";
    } else if (sc == SC_PRIVATE) {
      std::string ainit = (w.size() >= 4) ? ctx.format_array_init(w[3]) : "";
      ctx.decls_hlsl += "static " + ctx.hlsl_var_decl(pte, gv.hlsl_name, false) +
                        (ainit.empty() ? "" : (" = " + ainit)) + ";\n";
    } else if (sc == SC_INPUT) {
      u32 bi = 0xFFFFFFFFu;
      bool is_bi = (dec.builtin != 0xFFFFFFFFu) || has_builtin_in_type(ctx, pte, bi);
      if (dec.builtin != 0xFFFFFFFFu) bi = dec.builtin;
      if (is_bi) {
        if (ctx.active_ep.stage == SPV_STAGE_COMPUTE) {
          if (bi == BI_NUM_WORKGROUPS) {
            ctx.decls_hlsl += "static const uint3 " + gv.hlsl_name + " = uint3(65535, 65535, 65535);\n";
          } else {
            std::string sem = builtin_semantic(bi, SPV_STAGE_COMPUTE, false);
            std::string fty = (bi == BI_LOCAL_INVOCATION_INDEX) ? "uint" : "uint3";
            ctx.decls_hlsl += "static " + fty + " " + gv.hlsl_name + ";\n";
            if (in_sem_to_member.count(sem)) {
              cs_in_copies.push_back("  " + gv.hlsl_name + " = _in." + in_sem_to_member[sem] + ";\n");
            } else {
              in_sem_to_member[sem] = gv.hlsl_name;
              used_in_sems.insert(sem);
              add_vs_in("  " + fty + " " + gv.hlsl_name + " : " + sem + ";\n", sem);
              cs_in_copies.push_back("  " + gv.hlsl_name + " = _in." + gv.hlsl_name + ";\n");
            }
          }
        } else {
          std::string sem = builtin_semantic(bi, ctx.active_ep.stage, false);
          std::string hty = (bi == BI_VERTEX_INDEX || bi == BI_INSTANCE_INDEX) ? "uint"
                          : (bi == BI_FRONT_FACING ? "bool" : "float4");
          if (used_in_sems.count(sem)) {
            u32 slot = 0;
            while (used_in_sems.count("TEXCOORD" + std::to_string(slot))) slot++;
            sem = "TEXCOORD" + std::to_string(slot);
          }
          used_in_sems.insert(sem);
          add_vs_in("  " + hty + " " + gv.hlsl_name + " : " + sem + ";\n", sem);
          ctx.decls_hlsl += "static " + hty + " " + gv.hlsl_name + ";\n";
          vs_in_copies.push_back("  " + gv.hlsl_name + " = _in." + gv.hlsl_name + ";\n");
          ctx.res.inputs.push_back({0, bi, gv.hlsl_name, hty});
        }
      } else {
        u32 slots = type_location_slots(ctx, pte);
        u32 loc = (dec.location >= 0) ? (u32)dec.location : 0u;
        bool conflict = true;
        while (conflict) {
          conflict = false;
          for (u32 s = 0; s < slots; ++s) {
            std::string s_sem = "TEXCOORD" + std::to_string(loc + s);
            if (used_in_sems.count(s_sem)) {
              loc = loc + s + 1;
              conflict = true;
              break;
            }
          }
        }
        std::string sem = "TEXCOORD" + std::to_string(loc);
        for (u32 s = 0; s < slots; ++s) {
          used_in_sems.insert("TEXCOORD" + std::to_string(loc + s));
        }
        std::string hdecl = ctx.hlsl_var_decl(pte, gv.hlsl_name, false);
        add_vs_in("  " + hdecl + " : " + sem + ";\n", sem);
        ctx.decls_hlsl += "static " + hdecl + ";\n";
        vs_in_copies.push_back("  " + gv.hlsl_name + " = _in." + gv.hlsl_name + ";\n");
        ctx.res.inputs.push_back({loc, 0xFFFFFFFFu, gv.hlsl_name, ctx.hlsl_local_type(pte)});
      }
    } else if (sc == SC_OUTPUT) {
      u32 bi = 0xFFFFFFFFu;
      bool is_bi = (dec.builtin != 0xFFFFFFFFu) || has_builtin_in_type(ctx, pte, bi);
      if (dec.builtin != 0xFFFFFFFFu) bi = dec.builtin;
      if (is_bi && pty.kind == spv_type::T_STRUCT) {
        // Per-vertex gl_PerVertex block -> promote members to globals and copy Position to SV_Position
        std::string sdef = "struct " + ctx.hlsl_local_type(pte) + " {\n";
        for (u32 mi = 0; mi < (u32)pty.members.size(); ++mi) {
          sdef += "  " + ctx.hlsl_var_decl(pty.members[mi], cbuffer_member_expr(ctx, pte, mi), false) + ";\n";
        }
        sdef += "};\n";
        ctx.structs_hlsl += sdef;
        ctx.decls_hlsl += "static " + ctx.hlsl_local_type(pte) + " " + gv.hlsl_name + ";\n";
        std::string pos_field = cbuffer_member_expr(ctx, pte, 0);
        vs_out_struct += "  float4 gl_Position : SV_Position;\n";
        used_out_sems.insert("SV_Position");
        vs_out_copies.push_back("  _out.gl_Position = " + gv.hlsl_name + "." + pos_field + ";\n");
        vs_out_copies.push_back("  _out.gl_Position.y = -_out.gl_Position.y;\n");
        ctx.res.outputs.push_back({0, BI_POSITION, gv.hlsl_name, "float4"});
      } else if (is_bi) {
        std::string sem = builtin_semantic(bi, ctx.active_ep.stage, true);
        std::string hty = (bi == BI_FRAG_DEPTH) ? "float" : "float4";
        if (used_out_sems.count(sem)) {
          std::string prefix = (ctx.active_ep.stage == SPV_STAGE_FRAGMENT) ? "SV_Target" : "TEXCOORD";
          u32 slot = 0;
          while (used_out_sems.count(prefix + std::to_string(slot))) slot++;
          sem = prefix + std::to_string(slot);
        }
        used_out_sems.insert(sem);
        vs_out_struct += "  " + hty + " " + gv.hlsl_name + " : " + sem + ";\n";
        ctx.decls_hlsl += "static " + hty + " " + gv.hlsl_name + ";\n";
        vs_out_copies.push_back("  _out." + gv.hlsl_name + " = " + gv.hlsl_name + ";\n");
        if (bi == BI_POSITION) {
          vs_out_copies.push_back("  _out." + gv.hlsl_name + ".y = -_out." + gv.hlsl_name + ".y;\n");
        }
        ctx.res.outputs.push_back({0, bi, gv.hlsl_name, hty});
      } else {
        u32 slots = type_location_slots(ctx, pte);
        u32 loc = (dec.location >= 0) ? (u32)dec.location : 0u;
        std::string prefix = (ctx.active_ep.stage == SPV_STAGE_FRAGMENT) ? "SV_Target" : "TEXCOORD";
        bool conflict = true;
        while (conflict) {
          conflict = false;
          for (u32 s = 0; s < slots; ++s) {
            std::string s_sem = prefix + std::to_string(loc + s);
            if (used_out_sems.count(s_sem)) {
              loc = loc + s + 1;
              conflict = true;
              break;
            }
          }
        }
        std::string sem = prefix + std::to_string(loc);
        for (u32 s = 0; s < slots; ++s) {
          used_out_sems.insert(prefix + std::to_string(loc + s));
        }
        std::string hdecl = ctx.hlsl_var_decl(pte, gv.hlsl_name, false);
        vs_out_struct += "  " + hdecl + " : " + sem + ";\n";
        ctx.decls_hlsl += "static " + hdecl + ";\n";
        vs_out_copies.push_back("  _out." + gv.hlsl_name + " = " + gv.hlsl_name + ";\n");
        ctx.res.outputs.push_back({loc, 0xFFFFFFFFu, gv.hlsl_name, ctx.hlsl_local_type(pte)});
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
      sdef += "  " + ctx.hlsl_var_decl(kv.second.members[mi], cbuffer_member_expr(ctx, kv.first, mi), false) + ";\n";
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
      ctx.main_body_hlsl += "  " + ctx.hlsl_var_decl(pt, pv, true) + ";\n";
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
  std::string vs_in_struct = vs_in_user_struct + vs_in_sys_struct;
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
    if (!vs_out_struct.empty()) {
      hlsl += "  static const VSOut _zero_out;\n";
      hlsl += "  VSOut _out = _zero_out;\n";
    }
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
