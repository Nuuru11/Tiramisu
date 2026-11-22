// tiramisu/src/trt/x86_32.cpp
// Clean-room x86-32 interpreter. MIT: written from the public instruction
// encoding, no vendor code. Scope: integer guest code + flat memory window;
// anything outside the window is a NAMED trap, never a silent success.
#include "x86_32.h"

#include <string.h>

namespace {

typedef signed char i8;

const u32 CF = 1u << 0;
const u32 ZF = 1u << 6;
const u32 SF = 1u << 7;
const u32 OF = 1u << 11;

struct decoder {
  trt_x86_ctx* c;
  bool fault = false;
  u32 seg_base = 0;  // active FS/GS prefix base for this instruction
  bool ops16 = false;  // 0x66 operand-size prefix
  u16 i16() { u16 v = 0; for (int i = 0; i < 2; ++i) v |= (u16)byte() << (8 * i); return v; }

  u8 byte() {
    if (trt_x86_mem_read(c, c->eip, 1, &v8)) { fault = true; return 0; }
    ++c->eip;
    return v8;
  }
  u32 d32() {
    u32 v = 0;
    for (int i = 0; i < 4; ++i) v |= (u32)byte() << (8 * i);
    return v;
  }
  u32 i32() { return d32(); }
  u8 v8;
};

struct modrm_t {
  int mod = 0, reg = 0, rm = 0;
  u32 va = 0;
};

int decode_modrm(decoder& d, modrm_t& m) {
  u8 b = d.byte();
  m.mod = b >> 6;
  m.reg = (b >> 3) & 7;
  m.rm = b & 7;
  u32 disp = 0;
  if (m.mod == 3) return 0;  // register direct
  int base = -1, index = -1, scale = 1;
  if (m.rm == 4) {
    u8 sib = d.byte();
    scale = 1 << (sib >> 6);
    index = ((sib >> 3) & 7);
    base = sib & 7;
    if (base == 5 && m.mod == 0) {
      disp = d.d32();
      base = -1;
    }
  } else {
    base = m.rm;
    // mod=00 rm=101 is disp32 with NO base register (not [ebp]).
    if (m.mod == 0 && m.rm == 5) {
      disp = d.d32();
      base = -1;
    }
  }
  if (m.mod == 1) disp = (u32)(i8)d.byte();
  else if (m.mod == 2) disp = d.d32();
  m.va = disp + d.seg_base;
  if (base >= 0) m.va += d.c->r[base];
  if (index >= 0 && index != 4) m.va += d.c->r[index] * (u32)scale;
  return 0;
}

void set_reg_op(decoder& d, int i, u32 v) {
  if (d.ops16) d.c->r[i] = (d.c->r[i] & 0xffff0000u) | (v & 0xffffu);
  else d.c->r[i] = v;
}

u32 read_rm(decoder& d, const modrm_t& m) {
  if (d.ops16) {
    if (m.mod == 3) return d.c->r[m.rm] & 0xffffu;
    u16 v = 0;
    if (trt_x86_mem_read(d.c, m.va, 2, &v)) d.fault = true;
    return v;
  }
  if (m.mod == 3) return d.c->r[m.rm];
  u32 v = 0;
  if (trt_x86_mem_read(d.c, m.va, 4, &v)) d.fault = true;
  return v;
}

void write_rm(decoder& d, const modrm_t& m, u32 v) {
  if (m.mod == 3) {
    if (d.ops16) d.c->r[m.rm] = (d.c->r[m.rm] & 0xffff0000u) | (v & 0xffffu);
    else d.c->r[m.rm] = v;
    return;
  }
  if (d.ops16) { u16 w = (u16)v; trt_x86_mem_write(d.c, m.va, 2, &w); }
  else trt_x86_mem_write(d.c, m.va, 4, &v);
}

void set_logic_flags(trt_x86_ctx* c, u32 v) {
  c->eflags &= ~(ZF | SF | OF | CF);
  if (!v) c->eflags |= ZF;
  if (v & 0x80000000u) c->eflags |= SF;
}

void set_add_flags(trt_x86_ctx* c, u32 a, u32 b, u32 r) {
  set_logic_flags(c, r);
  if ((a & b) | ((~r) & (a | b))) c->eflags &= ~CF;
  else c->eflags |= CF;
  if ((~(a ^ b) & (a ^ r)) & 0x80000000u) c->eflags |= OF;
  else c->eflags &= ~OF;
}

void set_sub_flags(trt_x86_ctx* c, u32 a, u32 b, u32 r) {
  set_logic_flags(c, r);
  if (a < b) c->eflags |= CF; else c->eflags &= ~CF;
  if ((a ^ b) & (a ^ r) & 0x80000000u) c->eflags |= OF;
  else c->eflags &= ~OF;
}

u8 get_reg8(trt_x86_ctx* c, int i) {
  const u8* p = (const u8*)&c->r[i & 3];
  return (i & 4) ? p[1] : p[0];
}
void set_reg8(trt_x86_ctx* c, int i, u8 v) {
  u8* p = (u8*)&c->r[i & 3];
  if (i & 4) p[1] = v; else p[0] = v;
}
u8 read_rm8(decoder& d, const modrm_t& m) {
  if (m.mod == 3) return get_reg8(d.c, m.rm);
  u8 v = 0;
  if (trt_x86_mem_read(d.c, m.va, 1, &v)) d.fault = true;
  return v;
}
void write_rm8(decoder& d, const modrm_t& m, u8 v) {
  if (m.mod == 3) set_reg8(d.c, m.rm, v);
  else if (trt_x86_mem_write(d.c, m.va, 1, &v)) d.fault = true;
}

u32 apply_shift(int kind, u32 v, unsigned n) {
  n &= 31;
  switch (kind) {
    case 0: return (v << n) | (v >> ((32 - n) & 31));       // rol
    case 1: return (v >> n) | (v << ((32 - n) & 31));       // ror
    case 2: return v;                                        // rcl (no carry model)
    case 4: return v << n;                                   // shl
    case 5: return v >> n;                                   // shr
    case 7: return n ? ((u32)((i32)v >> n)) : v;             // sar
    default: return v;
  }
}

bool jcc(int cond, u32 flags) {
  switch (cond) {
    case 0x4: return (flags & ZF) != 0;             // je
    case 0x5: return (flags & ZF) == 0;             // jne
    case 0x2: return (flags & CF) != 0;             // jb
    case 0x3: return (flags & CF) == 0;             // jae
    case 0x7: return (flags & ZF) == 0 && (flags & SF) == (flags & OF); // jg
    case 0xc: return (flags & SF) != (flags & OF);  // jl
    case 0xd: return (flags & SF) == (flags & OF);  // jge
    case 0xe: return (flags & ZF) != 0 || (flags & SF) != (flags & OF); // jle
    default: return false;
  }
}

}  // namespace

