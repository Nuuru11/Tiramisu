// tiramisu/src/trt/glsl_hlsl.cpp
// Complete GLSL (110..460 & ESSL 100/300/310) -> HLSL SM5 translator.
#include "glsl_hlsl.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

// ---- 1. Lightweight Preprocessor ----

struct macro_def {
  bool has_params = false;
  std::vector<std::string> params;
  std::string body;
};

struct pp_state {
  std::map<std::string, macro_def> macros;
  int version = 120;
  bool is_es = false;
};

static std::string strip_comments_and_join(const char* src, size_t len) {
  std::string joined;
  joined.reserve(len);
  for (size_t i = 0; i < len; ++i) {
    if (src[i] == '\\') {
      if (i + 1 < len && src[i + 1] == '\n') {
        ++i;
        continue;
      }
      if (i + 2 < len && src[i + 1] == '\r' && src[i + 2] == '\n') {
        i += 2;
        continue;
      }
    }
    joined.push_back(src[i]);
  }
  std::string out;
  out.reserve(joined.size());
  for (size_t i = 0; i < joined.size(); ++i) {
    if (i + 1 < joined.size() && joined[i] == '/' && joined[i + 1] == '/') {
      while (i < joined.size() && joined[i] != '\n') ++i;
      if (i < joined.size()) out.push_back('\n');
    } else if (i + 1 < joined.size() && joined[i] == '/' &&
               joined[i + 1] == '*') {
      i += 2;
      while (i + 1 < joined.size() &&
             !(joined[i] == '*' && joined[i + 1] == '/')) {
        if (joined[i] == '\n') out.push_back('\n');
        ++i;
      }
      if (i + 1 < joined.size()) ++i;
      out.push_back(' ');
    } else {
      out.push_back(joined[i]);
    }
  }
  return out;
}

static inline bool is_ident_start(char c) {
  return isalpha((unsigned char)c) || c == '_';
}
static inline bool is_ident_char(char c) {
  return isalnum((unsigned char)c) || c == '_';
}

static std::string trim_ws(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && isspace((unsigned char)s[a])) ++a;
  while (b > a && isspace((unsigned char)s[b - 1])) --b;
  return s.substr(a, b - a);
}

static std::string expand_macros(const std::string& line, const pp_state& pp,
                                 int depth = 0) {
  if (depth > 24) return line;
  std::string out;
  out.reserve(line.size());
  for (size_t i = 0; i < line.size();) {
    if (is_ident_start(line[i])) {
      size_t j = i + 1;
      while (j < line.size() && is_ident_char(line[j])) ++j;
      const std::string id = line.substr(i, j - i);
      auto it = pp.macros.find(id);
      if (it == pp.macros.end()) {
        out.append(id);
        i = j;
        continue;
      }
      const macro_def& md = it->second;
      if (!md.has_params) {
        out.append(expand_macros(md.body, pp, depth + 1));
        i = j;
        continue;
      }
      size_t k = j;
      while (k < line.size() && isspace((unsigned char)line[k])) ++k;
      if (k >= line.size() || line[k] != '(') {
        out.append(id);
        i = j;
        continue;
      }
      // Parse macro invocation arguments
      std::vector<std::string> args;
      size_t p = k + 1;
      int paren = 1;
      size_t arg_start = p;
      while (p < line.size() && paren > 0) {
        if (line[p] == '(') ++paren;
        else if (line[p] == ')') {
          --paren;
          if (paren == 0) {
            std::string a = trim_ws(line.substr(arg_start, p - arg_start));
            if (!a.empty() || !args.empty() || !md.params.empty()) args.push_back(a);
            break;
          }
        } else if (line[p] == ',' && paren == 1) {
          args.push_back(trim_ws(line.substr(arg_start, p - arg_start)));
          arg_start = p + 1;
        }
        ++p;
      }
      if (paren == 0) {
        std::string sub;
        const std::string& b = md.body;
        for (size_t bi = 0; bi < b.size();) {
          if (is_ident_start(b[bi])) {
            size_t bj = bi + 1;
            while (bj < b.size() && is_ident_char(b[bj])) ++bj;
            const std::string bid = b.substr(bi, bj - bi);
            int p_idx = -1;
            for (size_t pi = 0; pi < md.params.size(); ++pi)
              if (md.params[pi] == bid) { p_idx = (int)pi; break; }
            if (p_idx >= 0 && (size_t)p_idx < args.size())
              sub.append(args[p_idx]);
            else
              sub.append(bid);
            bi = bj;
          } else {
            sub.push_back(b[bi++]);
          }
        }
        out.append(expand_macros(sub, pp, depth + 1));
        i = p + 1;
        continue;
      }
      out.append(id);
      i = j;
    } else {
      out.push_back(line[i++]);
    }
  }
  return out;
}

// Evaluate simple preprocessor constant #if expression
struct pp_expr_eval {
  std::string s;
  size_t p = 0;
  const pp_state* pp = nullptr;
  void skip() { while (p < s.size() && isspace((unsigned char)s[p])) ++p; }
  bool match(const char* op) {
    skip();
    size_t n = strlen(op);
    if (s.compare(p, n, op) == 0) {
      if (strcmp(op, "<") == 0 && s.compare(p, 2, "<=") == 0) return false;
      if (strcmp(op, ">") == 0 && s.compare(p, 2, ">=") == 0) return false;
      if (strcmp(op, "!") == 0 && s.compare(p, 2, "!=") == 0) return false;
      if (strcmp(op, "&") == 0 && s.compare(p, 2, "&&") == 0) return false;
      if (strcmp(op, "|") == 0 && s.compare(p, 2, "||") == 0) return false;
      p += n;
      return true;
    }
    return false;
  }
  long long parse_primary() {
    skip();
    if (match("(")) {
      long long v = parse_or();
      match(")");
      return v;
    }
    if (s.compare(p, 7, "defined") == 0 &&
        (p + 7 >= s.size() || !is_ident_char(s[p + 7]))) {
      p += 7;
      skip();
      bool par = match("(");
      skip();
      size_t start = p;
      while (p < s.size() && is_ident_char(s[p])) ++p;
      std::string id = s.substr(start, p - start);
      if (par) match(")");
      return pp->macros.count(id) ? 1 : 0;
    }
    if (p < s.size() && isdigit((unsigned char)s[p])) {
      char* endp = nullptr;
      long long v = strtoll(s.c_str() + p, &endp, 0);
      p = (size_t)(endp - s.c_str());
      while (p < s.size() && (s[p] == 'u' || s[p] == 'U' || s[p] == 'l' || s[p] == 'L'))
        ++p;
      return v;
    }
    if (p < s.size() && is_ident_start(s[p])) {
      size_t start = p;
      while (p < s.size() && is_ident_char(s[p])) ++p;
      std::string id = s.substr(start, p - start);
      auto it = pp->macros.find(id);
      if (it != pp->macros.end() && !it->second.has_params) {
        pp_expr_eval sub{expand_macros(it->second.body, *pp), 0, pp};
        return sub.parse_or();
      }
      return 0;
    }
    return 0;
  }
  long long parse_unary() {
    if (match("!")) return !parse_unary();
    if (match("-")) return -parse_unary();
    if (match("+")) return parse_unary();
    if (match("~")) return ~parse_unary();
    return parse_primary();
  }
  long long parse_mul() {
    long long v = parse_unary();
    for (;;) {
      if (match("*")) v *= parse_unary();
      else if (match("/")) { long long r = parse_unary(); v = r ? v / r : 0; }
      else if (match("%")) { long long r = parse_unary(); v = r ? v % r : 0; }
      else break;
    }
    return v;
  }
  long long parse_add() {
    long long v = parse_mul();
    for (;;) {
      if (match("+")) v += parse_mul();
      else if (match("-")) v -= parse_mul();
      else break;
    }
    return v;
  }
  long long parse_rel() {
    long long v = parse_add();
    for (;;) {
      if (match("<=")) v = (v <= parse_add());
      else if (match(">=")) v = (v >= parse_add());
      else if (match("<")) v = (v < parse_add());
      else if (match(">")) v = (v > parse_add());
      else break;
    }
    return v;
  }
  long long parse_eq() {
    long long v = parse_rel();
    for (;;) {
      if (match("==")) v = (v == parse_rel());
      else if (match("!=")) v = (v != parse_rel());
      else break;
    }
    return v;
  }
  long long parse_and() {
    long long v = parse_eq();
    while (match("&&")) {
      long long r = parse_eq();
      v = (v && r) ? 1 : 0;
    }
    return v;
  }
  long long parse_or() {
    long long v = parse_and();
    while (match("||")) {
      long long r = parse_and();
      v = (v || r) ? 1 : 0;
    }
    return v;
  }
};

