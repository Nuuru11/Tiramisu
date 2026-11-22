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
  }
  if (m.mod == 1) disp = (u32)(i8)d.byte();
  else if (m.mod == 2) disp = d.d32();
  m.va = disp;
  if (base >= 0) m.va += d.c->r[base];
  if (index >= 0 && index != 4) m.va += d.c->r[index] * (u32)scale;
  return 0;
}

u32 read_rm(decoder& d, const modrm_t& m) {
  if (m.mod == 3) return d.c->r[m.rm];
  u32 v = 0;
  if (trt_x86_mem_read(d.c, m.va, 4, &v)) d.fault = true;
  return v;
}

void write_rm(decoder& d, const modrm_t& m, u32 v) {
  if (m.mod == 3) d.c->r[m.rm] = v;
  else if (trt_x86_mem_write(d.c, m.va, 4, &v)) d.fault = true;
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
    if (d.fault) { c->stop_code = op_va; return 2; }
    switch (op) {
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
      case 0x0f: {                                        // jcc rel32
        u8 sub = d.byte();
        if ((sub & 0xf0) == 0x80) {
          u32 rel = d.i32();
          if (jcc(sub & 0xf, c->eflags)) c->eip += rel;
        } else { c->stop_code = op_va; return 2; }
        break; }
      default:
        if ((op & 0xf0) == 0x70) {                        // jcc rel8
          u8 r = d.byte();
          if (jcc(op & 0xf, c->eflags)) c->eip += (u32)(i8)r;
        } else if ((op & 0xf8) == 0xb8) {                 // mov r32, imm32
          c->r[op & 7] = d.i32();
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
                   op == 0x3b || op == 0x85 || op == 0xc7 ||
                   op == 0x8d || op == 0x83) {
          modrm_t m; decode_modrm(d, m);
          if (d.fault) { c->stop_code = op_va; return 2; }
          u32 a = read_rm(d, m), b = c->r[m.reg], res = 0;
          if (d.fault) { c->stop_code = op_va; return 2; }
          switch (op) {
            case 0x89: write_rm(d, m, b); break;          // mov r/m, r
            case 0x8b: c->r[m.reg] = a; break;            // mov r, r/m
            case 0x01: res = a + b; write_rm(d, m, res); set_add_flags(c, a, b, res); break;
            case 0x03: res = b + a; c->r[m.reg] = res; set_add_flags(c, b, a, res); break;
            case 0x29: res = a - b; write_rm(d, m, res); set_sub_flags(c, a, b, res); break;
            case 0x2b: res = b - a; c->r[m.reg] = res; set_sub_flags(c, b, a, res); break;
            case 0x31: res = a ^ b; write_rm(d, m, res); set_logic_flags(c, res); break;
            case 0x33: res = b ^ a; c->r[m.reg] = res; set_logic_flags(c, res); break;
            case 0x39: set_sub_flags(c, a, b, a - b); break;   // cmp r/m, r
            case 0x3b: set_sub_flags(c, b, a, b - a); break;   // cmp r, r/m
            case 0x85: set_logic_flags(c, a & b); break;       // test
            case 0x8d: c->r[m.reg] = m.va; break;              // lea
            case 0xc7: res = d.i32(); write_rm(d, m, res); break;
            case 0x83: { u8 imm = d.byte();                    // group-1 imm8
              if (m.reg == 0) { res = a + (u32)(i8)imm; write_rm(d, m, res); set_add_flags(c, a, (u32)(i8)imm, res); }
              else if (m.reg == 5) { res = a - (u32)(i8)imm; write_rm(d, m, res); set_sub_flags(c, a, (u32)(i8)imm, res); }
              else if (m.reg == 7) set_sub_flags(c, a, (u32)(i8)imm, a - (u32)(i8)imm);
              else { c->stop_code = op_va; return 2; }
              break; }
          }
        } else {
          c->stop_code = op_va;
          return 2;  // fail-visible: name the first unsupported opcode
        }
    }
    if (d.fault) { c->stop_code = op_va; return 2; }
  }
  c->stop_code = 0x80000004u;  // instruction budget = named hang
  return 2;
}