static void (*g_x86_trace)(u32, unsigned) = nullptr;
void trt_x86_set_trace(void (*fn)(u32, unsigned)) { g_x86_trace = fn; }

static unsigned g_last_opcode = 0;
static u32 g_last_fault_va = 0;
unsigned trt_x86_last_opcode(void) { return g_last_opcode; }
u32 trt_x86_last_fault_va(void) { return g_last_fault_va; }

int trt_x86_mem_read(const trt_x86_ctx* c, u32 va, u32 len, void* out) {
  if (!c || !c->mem) return -1;
  if (va < c->mem_va || (u64)va + len > (u64)c->mem_va + c->mem_size) return -1;
  memcpy(out, c->mem + (va - c->mem_va), len);
  return 0;
}

int trt_x86_mem_write(trt_x86_ctx* c, u32 va, u32 len, const void* in) {
  if (!c || !c->mem) return -1;
  if (va < c->mem_va || (u64)va + len > (u64)c->mem_va + c->mem_size) return -1;
  memcpy(c->mem + (va - c->mem_va), in, len);
  return 0;
}

int trt_x86_run(trt_x86_ctx* c, u64 max_instructions) {
  g_last_opcode = 0; g_last_fault_va = 0;
  if (!c || !c->mem) return 2;
  decoder d{c, false};
  for (u64 n = 0; n < max_instructions; ++n) {
    // A trap is an address outside the mapped window.
    if (c->eip < c->mem_va || c->eip >= c->mem_va + c->mem_size) {
      if (!c->trap) { c->stop_code = 0x80000003u; return 1; }
      int r = c->trap(c, c->eip, c->trap_user);
      if (r) { c->stop_code = (u32)r; return 1; }
      continue;
    }
    const u32 op_va = c->eip;
    u8 op = d.byte();
    // Segment/operand/REP prefixes. FS/GS are real on 32-bit Windows (TEB /
    // TLS), so their bases are applied to the effective address.
    d.seg_base = 0;
    d.ops16 = false;
    while (op == 0x64 || op == 0x65 || op == 0x26 || op == 0x2e ||
           op == 0x36 || op == 0x3e || op == 0xf2 || op == 0xf3 ||
           op == 0x66) {
      if (op == 0x64) d.seg_base = c->fs_base;
      if (op == 0x65) d.seg_base = c->gs_base;
      if (op == 0x66) d.ops16 = true;
      op = d.byte();
      if (d.fault) { g_last_opcode = op; g_last_fault_va = op_va; c->stop_code = op_va; return 2; }
    }
    if (d.fault) { g_last_opcode = op; g_last_fault_va = op_va; c->stop_code = op_va; return 2; }
    if (g_x86_trace) g_x86_trace(op_va, op);
    switch (op) {
      case 0x0d: {                                        // or eax, imm32 (no ModRM)
        u32 imm = d.i32();
        c->r[X86_EAX] |= imm;
        set_logic_flags(c, c->r[X86_EAX]);
        break;
      }
      case 0x90: break;                                   // nop
      case 0xf4: return 0;                                // hlt
      case 0xcc: c->stop_code = 0x80000003u; return 1;    // int3
      case 0xc3: {                                        // ret
        u32 sp = c->r[X86_ESP];
        u32 dest = 0;
        if (trt_x86_mem_read(c, sp, 4, &dest)) { c->stop_code = sp; return 2; }
        c->r[X86_ESP] = sp + 4;
        c->eip = dest;
        break;
      }
      case 0xc9:                                          // leave
        c->r[X86_ESP] = c->r[X86_EBP];
        { u32 dest = 0; trt_x86_mem_read(c, c->r[X86_ESP], 4, &dest);
          c->r[X86_ESP] += 4; c->r[X86_EBP] = dest; }
        break;
      case 0xe8: {                                        // call rel32
        u32 rel = d.i32();
        u32 sp = c->r[X86_ESP] - 4;
        if (trt_x86_mem_write(c, sp, 4, &c->eip)) { c->stop_code = sp; return 2; }
        c->r[X86_ESP] = sp;
        c->eip += rel;
        break;
      }
      case 0xe9: c->eip += d.i32(); break;                // jmp rel32
      case 0xeb: { u8 r = d.byte(); c->eip += (u32)(i8)r; break; }
      case 0xe0: case 0xe1: case 0xe2: {                   // loop/loopne/loope
        u8 rel = d.byte();
        --c->r[X86_ECX];
        bool take = c->r[X86_ECX] != 0;
        if (op == 0xe0) take = take && !(c->eflags & ZF);
        if (op == 0xe1) take = take && (c->eflags & ZF);
        if (take) c->eip += (u32)(i8)rel;
        break; }
      case 0xe3: { u8 r = d.byte();                        // jecxz
        if (!c->r[X86_ECX]) c->eip += (u32)(i8)r; break; }
      case 0x68: { u32 v = d.i32();                       // push imm32
        u32 sp = c->r[X86_ESP] - 4;
        if (trt_x86_mem_write(c, sp, 4, &v)) { c->stop_code = sp; return 2; }
        c->r[X86_ESP] = sp; break; }
      case 0x6a: { u8 v = d.byte();                       // push imm8
        u32 w = (u32)(i8)v, sp = c->r[X86_ESP] - 4;
        if (trt_x86_mem_write(c, sp, 4, &w)) { c->stop_code = sp; return 2; }
        c->r[X86_ESP] = sp; break; }
      case 0x00: case 0x02: case 0x08: case 0x0a:
      case 0x10: case 0x12: case 0x18: case 0x1a:
      case 0x20: case 0x22: case 0x28: case 0x2a: case 0x30:
      case 0x32: case 0x38: case 0x3a: case 0x84: case 0x85:
      case 0x88: case 0x8a: case 0xc6: {                   // 8-bit ALU/mov
        modrm_t m; decode_modrm(d, m);
        if (d.fault) { g_last_opcode = op; g_last_fault_va = op_va; c->stop_code = op_va; return 2; }
        u8 a = read_rm8(d, m), b = get_reg8(c, m.reg), r = 0;
        if (d.fault) { g_last_opcode = op; g_last_fault_va = op_va; c->stop_code = op_va; return 2; }
        switch (op) {
          case 0x00: r = a + b; write_rm8(d, m, r); break;
          case 0x02: r = b + a; set_reg8(c, m.reg, r); break;
          case 0x08: r = a | b; write_rm8(d, m, r); break;
          case 0x0a: r = b | a; set_reg8(c, m.reg, r); break;
          case 0x10: r = a + b; write_rm8(d, m, r); break;
          case 0x12: r = b + a; set_reg8(c, m.reg, r); break;
          case 0x18: r = a - b; write_rm8(d, m, r); break;
          case 0x1a: r = b - a; set_reg8(c, m.reg, r); break;
          case 0x20: r = a & b; write_rm8(d, m, r); break;
          case 0x22: r = b & a; set_reg8(c, m.reg, r); break;
          case 0x28: r = a - b; write_rm8(d, m, r); break;
          case 0x2a: r = b - a; set_reg8(c, m.reg, r); break;
          case 0x30: r = a ^ b; write_rm8(d, m, r); break;
          case 0x32: r = b ^ a; set_reg8(c, m.reg, r); break;
          case 0x38: r = a - b; break;
          case 0x3a: r = b - a; break;
          case 0x84: r = a & b; break;
          case 0x85: r = a & b; break;
          case 0x88: write_rm8(d, m, b); break;
          case 0x8a: set_reg8(c, m.reg, a); break;
          case 0xc6: r = d.byte(); write_rm8(d, m, r); break;
        }
        if (op != 0x88 && op != 0x8a && op != 0xc6) set_logic_flags(c, r);
        if (d.fault) { g_last_opcode = op; g_last_fault_va = op_va; c->stop_code = op_va; return 2; }
        break;
      }
      case 0xb0: case 0xb1: case 0xb2: case 0xb3:          // mov r8, imm8
      case 0xb4: case 0xb5: case 0xb6: case 0xb7:
        set_reg8(c, op & 7, d.byte());
        break;
      case 0xa0: case 0xa1: case 0xa2: case 0xa3: {        // moffs moves
        const u32 addr = d.d32();
        if (op == 0xa0) { u8 v = 0; trt_x86_mem_read(c, addr, 1, &v); c->r[X86_EAX] = (c->r[X86_EAX] & 0xffffff00u) | v; }
        else if (op == 0xa1) { u32 v = 0; trt_x86_mem_read(c, addr, 4, &v); c->r[X86_EAX] = v; }
        else if (op == 0xa2) { u8 v = (u8)c->r[X86_EAX]; trt_x86_mem_write(c, addr, 1, &v); }
        else { u32 v = c->r[X86_EAX]; trt_x86_mem_write(c, addr, 4, &v); }
        break;
      }
      case 0xf7: {                                        // group-3 r/m32,imm
        modrm_t m; decode_modrm(d, m);
        if (d.fault) { g_last_opcode = op; g_last_fault_va = op_va; c->stop_code = op_va; return 2; }
        u32 a = read_rm(d, m);
        if (d.fault) { g_last_opcode = op; g_last_fault_va = op_va; c->stop_code = op_va; return 2; }
        if (m.reg == 0) { /* test imm32 */ set_logic_flags(c, a & d.i32()); }
        else if (m.reg == 2) { write_rm(d, m, ~a); }
        else if (m.reg == 3) { u32 r = (u32)(-(i32)a); write_rm(d, m, r); set_sub_flags(c, 0, a, r); }
        else if (m.reg == 4) { u64 r = (u64)a * (u64)c->r[X86_EAX];
          c->r[X86_EAX] = (u32)r; c->r[X86_EDX] = (u32)(r >> 32); }
        else if (m.reg == 5) { u64 r = (u64)a * (u64)(i32)c->r[X86_EAX];
          c->r[X86_EAX] = (u32)r; c->r[X86_EDX] = (u32)(r >> 32); }
        else { g_last_opcode = op; g_last_fault_va = op_va; c->stop_code = op_va; return 2; }
        break;
      }
      case 0xc1: case 0xd1: case 0xd3: {                   // shift/rotate group
        modrm_t m; decode_modrm(d, m);
        if (d.fault) { g_last_opcode = op; g_last_fault_va = op_va; c->stop_code = op_va; return 2; }
        u32 a = read_rm(d, m);
        unsigned n = (op == 0xc1) ? d.byte() : (op == 0xd1) ? 1
                                                          : (c->r[X86_ECX] & 31);
        u32 res = apply_shift(m.reg, a, n);
        write_rm(d, m, res);
        if (d.fault) { g_last_opcode = op; g_last_fault_va = op_va; c->stop_code = op_va; return 2; }
        if (res) c->eflags &= ~ZF; else c->eflags |= ZF;
        break;
      }
      case 0x0f: {                                        // two-byte opcodes
        u8 sub = d.byte();
        if (sub == 0x31) {                                 // rdtsc
          static u64 ticks = 0;
          c->r[X86_EAX] = (u32)ticks;
          c->r[X86_EDX] = (u32)(ticks++ >> 32);
        } else if (sub == 0xb6 || sub == 0xb7 || sub == 0xbe || sub == 0xbf) {
          modrm_t m; decode_modrm(d, m);
          if (d.fault) { g_last_opcode = op; g_last_fault_va = op_va; c->stop_code = op_va; return 2; }
          u32 v = 0;
          const int len = (sub == 0xb6 || sub == 0xbe) ? 1 : 2;
          if (m.mod == 3) v = c->r[m.rm];
          else {
            u8 tmp[4] = {};
            if (trt_x86_mem_read(c, m.va, (u32)len, tmp)) d.fault = true;
            v = (len == 1) ? tmp[0] : (u32)(u16)(tmp[0] | (tmp[1] << 8));
          }
          if ((sub == 0xbe && (v & 0x80)) || (sub == 0xbf && (v & 0x8000)))
            v |= 0xffff0000u;
          c->r[m.reg] = v;
        } else if ((sub & 0xf0) == 0x80) {                 // jcc rel32
          u32 rel = d.i32();
          if (jcc(sub & 0xf, c->eflags)) c->eip += rel;
        } else { g_last_opcode = op; g_last_fault_va = op_va; c->stop_code = op_va; return 2; }
        break; }
      default:
        if ((op & 0xf0) == 0x70) {                        // jcc rel8
          u8 r = d.byte();
          if (jcc(op & 0xf, c->eflags)) c->eip += (u32)(i8)r;
        } else if ((op & 0xf8) == 0xb8) {                 // mov r32/r16, imm
          if (d.ops16) set_reg_op(d, op & 7, d.i16());
          else c->r[op & 7] = d.i32();
        } else if ((op & 0xf0) == 0x50) {                 // push r32
          u32 sp = c->r[X86_ESP] - 4;
          if (trt_x86_mem_write(c, sp, 4, &c->r[op & 7])) { c->stop_code = sp; return 2; }
          c->r[X86_ESP] = sp;
        } else if ((op & 0xf8) == 0x58) {                 // pop r32
          u32 sp = c->r[X86_ESP], v = 0;
          if (trt_x86_mem_read(c, sp, 4, &v)) { c->stop_code = sp; return 2; }
          c->r[X86_ESP] = sp + 4; c->r[op & 7] = v;
        } else if (op == 0x89 || op == 0x8b || op == 0x01 ||
                   op == 0x03 || op == 0x29 || op == 0x2b ||
                   op == 0x31 || op == 0x33 || op == 0x39 ||
                   op == 0x3b || op == 0x09 || op == 0x0b ||
                   op == 0x85 || op == 0xc7 ||
                   op == 0x8d || op == 0x83 || op == 0xff) {
          modrm_t m; decode_modrm(d, m);
          if (d.fault) { g_last_opcode = op; g_last_fault_va = op_va; c->stop_code = op_va; return 2; }
          u32 a = read_rm(d, m);
          u32 b = d.ops16 ? (c->r[m.reg] & 0xffffu) : c->r[m.reg];
          u32 res = 0;
          if (d.fault) { g_last_opcode = op; g_last_fault_va = op_va; c->stop_code = op_va; return 2; }
          switch (op) {
            case 0x89: write_rm(d, m, b); break;          // mov r/m, r
            case 0x8b: set_reg_op(d, m.reg, a); break;            // mov r, r/m
            case 0x09: res = a | b; write_rm(d, m, res); set_logic_flags(c, res); break;
            case 0x0b: res = b | a; set_reg_op(d, m.reg, res); set_logic_flags(c, res); break;

            case 0x01: res = a + b; write_rm(d, m, res); set_add_flags(c, a, b, res); break;
            case 0x03: res = b + a; set_reg_op(d, m.reg, res); set_add_flags(c, b, a, res); break;
            case 0x29: res = a - b; write_rm(d, m, res); set_sub_flags(c, a, b, res); break;
            case 0x2b: res = b - a; set_reg_op(d, m.reg, res); set_sub_flags(c, b, a, res); break;
            case 0x31: res = a ^ b; write_rm(d, m, res); set_logic_flags(c, res); break;
            case 0x33: res = b ^ a; set_reg_op(d, m.reg, res); set_logic_flags(c, res); break;
            case 0x39: set_sub_flags(c, a, b, a - b); break;   // cmp r/m, r
            case 0x3b: set_sub_flags(c, b, a, b - a); break;   // cmp r, r/m
            case 0x85: set_logic_flags(c, a & b); break;       // test
            case 0x8d: set_reg_op(d, m.reg, m.va); break;              // lea
            case 0xc7: res = d.ops16 ? d.i16() : d.i32(); write_rm(d, m, res); break;
            case 0xff: {                            // group-5 call/jmp r/m32
              if (m.reg == 2) {                     // call near r/m32
                u32 sp = c->r[X86_ESP] - 4;
                if (trt_x86_mem_write(c, sp, 4, &c->eip)) { c->stop_code = sp; return 2; }
                c->r[X86_ESP] = sp;
                c->eip = a;
              } else if (m.reg == 4) {              // jmp near r/m32
                c->eip = a;
              } else { g_last_opcode = op; g_last_fault_va = op_va; c->stop_code = op_va; return 2; }
              break; }
            case 0x83: { u8 imm = d.byte();                    // group-1 imm8
              if (m.reg == 0) { res = a + (u32)(i8)imm; write_rm(d, m, res); set_add_flags(c, a, (u32)(i8)imm, res); }
              else if (m.reg == 5) { res = a - (u32)(i8)imm; write_rm(d, m, res); set_sub_flags(c, a, (u32)(i8)imm, res); }
              else if (m.reg == 7) set_sub_flags(c, a, (u32)(i8)imm, a - (u32)(i8)imm);
              else if (m.reg == 1) { res = a | (u32)(i8)imm; write_rm(d, m, res); set_logic_flags(c, res); }
              else if (m.reg == 4) { res = a & (u32)(i8)imm; write_rm(d, m, res); set_logic_flags(c, res); }
              else if (m.reg == 6) { res = a ^ (u32)(i8)imm; write_rm(d, m, res); set_logic_flags(c, res); }
              else { g_last_opcode = op; g_last_fault_va = op_va; c->stop_code = op_va; return 2; }
              break; }
          }
        } else {
          g_last_opcode = op; g_last_fault_va = op_va;
          c->stop_code = op_va;
          return 2;  // fail-visible: name the first unsupported opcode
        }
    }
    if (d.fault) { g_last_opcode = op; g_last_fault_va = op_va; c->stop_code = op_va; return 2; }
  }
  c->stop_code = 0x80000004u;  // instruction budget = named hang
  return 2;
}