static std::string preprocess_glsl(const char* src, size_t len, pp_state& pp) {
  std::string clean = strip_comments_and_join(src, len);
  struct cond_frame { bool parent_active; bool this_taken; bool active; };
  std::vector<cond_frame> stack;
  auto is_active = [&]() -> bool {
    return stack.empty() ? true : stack.back().active;
  };

  std::string out;
  out.reserve(clean.size());
  size_t pos = 0;
  while (pos < clean.size()) {
    size_t eol = clean.find('\n', pos);
    if (eol == std::string::npos) eol = clean.size();
    std::string line = clean.substr(pos, eol - pos);
    pos = (eol < clean.size()) ? eol + 1 : clean.size();

    std::string t = trim_ws(line);
    if (!t.empty() && t[0] == '#') {
      size_t k = 1;
      while (k < t.size() && isspace((unsigned char)t[k])) ++k;
      size_t k2 = k;
      while (k2 < t.size() && is_ident_char(t[k2])) ++k2;
      const std::string dir = t.substr(k, k2 - k);
      const std::string rest = trim_ws(t.substr(k2));

      if (dir == "if") {
        bool p_act = is_active();
        bool cond = false;
        if (p_act) {
          pp_expr_eval ev{rest, 0, &pp};
          cond = ev.parse_or() != 0;
        }
        stack.push_back({p_act, cond, p_act && cond});
      } else if (dir == "ifdef" || dir == "ifndef") {
        bool p_act = is_active();
        size_t id_end = 0;
        while (id_end < rest.size() && is_ident_char(rest[id_end])) ++id_end;
        std::string sym = rest.substr(0, id_end);
        bool has = pp.macros.count(sym) != 0;
        if (dir == "ifndef") has = !has;
        stack.push_back({p_act, has, p_act && has});
      } else if (dir == "elif") {
        if (!stack.empty()) {
          auto& f = stack.back();
          if (f.parent_active && !f.this_taken) {
            pp_expr_eval ev{rest, 0, &pp};
            bool cond = ev.parse_or() != 0;
            f.this_taken = cond;
            f.active = cond;
          } else {
            f.active = false;
          }
        }
      } else if (dir == "else") {
        if (!stack.empty()) {
          auto& f = stack.back();
          f.active = f.parent_active && !f.this_taken;
          f.this_taken = true;
        }
      } else if (dir == "endif") {
        if (!stack.empty()) stack.pop_back();
      } else if (is_active()) {
        if (dir == "version") {
          pp.version = atoi(rest.c_str());
          if (rest.find("es") != std::string::npos) {
            pp.is_es = true;
            pp.macros["GL_ES"] = {false, {}, "1"};
          }
          char vb[16];
          snprintf(vb, sizeof(vb), "%d", pp.version);
          pp.macros["__VERSION__"] = {false, {}, vb};
        } else if (dir == "define") {
          size_t n_end = 0;
          while (n_end < rest.size() && is_ident_char(rest[n_end])) ++n_end;
          std::string name = rest.substr(0, n_end);
          if (!name.empty()) {
            macro_def md;
            size_t rpos = n_end;
            if (rpos < rest.size() && rest[rpos] == '(') {
              md.has_params = true;
              size_t close = rest.find(')', rpos + 1);
              if (close != std::string::npos) {
                std::string plist = rest.substr(rpos + 1, close - rpos - 1);
                size_t pcur = 0;
                while (pcur < plist.size()) {
                  size_t c = plist.find(',', pcur);
                  if (c == std::string::npos) c = plist.size();
                  std::string pn = trim_ws(plist.substr(pcur, c - pcur));
                  if (!pn.empty()) md.params.push_back(pn);
                  pcur = c + 1;
                }
                rpos = close + 1;
              }
            }
            md.body = trim_ws(rest.substr(rpos));
            pp.macros[name] = md;
          }
        } else if (dir == "undef") {
          size_t n_end = 0;
          while (n_end < rest.size() && is_ident_char(rest[n_end])) ++n_end;
          pp.macros.erase(rest.substr(0, n_end));
        }
      }
      out.push_back('\n');
    } else if (is_active()) {
      out.append(expand_macros(line, pp));
      out.push_back('\n');
    } else {
      out.push_back('\n');
    }
  }
  return out;
}

// ---- 2. Lexer ----

enum tok_type {
  TK_EOF = 0,
  TK_IDENT,
  TK_NUM,
  TK_SYM,
};

struct token {
  tok_type type = TK_EOF;
  std::string text;
};

static std::vector<token> tokenize_glsl(const std::string& src) {
  std::vector<token> toks;
  for (size_t i = 0; i < src.size();) {
    if (isspace((unsigned char)src[i])) {
      ++i;
      continue;
    }
    if (is_ident_start(src[i])) {
      size_t j = i + 1;
      while (j < src.size() && is_ident_char(src[j])) ++j;
      toks.push_back({TK_IDENT, src.substr(i, j - i)});
      i = j;
      continue;
    }
    if (isdigit((unsigned char)src[i]) ||
        (src[i] == '.' && i + 1 < src.size() && isdigit((unsigned char)src[i + 1]))) {
      size_t j = i;
      if (src[j] == '0' && j + 1 < src.size() &&
          (src[j + 1] == 'x' || src[j + 1] == 'X')) {
        j += 2;
        while (j < src.size() && isxdigit((unsigned char)src[j])) ++j;
      } else {
        while (j < src.size() && (isdigit((unsigned char)src[j]) || src[j] == '.'))
          ++j;
        if (j < src.size() && (src[j] == 'e' || src[j] == 'E')) {
          ++j;
          if (j < src.size() && (src[j] == '+' || src[j] == '-')) ++j;
          while (j < src.size() && isdigit((unsigned char)src[j])) ++j;
        }
      }
      while (j < src.size() &&
             (src[j] == 'f' || src[j] == 'F' || src[j] == 'u' || src[j] == 'U'))
        ++j;
      toks.push_back({TK_NUM, src.substr(i, j - i)});
      i = j;
      continue;
    }
    // 2/3 char operators
    static const char* const k_ops[] = {
        "<<=", ">>=", "++", "--", "<<", ">>", "<=", ">=", "==", "!=",
        "&&", "||", "^^", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
        nullptr};
    bool matched = false;
    for (int o = 0; k_ops[o]; ++o) {
      size_t olen = strlen(k_ops[o]);
      if (src.compare(i, olen, k_ops[o]) == 0) {
        toks.push_back({TK_SYM, k_ops[o]});
        i += olen;
        matched = true;
        break;
      }
    }
    if (!matched) {
      toks.push_back({TK_SYM, src.substr(i, 1)});
      ++i;
    }
  }
  toks.push_back({TK_EOF, ""});
  return toks;
}

// ---- 3. Type helpers & HLSL mapping ----

static glsl_val_kind parse_glsl_type(const std::string& t) {
  if (t == "float") return GLSL_T_FLOAT;
  if (t == "vec2") return GLSL_T_VEC2;
  if (t == "vec3") return GLSL_T_VEC3;
  if (t == "vec4") return GLSL_T_VEC4;
  if (t == "int") return GLSL_T_INT;
  if (t == "ivec2") return GLSL_T_IVEC2;
  if (t == "ivec3") return GLSL_T_IVEC3;
  if (t == "ivec4") return GLSL_T_IVEC4;
  if (t == "uint") return GLSL_T_UINT;
  if (t == "uvec2") return GLSL_T_UVEC2;
  if (t == "uvec3") return GLSL_T_UVEC3;
  if (t == "uvec4") return GLSL_T_UVEC4;
  if (t == "bool") return GLSL_T_BOOL;
  if (t == "bvec2") return GLSL_T_BVEC2;
  if (t == "bvec3") return GLSL_T_BVEC3;
  if (t == "bvec4") return GLSL_T_BVEC4;
  if (t == "mat2" || t == "mat2x2") return GLSL_T_MAT2;
  if (t == "mat3" || t == "mat3x3") return GLSL_T_MAT3;
  if (t == "mat4" || t == "mat4x4") return GLSL_T_MAT4;
  if (t == "sampler2D" || t == "isampler2D" || t == "usampler2D" ||
      t == "sampler1D")
    return GLSL_T_SAMPLER_2D;
  if (t == "samplerCube") return GLSL_T_SAMPLER_CUBE;
  if (t == "sampler3D") return GLSL_T_SAMPLER_3D;
  if (t == "sampler2DShadow") return GLSL_T_SAMPLER_2D_SHADOW;
  if (t == "sampler2DArray") return GLSL_T_SAMPLER_2D_ARRAY;
  if (t == "image2D" || t == "iimage2D" || t == "uimage2D")
    return GLSL_T_IMAGE_2D;
  return GLSL_T_UNKNOWN;
}

static bool is_sampler_kind(glsl_val_kind k) {
  return k >= GLSL_T_SAMPLER_2D && k <= GLSL_T_IMAGE_2D;
}
static bool is_matrix_kind(glsl_val_kind k) {
  return k >= GLSL_T_MAT2 && k <= GLSL_T_MAT4;
}

static const char* hlsl_type_str(glsl_val_kind k) {
  switch (k) {
    case GLSL_T_FLOAT: return "float";
    case GLSL_T_VEC2: return "float2";
    case GLSL_T_VEC3: return "float3";
    case GLSL_T_VEC4: return "float4";
    case GLSL_T_INT: return "int";
    case GLSL_T_IVEC2: return "int2";
    case GLSL_T_IVEC3: return "int3";
    case GLSL_T_IVEC4: return "int4";
    case GLSL_T_UINT: return "uint";
    case GLSL_T_UVEC2: return "uint2";
    case GLSL_T_UVEC3: return "uint3";
    case GLSL_T_UVEC4: return "uint4";
    case GLSL_T_BOOL: return "bool";
    case GLSL_T_BVEC2: return "bool2";
    case GLSL_T_BVEC3: return "bool3";
    case GLSL_T_BVEC4: return "bool4";
    case GLSL_T_MAT2: return "column_major float2x2";
    case GLSL_T_MAT3: return "column_major float3x3";
    case GLSL_T_MAT4: return "column_major float4x4";
    default: return "float4";
  }
}

static const char* hlsl_local_type_str(glsl_val_kind k) {
  if (k == GLSL_T_MAT2) return "float2x2";
  if (k == GLSL_T_MAT3) return "float3x3";
  if (k == GLSL_T_MAT4) return "float4x4";
  return hlsl_type_str(k);
}

static const char* hlsl_tex_decl_type(glsl_val_kind k) {
  switch (k) {
    case GLSL_T_SAMPLER_CUBE: return "TextureCube";
    case GLSL_T_SAMPLER_3D: return "Texture3D";
    case GLSL_T_SAMPLER_2D_ARRAY: return "Texture2DArray";
    case GLSL_T_IMAGE_2D: return "RWTexture2D<float4>";
    default: return "Texture2D";
  }
}

static u32 hlsl_var_raw_bytes(glsl_val_kind k, u32 arr) {
  u32 base = 4;
  switch (k) {
    case GLSL_T_FLOAT: case GLSL_T_INT: case GLSL_T_UINT: case GLSL_T_BOOL:
      base = 4; break;
    case GLSL_T_VEC2: case GLSL_T_IVEC2: case GLSL_T_UVEC2: case GLSL_T_BVEC2:
      base = 8; break;
    case GLSL_T_VEC3: case GLSL_T_IVEC3: case GLSL_T_UVEC3: case GLSL_T_BVEC3:
      base = 12; break;
    case GLSL_T_VEC4: case GLSL_T_IVEC4: case GLSL_T_UVEC4: case GLSL_T_BVEC4:
      base = 16; break;
    case GLSL_T_MAT2: base = 32; break; // 2 float2 columns -> 2x 16B slots in cbuffer
    case GLSL_T_MAT3: base = 48; break;
    case GLSL_T_MAT4: base = 64; break;
    default: base = 16; break;
  }
  if (arr <= 1) return base;
  // HLSL cbuffer array elements are 16-byte padded per element
  u32 elem_stride = (base <= 16) ? 16 : base;
  return elem_stride * (arr - 1) + base;
}

static u32 hlsl_cbuffer_alloc(u32& cursor, glsl_val_kind k, u32 arr) {
  const u32 sz = hlsl_var_raw_bytes(k, arr);
  if (arr <= 1 && sz <= 16) {
    const u32 row_rem = 16u - (cursor & 15u);
    if (sz > row_rem) cursor = (cursor + 15u) & ~15u;
  } else {
    cursor = (cursor + 15u) & ~15u;
  }
  const u32 off = cursor;
  cursor += sz;
  return off;
}

// ---- 4. Translator / Compiler Context ----

struct glsl_compiler {
  std::vector<token> toks;
  size_t pos = 0;
  glsl_compile_result res;
  const glsl_link_bindings* link = nullptr;

  std::map<std::string, glsl_val_kind> sym_types;
  std::map<std::string, std::string> rename_map;
  std::map<std::string, std::string> struct_defs;
  std::set<std::string> sampler_names;
  std::set<std::string> image_names;
  std::set<std::string> frag_out_names;
  std::set<std::string> vs_in_names;
  std::set<std::string> vs_out_names;

  bool compat_mv = false;
  bool compat_proj = false;
  bool compat_mvp = false;
  bool compat_normal = false;
  bool compat_vtx = false;
  bool compat_nrm = false;
  bool compat_col = false;
  bool compat_tc[8] = {};
  bool compat_out_col = false;
  bool compat_out_tc[8] = {};
  bool frag_color_used = false;
  bool frag_data_used = false;
  bool point_coord_used = false;

  u32 cb0_cursor = 0;
  int next_tex_slot = 0;
  int next_uav_slot = 0;
  int next_ubo_slot = 0;
  int next_var_slot = 0;

  std::string globals_hlsl;
  std::string cbuffer_hlsl;
  std::string funcs_hlsl;
  std::string main_hlsl;

  const token& cur() const {
    return (pos < toks.size()) ? toks[pos] : toks.back();
  }
  const token& peek(size_t off = 1) const {
    return (pos + off < toks.size()) ? toks[pos + off] : toks.back();
  }
  bool at(const char* s) const { return cur().text == s; }
  bool match(const char* s) {
    if (cur().text == s) { ++pos; return true; }
    return false;
  }
  token next() {
    token t = cur();
    if (pos + 1 < toks.size()) ++pos;
    return t;
  }

  int lookup_binding(const std::vector<glsl_link_bindings::pair>& v,
                     const std::string& n) const {
    for (const auto& p : v) if (p.name && n == p.name) return p.loc;
    return -1;
  }

  int alloc_input_loc(const std::string& name, int explicit_loc) {
    if (explicit_loc >= 0) return explicit_loc;
    if (link) {
      int b = lookup_binding(link->attrib_locs, name);
      if (b >= 0) return b;
      if (res.stage == GLSL_STAGE_FRAGMENT) {
        int vb = lookup_binding(link->varying_locs, name);
        if (vb >= 0) return vb;
      }
    }
    int used[32] = {};
    for (const auto& iv : res.inputs)
      if (iv.location >= 0 && iv.location < 32) used[iv.location] = 1;
    for (int i = 0; i < 32; ++i) if (!used[i]) return i;
    return 0;
  }

  int alloc_output_loc(const std::string& name, int explicit_loc) {
    if (explicit_loc >= 0) return explicit_loc;
    if (link) {
      if (res.stage == GLSL_STAGE_FRAGMENT) {
        int fb = lookup_binding(link->frag_locs, name);
        if (fb >= 0) return fb;
      } else if (res.stage == GLSL_STAGE_VERTEX) {
        int vb = lookup_binding(link->varying_locs, name);
        if (vb >= 0) return vb;
      }
    }
    return next_var_slot++;
  }

  // Translate an expression or parenthesized/bracketed chunk tracking matrix/type ops
  struct expr_out {
    std::string s;
    glsl_val_kind kind = GLSL_T_UNKNOWN;
  };

  std::string rewrite_tex_call(const std::string& fn,
                               const std::vector<expr_out>& args) {
    auto tex_name = [&](const expr_out& a) -> std::string { return trim_ws(a.s); };
    if ((fn == "texture" || fn == "texture2D" || fn == "textureCube" ||
         fn == "texture3D" || fn == "texture2DArray") &&
        args.size() >= 2) {
      const std::string tn = tex_name(args[0]);
      if (args.size() == 2)
        return tn + ".Sample(" + tn + "_smp, " + args[1].s + ")";
      return tn + ".SampleBias(" + tn + "_smp, " + args[1].s + ", " + args[2].s + ")";
    }
    if ((fn == "textureLod" || fn == "texture2DLod" || fn == "textureCubeLod") &&
        args.size() >= 3) {
      const std::string tn = tex_name(args[0]);
      return tn + ".SampleLevel(" + tn + "_smp, " + args[1].s + ", " + args[2].s + ")";
    }
    if ((fn == "textureGrad" || fn == "texture2DGrad") && args.size() >= 4) {
      const std::string tn = tex_name(args[0]);
      return tn + ".SampleGrad(" + tn + "_smp, " + args[1].s + ", " +
             args[2].s + ", " + args[3].s + ")";
    }
    if (fn == "textureProj" || fn == "texture2DProj") {
      if (args.size() >= 2) {
        const std::string tn = tex_name(args[0]);
        return tn + ".Sample(" + tn + "_smp, (" + args[1].s + ").xy / (" +
               args[1].s + ").w)";
      }
    }
    if ((fn == "shadow2D" || fn == "texture2DShadow") && args.size() >= 2) {
      const std::string tn = tex_name(args[0]);
      return "float4(" + tn + ".SampleCmpLevelZero(" + tn + "_smp, (" +
             args[1].s + ").xy, (" + args[1].s + ").z).xxx, 1.0)";
    }
    if (fn == "texelFetch" && args.size() >= 2) {
      const std::string tn = tex_name(args[0]);
      std::string lod = (args.size() >= 3) ? args[2].s : "0";
      return tn + ".Load(int3(" + args[1].s + ", " + lod + "))";
    }
    if (fn == "textureSize" && args.size() >= 2) {
      const std::string tn = tex_name(args[0]);
      return "_gl_texSize_" + tn + "(" + args[1].s + ")";
    }
    if (fn == "imageLoad" && args.size() >= 2) {
      return tex_name(args[0]) + "[" + args[1].s + "]";
    }
    if (fn == "imageStore" && args.size() >= 3) {
      return "(" + tex_name(args[0]) + "[" + args[1].s + "] = " + args[2].s + ")";
    }
    return "";
  }

  static std::string map_builtin_fn(const std::string& fn,
                                    const std::vector<expr_out>& args) {
    if (fn == "mix") return "lerp";
    if (fn == "fract") return "frac";
    if (fn == "inversesqrt") return "rsqrt";
    if (fn == "dFdx") return "ddx";
    if (fn == "dFdy") return "ddy";
    if (fn == "atan" && args.size() == 2) return "atan2";
    if (fn == "floatBitsToInt") return "asint";
    if (fn == "floatBitsToUint") return "asuint";
    if (fn == "intBitsToFloat" || fn == "uintBitsToFloat") return "asfloat";
    if (fn == "barrier" || fn == "groupMemoryBarrier" ||
        fn == "memoryBarrierShared")
      return "GroupMemoryBarrierWithGroupSync";
    if (fn == "memoryBarrier" || fn == "memoryBarrierImage" ||
        fn == "memoryBarrierBuffer")
      return "AllMemoryBarrierWithGroupSync";
    return "";
  }

  expr_out parse_expr();
  expr_out parse_assign();
  expr_out parse_cond();
  expr_out parse_or();
  expr_out parse_and();
  expr_out parse_bitor();
  expr_out parse_bitxor();
  expr_out parse_bitand();
  expr_out parse_eq();
  expr_out parse_rel();
  expr_out parse_shift();
  expr_out parse_add();
  expr_out parse_mul();
  expr_out parse_unary();
  expr_out parse_postfix();
  expr_out parse_primary();

  void note_builtin_ref(const std::string& id) {
    if (id == "gl_ModelViewMatrix") compat_mv = true;
    else if (id == "gl_ProjectionMatrix") compat_proj = true;
    else if (id == "gl_ModelViewProjectionMatrix") compat_mvp = true;
    else if (id == "gl_NormalMatrix") compat_normal = true;
    else if (id == "gl_Vertex") compat_vtx = true;
    else if (id == "gl_Normal") compat_nrm = true;
    else if (id == "gl_Color" || id == "gl_FrontColor") compat_col = true;
    else if (id == "gl_MultiTexCoord0") compat_tc[0] = true;
    else if (id == "gl_MultiTexCoord1") compat_tc[1] = true;
    else if (id == "gl_MultiTexCoord2") compat_tc[2] = true;
    else if (id == "gl_MultiTexCoord3") compat_tc[3] = true;
    else if (id == "gl_FragCoord") res.uses_frag_coord = true;
    else if (id == "gl_FrontFacing") res.uses_front_facing = true;
    else if (id == "gl_FragDepth") res.writes_frag_depth = true;
    else if (id == "gl_FragColor") frag_color_used = true;
    else if (id == "gl_FragData") frag_data_used = true;
    else if (id == "gl_PointCoord") point_coord_used = true;
  }

  std::string translate_ident(const std::string& id, glsl_val_kind* out_k) {
    note_builtin_ref(id);
    if (id == "gl_Position") { *out_k = GLSL_T_VEC4; return "_out.gl_Position"; }
    if (id == "gl_FragColor") { *out_k = GLSL_T_VEC4; return "_out.gl_FragColor"; }
    if (id == "gl_FragData") { *out_k = GLSL_T_VEC4; return "_out.gl_FragData"; }
    if (id == "gl_FragDepth") { *out_k = GLSL_T_FLOAT; return "_out.gl_FragDepth"; }
    if (id == "gl_FragCoord") { *out_k = GLSL_T_VEC4; return "_in.gl_FragCoord"; }
    if (id == "gl_FrontFacing") { *out_k = GLSL_T_BOOL; return "_in.gl_FrontFacing"; }
    if (id == "gl_PointCoord") { *out_k = GLSL_T_VEC2; return "_in.gl_PointCoord"; }
    if (id == "gl_GlobalInvocationID") { *out_k = GLSL_T_UVEC3; return "_cs_gid"; }
    if (id == "gl_LocalInvocationID") { *out_k = GLSL_T_UVEC3; return "_cs_tid"; }
    if (id == "gl_WorkGroupID") { *out_k = GLSL_T_UVEC3; return "_cs_wgid"; }
    if (id == "gl_LocalInvocationIndex") { *out_k = GLSL_T_UINT; return "_cs_gidx"; }
    if (id == "gl_ModelViewProjectionMatrix") { *out_k = GLSL_T_MAT4; return "gl_ModelViewProjectionMatrix"; }
    if (id == "gl_ModelViewMatrix") { *out_k = GLSL_T_MAT4; return "gl_ModelViewMatrix"; }
    if (id == "gl_ProjectionMatrix") { *out_k = GLSL_T_MAT4; return "gl_ProjectionMatrix"; }
    if (id == "gl_NormalMatrix") { *out_k = GLSL_T_MAT3; return "gl_NormalMatrix"; }
    if (id == "gl_Vertex") { *out_k = GLSL_T_VEC4; return "_in.gl_Vertex"; }
    if (id == "gl_Normal") { *out_k = GLSL_T_VEC3; return "_in.gl_Normal"; }
    if (id == "gl_Color") { *out_k = GLSL_T_VEC4; return "_in.gl_Color"; }
    if (id == "gl_FrontColor") { *out_k = GLSL_T_VEC4; compat_out_col = true; return "_out.gl_FrontColor"; }
    if (id == "gl_TexCoord") { *out_k = GLSL_T_VEC4; return "_gl_tc_arr"; }
    if (id.rfind("gl_MultiTexCoord", 0) == 0 && id.size() == 17 &&
        isdigit((unsigned char)id[16])) {
      int u = id[16] - '0';
      if (u >= 0 && u < 8) compat_tc[u] = true;
      *out_k = GLSL_T_VEC4;
      return "_in." + id;
    }
    auto st = sym_types.find(id);
    if (st != sym_types.end()) *out_k = st->second;
    auto rm = rename_map.find(id);
    return (rm != rename_map.end()) ? rm->second : id;
  }

  std::string ctor_helper_name(glsl_val_kind k) {
    switch (k) {
      case GLSL_T_FLOAT: return "float";
      case GLSL_T_INT: return "int";
      case GLSL_T_UINT: return "uint";
      case GLSL_T_BOOL: return "bool";
      case GLSL_T_VEC2: return "_v2";
      case GLSL_T_VEC3: return "_v3";
      case GLSL_T_VEC4: return "_v4";
      case GLSL_T_IVEC2: return "_iv2";
      case GLSL_T_IVEC3: return "_iv3";
      case GLSL_T_IVEC4: return "_iv4";
      case GLSL_T_UVEC2: return "_uv2";
      case GLSL_T_UVEC3: return "_uv3";
      case GLSL_T_UVEC4: return "_uv4";
      case GLSL_T_BVEC2: return "_bv2";
      case GLSL_T_BVEC3: return "_bv3";
      case GLSL_T_BVEC4: return "_bv4";
      case GLSL_T_MAT2: return "_m2";
      case GLSL_T_MAT3: return "_m3";
      case GLSL_T_MAT4: return "_m4";
      default: return "float4";
    }
  }

  std::string parse_stmt(const std::string& ind);
  std::string parse_block(const std::string& ind);
  bool parse_top_level();
};

glsl_compiler::expr_out glsl_compiler::parse_primary() {
  if (at("(")) {
    next();
    expr_out e = parse_expr();
    match(")");
    return {"(" + e.s + ")", e.kind};
  }
  if (cur().type == TK_NUM) {
    token t = next();
    glsl_val_kind k = GLSL_T_INT;
    if (t.text.find('.') != std::string::npos ||
        t.text.find('e') != std::string::npos ||
        t.text.find('E') != std::string::npos ||
        t.text.back() == 'f' || t.text.back() == 'F')
      k = GLSL_T_FLOAT;
    else if (t.text.back() == 'u' || t.text.back() == 'U')
      k = GLSL_T_UINT;
    return {t.text, k};
  }
  if (cur().type == TK_IDENT) {
    std::string id = next().text;
    if (id == "true" || id == "false") return {id, GLSL_T_BOOL};
    glsl_val_kind k = GLSL_T_UNKNOWN;
    std::string mapped = translate_ident(id, &k);
    return {mapped, k};
  }
  return {"", GLSL_T_UNKNOWN};
}

glsl_compiler::expr_out glsl_compiler::parse_postfix() {
  // Check type constructor call: vec4(...), mat4(...), etc.
  if (cur().type == TK_IDENT && peek().text == "(") {
    glsl_val_kind tk = parse_glsl_type(cur().text);
    std::string raw_fn = next().text;
    next(); // '('
    std::vector<expr_out> args;
    if (!at(")")) {
      for (;;) {
        args.push_back(parse_assign());
        if (!match(",")) break;
      }
    }
    match(")");
    if (tk != GLSL_T_UNKNOWN) {
      std::string out = ctor_helper_name(tk) + "(";
      for (size_t i = 0; i < args.size(); ++i) {
        if (i) out += ", ";
        out += args[i].s;
      }
      out += ")";
      return {out, tk};
    }
    std::string tx = rewrite_tex_call(raw_fn, args);
    if (!tx.empty()) return {tx, GLSL_T_VEC4};
    std::string mf = map_builtin_fn(raw_fn, args);
    if (!mf.empty()) raw_fn = mf;
    else if (raw_fn == "mod" && args.size() == 2) {
      return {"_gl_mod(" + args[0].s + ", " + args[1].s + ")", args[0].kind};
    } else if (raw_fn == "greaterThan" && args.size() == 2) {
      return {"((" + args[0].s + ") > (" + args[1].s + "))", GLSL_T_BVEC4};
    } else if (raw_fn == "lessThan" && args.size() == 2) {
      return {"((" + args[0].s + ") < (" + args[1].s + "))", GLSL_T_BVEC4};
    } else if (raw_fn == "greaterThanEqual" && args.size() == 2) {
      return {"((" + args[0].s + ") >= (" + args[1].s + "))", GLSL_T_BVEC4};
    } else if (raw_fn == "lessThanEqual" && args.size() == 2) {
      return {"((" + args[0].s + ") <= (" + args[1].s + "))", GLSL_T_BVEC4};
    } else if (raw_fn == "equal" && args.size() == 2) {
      return {"((" + args[0].s + ") == (" + args[1].s + "))", GLSL_T_BVEC4};
    } else if (raw_fn == "notEqual" && args.size() == 2) {
      return {"((" + args[0].s + ") != (" + args[1].s + "))", GLSL_T_BVEC4};
    } else if (raw_fn == "not" && args.size() == 1) {
      return {"(!(" + args[0].s + "))", args[0].kind};
    } else if (raw_fn == "inverse" && args.size() == 1) {
      return {"transpose(" + args[0].s + ")", args[0].kind};
    }
    glsl_val_kind rk = args.empty() ? GLSL_T_FLOAT : args[0].kind;
    auto st = sym_types.find(raw_fn);
    if (st != sym_types.end()) rk = st->second;
    std::string out = raw_fn + "(";
    for (size_t i = 0; i < args.size(); ++i) {
      if (i) out += ", ";
      out += args[i].s;
    }
    out += ")";
    return {out, rk};
  }

  expr_out e = parse_primary();
  for (;;) {
    if (match(".")) {
      std::string fld = next().text;
      if (fld == "length" && match("(")) {
        match(")");
        e.s = "0";
        e.kind = GLSL_T_INT;
      } else {
        // Normalize GLSL stpq / rgba aliases to HLSL xyzw safely when not struct member
        e.s += "." + fld;
        if (fld.size() == 1) e.kind = GLSL_T_FLOAT;
        else if (fld.size() == 2) e.kind = GLSL_T_VEC2;
        else if (fld.size() == 3) e.kind = GLSL_T_VEC3;
        else if (fld.size() == 4) e.kind = GLSL_T_VEC4;
      }
    } else if (match("[")) {
      expr_out idx = parse_expr();
      match("]");
      if (e.s == "_gl_tc_arr") {
        int slot = atoi(idx.s.c_str());
        if (slot < 0 || slot >= 8) slot = 0;
        if (res.stage == GLSL_STAGE_VERTEX) {
          compat_out_tc[slot] = true;
          e.s = "_out.gl_TexCoord" + std::to_string(slot);
        } else {
          compat_out_tc[slot] = true;
          e.s = "_in.gl_TexCoord" + std::to_string(slot);
        }
        e.kind = GLSL_T_VEC4;
      } else if (e.s == "_out.gl_FragData") {
        int slot = atoi(idx.s.c_str());
        if (slot < 0 || slot >= 8) slot = 0;
        e.s = "_out.gl_FragData" + std::to_string(slot);
        e.kind = GLSL_T_VEC4;
      } else {
        e.s += "[" + idx.s + "]";
        if (e.kind == GLSL_T_MAT4) e.kind = GLSL_T_VEC4;
        else if (e.kind == GLSL_T_MAT3) e.kind = GLSL_T_VEC3;
        else if (e.kind == GLSL_T_MAT2) e.kind = GLSL_T_VEC2;
      }
    } else if (at("++") || at("--")) {
      e.s += next().text;
    } else {
      break;
    }
  }
  return e;
}

glsl_compiler::expr_out glsl_compiler::parse_unary() {
  if (at("+") || at("-") || at("!") || at("~") || at("++") || at("--")) {
    std::string op = next().text;
    expr_out r = parse_unary();
    return {op + r.s, r.kind};
  }
  return parse_postfix();
}

glsl_compiler::expr_out glsl_compiler::parse_mul() {
  expr_out l = parse_unary();
  while (at("*") || at("/") || at("%")) {
    std::string op = next().text;
    expr_out r = parse_unary();
    if (op == "*" && (is_matrix_kind(l.kind) || is_matrix_kind(r.kind))) {
      glsl_val_kind rk = l.kind;
      if (is_matrix_kind(l.kind) && !is_matrix_kind(r.kind)) rk = r.kind;
      else if (!is_matrix_kind(l.kind) && is_matrix_kind(r.kind)) rk = l.kind;
      l.s = "mul(" + l.s + ", " + r.s + ")";
      l.kind = rk;
    } else {
      l.s += " " + op + " " + r.s;
      if (l.kind == GLSL_T_UNKNOWN || l.kind == GLSL_T_FLOAT) l.kind = r.kind;
    }
  }
  return l;
}

glsl_compiler::expr_out glsl_compiler::parse_add() {
  expr_out l = parse_mul();
  while (at("+") || at("-")) {
    std::string op = next().text;
    expr_out r = parse_mul();
    l.s += " " + op + " " + r.s;
  }
  return l;
}

glsl_compiler::expr_out glsl_compiler::parse_shift() {
  expr_out l = parse_add();
  while (at("<<") || at(">>")) {
    std::string op = next().text;
    expr_out r = parse_add();
    l.s += " " + op + " " + r.s;
  }
  return l;
}

glsl_compiler::expr_out glsl_compiler::parse_rel() {
  expr_out l = parse_shift();
  while (at("<") || at(">") || at("<=") || at(">=")) {
    std::string op = next().text;
    expr_out r = parse_shift();
    l.s += " " + op + " " + r.s;
    l.kind = GLSL_T_BOOL;
  }
  return l;
}

glsl_compiler::expr_out glsl_compiler::parse_eq() {
  expr_out l = parse_rel();
  while (at("==") || at("!=")) {
    std::string op = next().text;
    expr_out r = parse_rel();
    l.s += " " + op + " " + r.s;
    l.kind = GLSL_T_BOOL;
  }
  return l;
}

glsl_compiler::expr_out glsl_compiler::parse_bitand() {
  expr_out l = parse_eq();
  while (match("&")) {
    expr_out r = parse_eq();
    l.s += " & " + r.s;
  }
  return l;
}

glsl_compiler::expr_out glsl_compiler::parse_bitxor() {
  expr_out l = parse_bitand();
  while (match("^")) {
    expr_out r = parse_bitand();
    l.s += " ^ " + r.s;
  }
  return l;
}

glsl_compiler::expr_out glsl_compiler::parse_bitor() {
  expr_out l = parse_bitxor();
  while (match("|")) {
    expr_out r = parse_bitxor();
    l.s += " | " + r.s;
  }
  return l;
}

glsl_compiler::expr_out glsl_compiler::parse_and() {
  expr_out l = parse_bitor();
  while (match("&&")) {
    expr_out r = parse_bitor();
    l.s += " && " + r.s;
    l.kind = GLSL_T_BOOL;
  }
  return l;
}

glsl_compiler::expr_out glsl_compiler::parse_or() {
  expr_out l = parse_and();
  while (at("||") || at("^^")) {
    std::string op = next().text;
    expr_out r = parse_and();
    l.s += (op == "^^" ? " != " : " || ") + r.s;
    l.kind = GLSL_T_BOOL;
  }
  return l;
}

glsl_compiler::expr_out glsl_compiler::parse_cond() {
  expr_out c = parse_or();
  if (match("?")) {
    expr_out a = parse_expr();
    match(":");
    expr_out b = parse_cond();
    return {"(" + c.s + " ? " + a.s + " : " + b.s + ")", a.kind};
  }
  return c;
}

glsl_compiler::expr_out glsl_compiler::parse_assign() {
  expr_out l = parse_cond();
  if (at("=") || at("+=") || at("-=") || at("*=") || at("/=") ||
      at("%=") || at("&=") || at("|=") || at("^=") || at("<<=") || at(">>=")) {
    std::string op = next().text;
    expr_out r = parse_assign();
    if (op == "*=" && (is_matrix_kind(l.kind) || is_matrix_kind(r.kind))) {
      return {l.s + " = mul(" + l.s + ", " + r.s + ")", l.kind};
    }
    return {l.s + " " + op + " " + r.s, l.kind};
  }
  return l;
}

glsl_compiler::expr_out glsl_compiler::parse_expr() {
  expr_out e = parse_assign();
  while (match(",")) {
    expr_out r = parse_assign();
    e.s += ", " + r.s;
    e.kind = r.kind;
  }
  return e;
}

static bool is_decl_qualifier(const std::string& w) {
  return w == "const" || w == "in" || w == "out" || w == "inout" ||
         w == "uniform" || w == "varying" || w == "attribute" ||
         w == "highp" || w == "mediump" || w == "lowp" ||
         w == "flat" || w == "smooth" || w == "noperspective" ||
         w == "centroid" || w == "invariant" || w == "readonly" ||
         w == "writeonly" || w == "coherent" || w == "shared";
}

std::string glsl_compiler::parse_stmt(const std::string& ind) {
  if (cur().type == TK_EOF) return "";
  if (at("{")) return parse_block(ind);
  if (match(";")) return ind + ";\n";
  if (match("discard")) {
    match(";");
    return ind + "clip(-1);\n";
  }
  if (match("return")) {
    if (match(";")) return ind + "return;\n";
    expr_out e = parse_expr();
    match(";");
    return ind + "return " + e.s + ";\n";
  }
  if (match("break")) { match(";"); return ind + "break;\n"; }
  if (match("continue")) { match(";"); return ind + "continue;\n"; }
  if (match("if")) {
    match("(");
    expr_out c = parse_expr();
    match(")");
    std::string out = ind + "if (" + c.s + ") ";
    std::string t = parse_stmt(ind);
    out += (t.rfind(ind, 0) == 0 && !t.empty() && t[ind.size()] == '{')
               ? t.substr(ind.size())
               : ("\n" + t);
    if (match("else")) {
      out += ind + "else ";
      std::string e = parse_stmt(ind);
      out += (e.rfind(ind, 0) == 0 && !e.empty() && e[ind.size()] == '{')
                 ? e.substr(ind.size())
                 : ("\n" + e);
    }
    return out;
  }
  if (match("while")) {
    match("(");
    expr_out c = parse_expr();
    match(")");
    return ind + "while (" + c.s + ") " + parse_stmt(ind);
  }
  if (match("for")) {
    match("(");
    std::string init;
    if (!at(";")) {
      if (is_decl_qualifier(cur().text) ||
          parse_glsl_type(cur().text) != GLSL_T_UNKNOWN) {
        init = trim_ws(parse_stmt(""));
        if (!init.empty() && init.back() == ';') init.pop_back();
      } else {
        init = parse_expr().s;
        match(";");
      }
    } else {
      match(";");
    }
    std::string cond = at(";") ? "" : parse_expr().s;
    match(";");
    std::string step = at(")") ? "" : parse_expr().s;
    match(")");
    return ind + "for (" + init + "; " + cond + "; " + step + ") " +
           parse_stmt(ind);
  }

  // Local variable declaration check
  {
    size_t save = pos;
    bool is_const = false;
    while (cur().type == TK_IDENT && is_decl_qualifier(cur().text)) {
      if (cur().text == "const") is_const = true;
      next();
    }
    if (cur().type == TK_IDENT &&
        (parse_glsl_type(cur().text) != GLSL_T_UNKNOWN ||
         struct_defs.count(cur().text))) {
      std::string tname = next().text;
      glsl_val_kind vk = parse_glsl_type(tname);
      std::string htype =
          vk != GLSL_T_UNKNOWN ? hlsl_local_type_str(vk) : tname;
      std::string line = ind + (is_const ? "const " : "") + htype + " ";
      for (int vi = 0;; ++vi) {
        if (cur().type != TK_IDENT) break;
        std::string vn = next().text;
        sym_types[vn] = vk;
        if (vi) line += ", ";
        line += vn;
        while (match("[")) {
          if (at("]")) { match("]"); }
          else {
            expr_out sz = parse_expr();
            match("]");
            line += "[" + sz.s + "]";
          }
        }
        if (match("=")) {
          expr_out init = parse_assign();
          line += " = " + init.s;
        }
        if (!match(",")) break;
      }
      match(";");
      return line + ";\n";
    }
    pos = save;
  }

  expr_out e = parse_expr();
  match(";");
  return ind + e.s + ";\n";
}

std::string glsl_compiler::parse_block(const std::string& ind) {
  match("{");
  std::string out = ind + "{\n";
  while (cur().type != TK_EOF && !at("}")) {
    out += parse_stmt(ind + "  ");
  }
  match("}");
  out += ind + "}\n";
  return out;
}

bool glsl_compiler::parse_top_level() {
  while (cur().type != TK_EOF) {
    // Parse layout(...) qualifier if present
    int explicit_loc = -1;
    int explicit_binding = -1;
    while (at("layout") || is_decl_qualifier(cur().text) || at("precision")) {
      if (match("precision")) {
        while (cur().type != TK_EOF && !at(";")) next();
        match(";");
        continue;
      }
      if (match("layout")) {
        match("(");
        int depth = 1;
        while (cur().type != TK_EOF && depth > 0) {
          if (at("(")) { ++depth; next(); }
          else if (at(")")) { --depth; next(); }
          else if (at("location") && peek().text == "=") {
            next(); next();
            explicit_loc = atoi(next().text.c_str());
          } else if (at("binding") && peek().text == "=") {
            next(); next();
            explicit_binding = atoi(next().text.c_str());
          } else if (at("local_size_x") && peek().text == "=") {
            next(); next();
            res.local_size[0] = (u32)atoi(next().text.c_str());
          } else if (at("local_size_y") && peek().text == "=") {
            next(); next();
            res.local_size[1] = (u32)atoi(next().text.c_str());
          } else if (at("local_size_z") && peek().text == "=") {
            next(); next();
            res.local_size[2] = (u32)atoi(next().text.c_str());
          } else {
            next();
          }
        }
        continue;
      }
      break;
    }

    if (match(";")) continue;
    if (cur().type == TK_EOF) break;

    // Collect storage qualifiers
    bool q_uniform = false, q_in = false, q_out = false, q_varying = false,
         q_attr = false, q_const = false, q_shared = false, q_buffer = false;
    while (cur().type == TK_IDENT &&
           (is_decl_qualifier(cur().text) || cur().text == "buffer")) {
      std::string q = next().text;
      if (q == "uniform") q_uniform = true;
      else if (q == "in") q_in = true;
      else if (q == "out") q_out = true;
      else if (q == "varying") q_varying = true;
      else if (q == "attribute") q_attr = true;
      else if (q == "const") q_const = true;
      else if (q == "shared") q_shared = true;
      else if (q == "buffer") q_buffer = true;
    }

    if (at(";")) { next(); continue; } // e.g. `layout(local_size_x=8) in;`

    // Check interface blocks (`uniform Block { ... } [inst];` or `in/out Block { ... }`)
    if ((q_uniform || q_in || q_out || q_buffer) && cur().type == TK_IDENT &&
        peek().text == "{") {
      std::string bname = next().text;
      match("{");
      if (q_uniform) {
        int bslot = (explicit_binding >= 0) ? explicit_binding : next_ubo_slot++;
        u32 ubo_sz = 0;
        std::string body;
        while (cur().type != TK_EOF && !at("}")) {
          while (is_decl_qualifier(cur().text)) next();
          std::string ft = next().text;
          glsl_val_kind fk = parse_glsl_type(ft);
          while (cur().type == TK_IDENT) {
            std::string fn = next().text;
            u32 arr = 1;
            std::string arr_sfx;
            if (match("[")) {
              expr_out sz = parse_expr();
              match("]");
              arr = (u32)atoi(sz.s.c_str());
              if (!arr) arr = 1;
              arr_sfx = "[" + sz.s + "]";
            }
            sym_types[fn] = fk;
            hlsl_cbuffer_alloc(ubo_sz, fk, arr);
            body += "  " + std::string(hlsl_type_str(fk)) + " " + fn + arr_sfx + ";\n";
            if (!match(",")) break;
          }
          match(";");
        }
        match("}");
        std::string inst;
        if (cur().type == TK_IDENT) {
          inst = next().text;
          // Remap inst.member -> member cleanly via prefix rename?
        }
        match(";");
        char hdr[128];
        snprintf(hdr, sizeof(hdr), "cbuffer %s : register(b%d) {\n",
                 bname.c_str(), bslot + 1);
        globals_hlsl += hdr + body + "};\n";
        res.ubos.push_back({bname, inst, (u32)bslot, (ubo_sz + 15u) & ~15u});
      } else {
        // Varying / storage block
        while (cur().type != TK_EOF && !at("}")) next();
        match("}");
        if (cur().type == TK_IDENT) next();
        match(";");
      }
      continue;
    }

    // struct definition
    if (match("struct")) {
      std::string sname = next().text;
      match("{");
      std::string sdef = "struct " + sname + " {\n";
      while (cur().type != TK_EOF && !at("}")) {
        std::string ft = next().text;
        glsl_val_kind fk = parse_glsl_type(ft);
        std::string fn = next().text;
        sdef += "  " + std::string(fk != GLSL_T_UNKNOWN ? hlsl_local_type_str(fk) : ft) +
                " " + fn + ";\n";
        match(";");
      }
      match("}");
      match(";");
      sdef += "};\n";
      struct_defs[sname] = sdef;
      globals_hlsl += sdef;
      continue;
    }

    if (cur().type != TK_IDENT) { next(); continue; }
    std::string type_id = next().text;
    glsl_val_kind vk = parse_glsl_type(type_id);
    if (cur().type != TK_IDENT) { continue; }
    std::string name = next().text;

    // Function definition or prototype
    if (at("(")) {
      next(); // '('
      std::string params;
      std::vector<std::string> local_shadow;
      while (cur().type != TK_EOF && !at(")")) {
        std::string pq;
        while (cur().type == TK_IDENT && is_decl_qualifier(cur().text)) {
          if (cur().text == "out" || cur().text == "inout") pq = cur().text + " ";
          next();
        }
        if (at("void") && peek().text == ")") { next(); break; }
        std::string pt = next().text;
        glsl_val_kind pk = parse_glsl_type(pt);
        std::string pn;
        if (cur().type == TK_IDENT) pn = next().text;
        if (!pn.empty()) sym_types[pn] = pk;
        if (!params.empty()) params += ", ";
        params += pq + std::string(pk != GLSL_T_UNKNOWN ? hlsl_local_type_str(pk) : pt) +
                  (pn.empty() ? "" : " " + pn);
        match(",");
      }
      match(")");
      if (match(";")) continue; // prototype

      std::string body = parse_block("  ");
      if (name == "main") {
        main_hlsl = body;
      } else {
        sym_types[name] = vk;
        std::string rht =
            type_id == "void" ? "void"
                              : (vk != GLSL_T_UNKNOWN ? hlsl_local_type_str(vk) : type_id);
        funcs_hlsl += rht + " " + name + "(" + params + ")\n" + body + "\n";
      }
      continue;
    }

    // Variable declarations (uniform / in / out / varying / attribute / global)
    for (int vi = 0;; ++vi) {
      std::string vname = (vi == 0) ? name : next().text;
      u32 arr = 1;
      std::string arr_sfx;
      if (match("[")) {
        if (at("]")) { match("]"); }
        else {
          expr_out sz = parse_expr();
          match("]");
          arr = (u32)atoi(sz.s.c_str());
          if (!arr) arr = 1;
          arr_sfx = "[" + sz.s + "]";
        }
      }
      std::string init_expr;
      if (match("=")) init_expr = parse_assign().s;

      sym_types[vname] = vk;

      if (q_uniform) {
        if (is_sampler_kind(vk)) {
          int slot = (explicit_binding >= 0) ? explicit_binding : next_tex_slot++;
          if (vk == GLSL_T_IMAGE_2D) {
            slot = (explicit_binding >= 0) ? explicit_binding : next_uav_slot++;
            char buf[160];
            snprintf(buf, sizeof(buf), "%s %s : register(u%d);\n",
                     hlsl_tex_decl_type(vk), vname.c_str(), slot);
            globals_hlsl += buf;
            image_names.insert(vname);
          } else {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "%s %s : register(t%d);\n"
                     "%s %s_smp : register(s%d);\n"
                     "int2 _gl_texSize_%s(int lod) { uint w=1,h=1,l=1; %s.GetDimensions(lod,w,h,l); return int2((int)w,(int)h); }\n",
                     hlsl_tex_decl_type(vk), vname.c_str(), slot,
                     (vk == GLSL_T_SAMPLER_2D_SHADOW ? "SamplerComparisonState"
                                                      : "SamplerState"),
                     vname.c_str(), slot, vname.c_str(), vname.c_str());
            globals_hlsl += buf;
            sampler_names.insert(vname);
          }
          res.uniforms.push_back({vname, vk, -1, arr, 0, 0, slot, false});
        } else {
          u32 off = hlsl_cbuffer_alloc(cb0_cursor, vk, arr);
          u32 bytes = hlsl_var_raw_bytes(vk, arr);
          cbuffer_hlsl += "  " + std::string(hlsl_type_str(vk)) + " " + vname +
                          arr_sfx + ";\n";
          res.uniforms.push_back({vname, vk, explicit_loc, arr, off, bytes, -1, false});
        }
      } else if (q_attr || (q_in && res.stage == GLSL_STAGE_VERTEX)) {
        int loc = alloc_input_loc(vname, explicit_loc);
        rename_map[vname] = "_in." + vname;
        vs_in_names.insert(vname);
        res.inputs.push_back({vname, vk, loc, arr, 0, 0, -1, false});
      } else if ((q_varying && res.stage == GLSL_STAGE_VERTEX) ||
                 (q_out && res.stage == GLSL_STAGE_VERTEX)) {
        int loc = alloc_output_loc(vname, explicit_loc);
        rename_map[vname] = "_out." + vname;
        vs_out_names.insert(vname);
        res.outputs.push_back({vname, vk, loc, arr, 0, 0, -1, false});
      } else if ((q_varying || q_in) && res.stage == GLSL_STAGE_FRAGMENT) {
        int loc = alloc_input_loc(vname, explicit_loc);
        rename_map[vname] = "_in." + vname;
        res.inputs.push_back({vname, vk, loc, arr, 0, 0, -1, false});
      } else if (q_out && res.stage == GLSL_STAGE_FRAGMENT) {
        int loc = alloc_output_loc(vname, explicit_loc);
        rename_map[vname] = "_out." + vname;
        frag_out_names.insert(vname);
        res.outputs.push_back({vname, vk, loc, arr, 0, 0, -1, false});
      } else {
        std::string pfx;
        if (q_shared) pfx = "groupshared ";
        else if (q_const) pfx = "static const ";
        else pfx = "static ";
        globals_hlsl += pfx + std::string(hlsl_local_type_str(vk)) + " " +
                        vname + arr_sfx;
        if (!init_expr.empty()) globals_hlsl += " = " + init_expr;
        globals_hlsl += ";\n";
      }
      if (!match(",")) break;
    }
    match(";");
  }
  return true;
}

static const char* k_hlsl_preamble = R"(
float2 _v2(float a) { return float2(a, a); }
float2 _v2(float a, float b) { return float2(a, b); }
float2 _v2(float2 a) { return a; }
float2 _v2(float3 a) { return a.xy; }
float2 _v2(float4 a) { return a.xy; }
float3 _v3(float a) { return float3(a, a, a); }
float3 _v3(float a, float b, float c) { return float3(a, b, c); }
float3 _v3(float2 a, float b) { return float3(a, b); }
float3 _v3(float a, float2 b) { return float3(a, b); }
float3 _v3(float3 a) { return a; }
float3 _v3(float4 a) { return a.xyz; }
float4 _v4(float a) { return float4(a, a, a, a); }
float4 _v4(float a, float b, float c, float d) { return float4(a, b, c, d); }
float4 _v4(float2 a, float b, float c) { return float4(a, b, c); }
float4 _v4(float a, float2 b, float c) { return float4(a, b, c); }
float4 _v4(float a, float b, float2 c) { return float4(a, b, c); }
float4 _v4(float2 a, float2 b) { return float4(a, b); }
float4 _v4(float3 a, float b) { return float4(a, b); }
float4 _v4(float a, float3 b) { return float4(a, b); }
float4 _v4(float4 a) { return a; }
int2 _iv2(int a) { return int2(a, a); }
int2 _iv2(int a, int b) { return int2(a, b); }
int2 _iv2(float2 a) { return (int2)a; }
int3 _iv3(int a) { return int3(a, a, a); }
int3 _iv3(int a, int b, int c) { return int3(a, b, c); }
int3 _iv3(int2 a, int b) { return int3(a, b); }
int4 _iv4(int a) { return int4(a, a, a, a); }
int4 _iv4(int a, int b, int c, int d) { return int4(a, b, c, d); }
int4 _iv4(int3 a, int b) { return int4(a, b); }
uint2 _uv2(uint a) { return uint2(a, a); }
uint2 _uv2(uint a, uint b) { return uint2(a, b); }
uint3 _uv3(uint a) { return uint3(a, a, a); }
uint3 _uv3(uint a, uint b, uint c) { return uint3(a, b, c); }
uint4 _uv4(uint a) { return uint4(a, a, a, a); }
uint4 _uv4(uint a, uint b, uint c, uint d) { return uint4(a, b, c, d); }
bool2 _bv2(bool a) { return bool2(a, a); }
bool2 _bv2(bool a, bool b) { return bool2(a, b); }
bool3 _bv3(bool a) { return bool3(a, a, a); }
bool3 _bv3(bool a, bool b, bool c) { return bool3(a, b, c); }
bool4 _bv4(bool a) { return bool4(a, a, a, a); }
bool4 _bv4(bool a, bool b, bool c, bool d) { return bool4(a, b, c, d); }
float2x2 _m2(float s) { return float2x2(s,0, 0,s); }
float2x2 _m2(float a,float b,float c,float d) { return float2x2(a,c, b,d); }
float3x3 _m3(float s) { return float3x3(s,0,0, 0,s,0, 0,0,s); }
float3x3 _m3(float4x4 m) { return (float3x3)m; }
float3x3 _m3(float3 c0, float3 c1, float3 c2) { return float3x3(c0.x,c1.x,c2.x, c0.y,c1.y,c2.y, c0.z,c1.z,c2.z); }
float4x4 _m4(float s) { return float4x4(s,0,0,0, 0,s,0,0, 0,0,s,0, 0,0,0,s); }
float4x4 _m4(float3x3 m) { return float4x4(m[0].x,m[0].y,m[0].z,0, m[1].x,m[1].y,m[1].z,0, m[2].x,m[2].y,m[2].z,0, 0,0,0,1); }
float4x4 _m4(float4 c0,float4 c1,float4 c2,float4 c3) { return float4x4(c0.x,c1.x,c2.x,c3.x, c0.y,c1.y,c2.y,c3.y, c0.z,c1.z,c2.z,c3.z, c0.w,c1.w,c2.w,c3.w); }
float _gl_mod(float x, float y) { return x - y * floor(x / y); }
float2 _gl_mod(float2 x, float2 y) { return x - y * floor(x / y); }
float3 _gl_mod(float3 x, float3 y) { return x - y * floor(x / y); }
float4 _gl_mod(float4 x, float4 y) { return x - y * floor(x / y); }
)";

}  // namespace

glsl_compile_result glsl_to_hlsl(const char* source, u32 source_len,
                                 glsl_stage stage,
                                 const glsl_link_bindings* link) {
  glsl_compile_result empty{};
  empty.stage = stage;
  if (!source) { empty.log = "null source"; return empty; }
  const size_t slen = source_len ? (size_t)source_len : strlen(source);
  if (slen == 0) { empty.log = "empty source"; return empty; }

  pp_state pp;
  pp.macros["__VERSION__"] = {false, {}, "120"};
  std::string prep = preprocess_glsl(source, slen, pp);

  glsl_compiler cmp;
  cmp.res.stage = stage;
  cmp.res.version = pp.version;
  cmp.res.is_es = pp.is_es;
  cmp.link = link;
  cmp.toks = tokenize_glsl(prep);
  cmp.parse_top_level();

  // Add legacy compat built-in inputs / outputs / matrices if referenced
  if (cmp.compat_mvp) {
    u32 off = hlsl_cbuffer_alloc(cmp.cb0_cursor, GLSL_T_MAT4, 1);
    cmp.cbuffer_hlsl += "  column_major float4x4 gl_ModelViewProjectionMatrix;\n";
    cmp.res.uniforms.push_back({"gl_ModelViewProjectionMatrix", GLSL_T_MAT4, -1, 1, off, 64, -1, true});
  }
  if (cmp.compat_mv) {
    u32 off = hlsl_cbuffer_alloc(cmp.cb0_cursor, GLSL_T_MAT4, 1);
    cmp.cbuffer_hlsl += "  column_major float4x4 gl_ModelViewMatrix;\n";
    cmp.res.uniforms.push_back({"gl_ModelViewMatrix", GLSL_T_MAT4, -1, 1, off, 64, -1, true});
  }
  if (cmp.compat_proj) {
    u32 off = hlsl_cbuffer_alloc(cmp.cb0_cursor, GLSL_T_MAT4, 1);
    cmp.cbuffer_hlsl += "  column_major float4x4 gl_ProjectionMatrix;\n";
    cmp.res.uniforms.push_back({"gl_ProjectionMatrix", GLSL_T_MAT4, -1, 1, off, 64, -1, true});
  }
  if (cmp.compat_normal) {
    u32 off = hlsl_cbuffer_alloc(cmp.cb0_cursor, GLSL_T_MAT3, 1);
    cmp.cbuffer_hlsl += "  column_major float3x3 gl_NormalMatrix;\n";
    cmp.res.uniforms.push_back({"gl_NormalMatrix", GLSL_T_MAT3, -1, 1, off, 48, -1, true});
  }
  if (stage == GLSL_STAGE_VERTEX) {
    if (cmp.compat_vtx) cmp.res.inputs.push_back({"gl_Vertex", GLSL_T_VEC4, 0, 1, 0, 0, -1, true});
    if (cmp.compat_nrm) cmp.res.inputs.push_back({"gl_Normal", GLSL_T_VEC3, 2, 1, 0, 0, -1, true});
    if (cmp.compat_col) cmp.res.inputs.push_back({"gl_Color", GLSL_T_VEC4, 3, 1, 0, 0, -1, true});
    for (int t = 0; t < 8; ++t) {
      if (cmp.compat_tc[t]) {
        cmp.res.inputs.push_back({"gl_MultiTexCoord" + std::to_string(t),
                                  GLSL_T_VEC4, 8 + t, 1, 0, 0, -1, true});
      }
    }
    if (cmp.compat_out_col) {
      int loc = cmp.alloc_output_loc("gl_FrontColor", -1);
      cmp.res.outputs.push_back({"gl_FrontColor", GLSL_T_VEC4, loc, 1, 0, 0, -1, true});
    }
    for (int t = 0; t < 8; ++t) {
      if (cmp.compat_out_tc[t]) {
        std::string nm = "gl_TexCoord" + std::to_string(t);
        int loc = cmp.alloc_output_loc(nm, -1);
        cmp.res.outputs.push_back({nm, GLSL_T_VEC4, loc, 1, 0, 0, -1, true});
      }
    }
  } else if (stage == GLSL_STAGE_FRAGMENT) {
    if (cmp.compat_col) {
      int loc = cmp.alloc_input_loc("gl_FrontColor", -1);
      cmp.res.inputs.push_back({"gl_Color", GLSL_T_VEC4, loc, 1, 0, 0, -1, true});
    }
    for (int t = 0; t < 8; ++t) {
      if (cmp.compat_out_tc[t]) {
        std::string nm = "gl_TexCoord" + std::to_string(t);
        int loc = cmp.alloc_input_loc(nm, -1);
        cmp.res.inputs.push_back({nm, GLSL_T_VEC4, loc, 1, 0, 0, -1, true});
      }
    }
  }

  cmp.res.cb0_bytes = (cmp.cb0_cursor + 15u) & ~15u;

  std::string hlsl = k_hlsl_preamble;
  if (cmp.res.cb0_bytes > 0 && !cmp.cbuffer_hlsl.empty()) {
    hlsl += "\ncbuffer GLUniforms : register(b0) {\n" + cmp.cbuffer_hlsl + "};\n";
  }
  hlsl += cmp.globals_hlsl;
  hlsl += cmp.funcs_hlsl;

  if (stage == GLSL_STAGE_VERTEX) {
    hlsl += "\nstruct VSIn {\n";
    if (cmp.res.inputs.empty()) {
      hlsl += "  uint _vid : SV_VertexID;\n";
    } else {
      for (const auto& iv : cmp.res.inputs) {
        hlsl += "  " + std::string(hlsl_local_type_str(iv.kind)) + " " +
                iv.name + " : TEXCOORD" + std::to_string(iv.location) + ";\n";
      }
    }
    hlsl += "};\nstruct VSOut {\n  float4 gl_Position : SV_Position;\n";
    for (const auto& ov : cmp.res.outputs) {
      hlsl += "  " + std::string(hlsl_local_type_str(ov.kind)) + " " +
              ov.name + " : TEXCOORD" + std::to_string(ov.location) + ";\n";
    }
    hlsl += "};\nVSOut main(VSIn _in) {\n  VSOut _out = (VSOut)0;\n";
    hlsl += cmp.main_hlsl;
    hlsl += "  return _out;\n}\n";
  } else if (stage == GLSL_STAGE_FRAGMENT) {
    hlsl += "\nstruct PSIn {\n  float4 gl_FragCoord : SV_Position;\n";
    if (cmp.res.uses_front_facing) hlsl += "  bool gl_FrontFacing : SV_IsFrontFace;\n";
    if (cmp.point_coord_used) hlsl += "  float2 gl_PointCoord : PSIZE;\n";
    for (const auto& iv : cmp.res.inputs) {
      hlsl += "  " + std::string(hlsl_local_type_str(iv.kind)) + " " +
              iv.name + " : TEXCOORD" + std::to_string(iv.location) + ";\n";
    }
    hlsl += "};\nstruct PSOut {\n";
    if (cmp.frag_color_used || (cmp.res.outputs.empty() && !cmp.frag_data_used)) {
      hlsl += "  float4 gl_FragColor : SV_Target0;\n";
    }
    if (cmp.frag_data_used) {
      for (int i = 0; i < 4; ++i)
        hlsl += "  float4 gl_FragData" + std::to_string(i) +
                " : SV_Target" + std::to_string(i) + ";\n";
    }
    for (const auto& ov : cmp.res.outputs) {
      hlsl += "  " + std::string(hlsl_local_type_str(ov.kind)) + " " +
              ov.name + " : SV_Target" + std::to_string(ov.location) + ";\n";
    }
    if (cmp.res.writes_frag_depth) hlsl += "  float gl_FragDepth : SV_Depth;\n";
    hlsl += "};\nPSOut main(PSIn _in) {\n  PSOut _out = (PSOut)0;\n";
    hlsl += cmp.main_hlsl;
    hlsl += "  return _out;\n}\n";
  } else {
    char nh[128];
    snprintf(nh, sizeof(nh), "\n[numthreads(%u, %u, %u)]\n",
             cmp.res.local_size[0] ? cmp.res.local_size[0] : 1u,
             cmp.res.local_size[1] ? cmp.res.local_size[1] : 1u,
             cmp.res.local_size[2] ? cmp.res.local_size[2] : 1u);
    hlsl += nh;
    hlsl +=
        "void main(uint3 _cs_gid : SV_DispatchThreadID, uint3 _cs_tid : SV_GroupThreadID,\n"
        "          uint3 _cs_wgid : SV_GroupID, uint _cs_gidx : SV_GroupIndex)\n"
        "{\n" + cmp.main_hlsl + "}\n";
  }

  cmp.res.hlsl = std::move(hlsl);
  cmp.res.ok = !cmp.main_hlsl.empty() || !cmp.funcs_hlsl.empty();
  return cmp.res;
}
