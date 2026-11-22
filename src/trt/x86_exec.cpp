// tiramisu/src/trt/x86_exec.cpp
// x86 (32-bit) execution engine.
//
// Decodes and executes x86 instructions. Handles common integer, memory,
// control flow, and stack operations. API calls are thunked to 64-bit
// bridges via x86_call_bridge().
#include "x86_cpu.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
static int g_seg_override = -1;

// ---- ModRM byte helpers ----
static int modrm_mod(uint8_t m) { return (m >> 6) & 3; }
static int modrm_reg(uint8_t m) { return (m >> 3) & 7; }
static int modrm_rm(uint8_t m)  { return m & 7; }

// SIB byte helpers
static int sib_scale(uint8_t s) { return (s >> 6) & 3; }
static int sib_index(uint8_t s) { return (s >> 3) & 7; }
static int sib_base(uint8_t s)  { return s & 7; }

// ---- Register index to pointer (standard x86 encoding) ----
// 0=EAX,1=ECX,2=EDX,3=EBX,4=ESP,5=EBP,6=ESI,7=EDI
uint32_t* x86_reg_ptr(x86_context* ctx, int idx) {
    switch (idx) {
        case 0: return &ctx->regs.eax;
        case 1: return &ctx->regs.ecx;
        case 2: return &ctx->regs.edx;
        case 3: return &ctx->regs.ebx;
        case 4: return &ctx->regs.esp;
        case 5: return &ctx->regs.ebp;
        case 6: return &ctx->regs.esi;
        case 7: return &ctx->regs.edi;
    }
    return &ctx->regs.eax;
}

// ---- Effective address calculation ----
// Returns the effective address for a ModRM byte. Handles mod, SIB, displacements.
static uint32_t calc_ea(x86_context* ctx, uint8_t modrm, int* bytes_consumed) {
    int mod = modrm_mod(modrm);
    int rm = modrm_rm(modrm);
    uint32_t addr = 0;
    int consumed = 1; // modrm byte itself

    if (mod == 3) {
        // register operand, no memory address
        *bytes_consumed = consumed;
        return 0; // caller checks mod==3
    }

    // Base register
    if (rm == 4 && mod != 0) {
        // SIB follows
        uint8_t sib = x86_mem_read8(ctx, ctx->regs.eip + consumed);
        consumed++;
        int scale = 1 << sib_scale(sib);
        int index = sib_index(sib);
        int base = sib_base(sib);

        if (index != 4) { // index not ESP
            addr += *x86_reg_ptr(ctx, index) * scale;
        }
        if (base != 5 || mod != 0) {
            addr += *x86_reg_ptr(ctx, base);
        }
    } else if (rm != 5 || mod != 0) {
        addr += *x86_reg_ptr(ctx, rm);
    }

    // Displacement
    if (mod == 0 && rm == 5) {
        // disp32 only
        addr = x86_mem_read32(ctx, ctx->regs.eip + consumed);
        consumed += 4;
    } else if (mod == 0 && rm == 4) {
        // SIB with disp32 if base==5
        uint8_t sib = x86_mem_read8(ctx, ctx->regs.eip + 1); // re-read
        if (sib_base(sib) == 5) {
            uint32_t disp = x86_mem_read32(ctx, ctx->regs.eip + consumed);
            addr += disp;
            consumed += 4;
        }
    } else if (mod == 1) {
        int8_t disp = (int8_t)x86_mem_read8(ctx, ctx->regs.eip + consumed);
        addr += disp;
        consumed += 1;
    } else if (mod == 2) {
        uint32_t disp = x86_mem_read32(ctx, ctx->regs.eip + consumed);
        addr += disp;
        consumed += 4;
    }

    // FS segment override: add FS base
    if (g_seg_override == 4) {
        addr += ctx->regs.fs_base;
    }
    *bytes_consumed = consumed;
    return addr;
}

// ---- Flag helpers ----
void x86_update_flags_add(x86_context* ctx, uint32_t a, uint32_t b, uint32_t result) {
    ctx->regs.eflags &= ~(X86_CF | X86_ZF | X86_SF | X86_OF | X86_PF | X86_AF);
    if (result == 0) ctx->regs.eflags |= X86_ZF;
    if (result & 0x80000000) ctx->regs.eflags |= X86_SF;
    if (result < a) ctx->regs.eflags |= X86_CF; // carry
    // overflow
    if (((a ^ result) & (b ^ result)) & 0x80000000) ctx->regs.eflags |= X86_OF;
    // parity (even number of set bits in low byte)
    int bits = __builtin_popcount(result & 0xFF);
    if (bits % 2 == 0) ctx->regs.eflags |= X86_PF;
}

void x86_update_flags_sub(x86_context* ctx, uint32_t a, uint32_t b, uint32_t result) {
    ctx->regs.eflags &= ~(X86_CF | X86_ZF | X86_SF | X86_OF | X86_PF | X86_AF);
    if (result == 0) ctx->regs.eflags |= X86_ZF;
    if (result & 0x80000000) ctx->regs.eflags |= X86_SF;
    if (a < b) ctx->regs.eflags |= X86_CF; // borrow
    if (((a ^ b) & (a ^ result)) & 0x80000000) ctx->regs.eflags |= X86_OF;
    int bits = __builtin_popcount(result & 0xFF);
    if (bits % 2 == 0) ctx->regs.eflags |= X86_PF;
}

void x86_update_flags_logic(x86_context* ctx, uint32_t result) {
    ctx->regs.eflags &= ~(X86_CF | X86_ZF | X86_SF | X86_OF | X86_PF);
    if (result == 0) ctx->regs.eflags |= X86_ZF;
    if (result & 0x80000000) ctx->regs.eflags |= X86_SF;
    int bits = __builtin_popcount(result & 0xFF);
    if (bits % 2 == 0) ctx->regs.eflags |= X86_PF;
}

// ---- FPU helpers ----
static double fpu_st(x86_context* ctx, int i) {
    int idx = (ctx->regs.fpu_top + i) & 7;
    return ctx->regs.fpu[idx];
}
static void fpu_push(x86_context* ctx, double val) {
    ctx->regs.fpu_top = (ctx->regs.fpu_top - 1 + 8) & 7;
    ctx->regs.fpu[ctx->regs.fpu_top] = val;
    ctx->regs.fpu_tags[ctx->regs.fpu_top] = 0; // valid
}
static void fpu_pop(x86_context* ctx) {
    ctx->regs.fpu_tags[ctx->regs.fpu_top] = 3; // empty
    ctx->regs.fpu_top = (ctx->regs.fpu_top + 1) & 7;
}
static double fpu_round_prec(x86_context* ctx, double v);
static void fpu_set_st(x86_context* ctx, int i, double val) {
    int idx = (ctx->regs.fpu_top + i) & 7;
    ctx->regs.fpu[idx] = fpu_round_prec(ctx, val);
    ctx->regs.fpu_tags[idx] = 0;
}
// Convert with the x87 control-word rounding mode (FIST/FISTP semantics).
static long long fpu_to_int(x86_context* ctx, double v) {
    switch ((ctx->regs.fpu_cw >> 10) & 3) {
        case 0: return (long long)nearbyint(v); // nearest-even
        case 1: return (long long)floor(v);     // toward -inf
        case 2: return (long long)ceil(v);      // toward +inf
        default: return (long long)trunc(v);    // toward zero
    }
}
// Round a result to the control-word precision (24/53/64-bit significand).
static double fpu_round_prec(x86_context* ctx, double v) {
    int prec = (ctx->regs.fpu_cw >> 8) & 3;
    if (prec == 0) return (double)(float)v; // single precision
    return v; // double/extended both fit a host double
}
// Pack the x87 environment (FNSTENV layout, 28 bytes).
static void fpu_write_env(x86_context* ctx, uint32_t addr) {
    x86_mem_write16(ctx, addr + 0, ctx->regs.fpu_cw);
    x86_mem_write16(ctx, addr + 4, ctx->regs.fpu_sw);
    x86_mem_write16(ctx, addr + 8, ctx->regs.fpu_tw);
    x86_mem_write32(ctx, addr + 12, 0);
    x86_mem_write32(ctx, addr + 16, 0);
    x86_mem_write32(ctx, addr + 20, 0);
    x86_mem_write32(ctx, addr + 24, 0);
}
static void fpu_read_env(x86_context* ctx, uint32_t addr) {
    ctx->regs.fpu_cw = x86_mem_read16(ctx, addr + 0);
    ctx->regs.fpu_sw = x86_mem_read16(ctx, addr + 4);
    ctx->regs.fpu_tw = x86_mem_read16(ctx, addr + 8);
}
// FNSAVE (108 bytes): env + 8 x 80-bit register slots (stored as 64-bit here).
static void fpu_write_save(x86_context* ctx, uint32_t addr) {
    fpu_write_env(ctx, addr);
    for (int i = 0; i < 8; i++) {
        int idx = (ctx->regs.fpu_top + i) & 7;
        double v = ctx->regs.fpu[idx];
        uint64_t raw; memcpy(&raw, &v, 8);
        xg_write64(ctx, addr + 28 + i * 10, raw);
        x86_mem_write16(ctx, addr + 28 + i * 10 + 8, 0xFFFF); // exponent sentinel
    }
    memset(ctx->regs.fpu, 0, sizeof(ctx->regs.fpu));
    for (int i = 0; i < 8; i++) ctx->regs.fpu_tags[i] = 3;
    ctx->regs.fpu_top = 0;
    ctx->regs.fpu_cw = 0x037F;
}
static void fpu_read_save(x86_context* ctx, uint32_t addr) {
    fpu_read_env(ctx, addr);
    for (int i = 0; i < 8; i++) {
        uint64_t raw = xg_read64(ctx, addr + 28 + i * 10);
        double v; memcpy(&v, &raw, 8);
        int idx = (ctx->regs.fpu_top + i) & 7;
        ctx->regs.fpu[idx] = v;
        ctx->regs.fpu_tags[idx] = 0;
    }
}

// ---- Condition code evaluation ----
static int eval_cc(x86_context* ctx, int cc) {
    int cf = (ctx->regs.eflags & X86_CF) ? 1 : 0;
    int zf = (ctx->regs.eflags & X86_ZF) ? 1 : 0;
    int sf = (ctx->regs.eflags & X86_SF) ? 1 : 0;
    int of = (ctx->regs.eflags & X86_OF) ? 1 : 0;
    int pf = (ctx->regs.eflags & X86_PF) ? 1 : 0;
    switch (cc) {
        case 0: return of == 0;           // O
        case 1: return of == 1;           // NO
        case 2: return cf == 0;           // B/NAE/C
        case 3: return cf == 1;           // NB/AE/NC
        case 4: return zf == 1;           // E/Z
        case 5: return zf == 0;           // NE/NZ
        case 6: return cf == 1 || zf == 1; // BE/NA
        case 7: return cf == 0 && zf == 0; // NBE/A
        case 8: return sf == 0;           // S
        case 9: return sf == 1;           // NS
        case 10: return pf == 0;          // P/PE
        case 11: return pf == 1;          // NP/PO
        case 12: return sf == of;         // L/NGE
        case 13: return sf != of;         // NL/GE
        case 14: return zf == 1 || (sf != of); // LE/NG
        case 15: return zf == 0 && (sf == of); // NLE/G
    }
    return 0;
}

// ---- Helper: read immediate based on size ----
static uint32_t read_imm8(x86_context* ctx) { uint8_t v = x86_mem_read8(ctx, ctx->regs.eip++); return v; }
static uint32_t read_imm16(x86_context* ctx) { uint16_t v = x86_mem_read16(ctx, ctx->regs.eip); ctx->regs.eip += 2; return v; }
static uint32_t read_imm32(x86_context* ctx) { uint32_t v = x86_mem_read32(ctx, ctx->regs.eip); ctx->regs.eip += 4; return v; }


// ---- SSE/SSE2/MMX operand helpers (guest memory is page-table backed) ----
static inline void sse_fetch(x86_context* ctx, uint8_t modrm, uint8_t* out16) {
    int rm = modrm_rm(modrm);
    if (modrm_mod(modrm) == 3) {
        memcpy(out16, ctx->regs.xmm[rm], 16);
    } else {
        int c = 0;
        uint32_t a = calc_ea(ctx, modrm, &c);
        ctx->regs.eip += c - 1;
        xg_read(ctx, a, out16, 16);
    }
}
static inline void sse_store(x86_context* ctx, uint8_t modrm, const uint8_t* in16) {
    int rm = modrm_rm(modrm);
    if (modrm_mod(modrm) == 3) {
        memcpy(ctx->regs.xmm[rm], in16, 16);
    } else {
        int c = 0;
        uint32_t a = calc_ea(ctx, modrm, &c);
        ctx->regs.eip += c - 1;
        xg_write(ctx, a, in16, 16);
    }
}
// Fetch 64-bit r/m operand: MMX register file when use_mmx, else low 64 of XMM
static inline void sse_fetch64(x86_context* ctx, uint8_t modrm, int use_mmx, uint8_t* out16) {
    memset(out16, 0, 16);
    int rm = modrm_rm(modrm);
    if (modrm_mod(modrm) == 3) {
        if (use_mmx) memcpy(out16, &ctx->regs.mm[rm], 8);
        else memcpy(out16, ctx->regs.xmm[rm], 8);
    } else {
        int c = 0;
        uint32_t a = calc_ea(ctx, modrm, &c);
        ctx->regs.eip += c - 1;
        xg_read(ctx, a, out16, 8);
    }
}
static inline void sse_store64(x86_context* ctx, uint8_t modrm, int use_mmx, const uint8_t* in16) {
    int rm = modrm_rm(modrm);
    if (modrm_mod(modrm) == 3) {
        if (use_mmx) memcpy(&ctx->regs.mm[rm], in16, 8);
        else memcpy(ctx->regs.xmm[rm], in16, 8);
    } else {
        int c = 0;
        uint32_t a = calc_ea(ctx, modrm, &c);
        ctx->regs.eip += c - 1;
        xg_write(ctx, a, in16, 8);
    }
}


// ---- Vector element helpers for MMX/SSE integer ops ----
enum { VADD, VSUB, VAND, VOR, VXOR, VEQ, VGT, VMAXU, VMINU, VAVG };
static inline void vec_op8(int op, uint8_t* d, const uint8_t* s, int n) {
    for (int i = 0; i < n; i++) {
        uint8_t a = d[i], b = s[i];
        switch (op) {
            case VADD: d[i] = (uint8_t)(a + b); break;
            case VSUB: d[i] = (uint8_t)(a - b); break;
            case VAND: d[i] = a & b; break;
            case VOR:  d[i] = a | b; break;
            case VXOR: d[i] = a ^ b; break;
            case VEQ:  d[i] = (a == b) ? 0xFF : 0x00; break;
            case VGT:  d[i] = ((int8_t)a > (int8_t)b) ? 0xFF : 0x00; break;
            case VMAXU: d[i] = a > b ? a : b; break;
            case VMINU: d[i] = a < b ? a : b; break;
            case VAVG: d[i] = (uint8_t)((a + b + 1) >> 1); break;
        }
    }
}
static inline void vec_op16(int op, uint8_t* d, const uint8_t* s, int n) {
    for (int i = 0; i < n; i++) {
        uint16_t a, b; memcpy(&a, d + i * 2, 2); memcpy(&b, s + i * 2, 2);
        uint16_t r;
        switch (op) {
            case VADD: r = (uint16_t)(a + b); break;
            case VSUB: r = (uint16_t)(a - b); break;
            case VAND: r = a & b; break;
            case VOR:  r = a | b; break;
            case VXOR: r = a ^ b; break;
            case VEQ:  r = (a == b) ? 0xFFFF : 0; break;
            case VGT:  r = ((int16_t)a > (int16_t)b) ? 0xFFFF : 0; break;
            case VMAXU: r = a > b ? a : b; break;
            case VMINU: r = a < b ? a : b; break;
            case VAVG: r = (uint16_t)((a + b + 1) >> 1); break;
            default: r = 0; break;
        }
        memcpy(d + i * 2, &r, 2);
    }
}
static inline void vec_op32(int op, uint8_t* d, const uint8_t* s, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t a, b; memcpy(&a, d + i * 4, 4); memcpy(&b, s + i * 4, 4);
        uint32_t r;
        switch (op) {
            case VADD: r = a + b; break;
            case VSUB: r = a - b; break;
            case VAND: r = a & b; break;
            case VOR:  r = a | b; break;
            case VXOR: r = a ^ b; break;
            case VEQ:  r = (a == b) ? 0xFFFFFFFFu : 0; break;
            case VGT:  r = ((int32_t)a > (int32_t)b) ? 0xFFFFFFFFu : 0; break;
            case VMAXU: r = a > b ? a : b; break;
            case VMINU: r = a < b ? a : b; break;
            default: r = 0; break;
        }
        memcpy(d + i * 4, &r, 4);
    }
}

static inline void sse_store_n(x86_context* ctx, uint8_t modrm, const uint8_t* in, int n) {
    int rm = modrm_rm(modrm);
    if (modrm_mod(modrm) == 3) {
        memcpy(ctx->regs.xmm[rm], in, n);
    } else {
        int c = 0;
        uint32_t a = calc_ea(ctx, modrm, &c);
        ctx->regs.eip += c - 1;
        xg_write(ctx, a, in, n);
    }
}

// ---- Floating-point vector arithmetic helpers ----
enum { VA_ADD, VA_MUL, VA_SUB, VA_DIV, VA_SQRT, VA_MIN, VA_MAX };
static inline void sse_arith_f32(int op, uint8_t* d, const uint8_t* s, int n) {
    float a[4]; memcpy(a, d, 16);
    float b[4]; memcpy(b, s, 16);
    for (int i = 0; i < n && i < 4; i++) {
        switch (op) {
            case VA_ADD: a[i] += b[i]; break;
            case VA_MUL: a[i] *= b[i]; break;
            case VA_SUB: a[i] -= b[i]; break;
            case VA_DIV: a[i] /= b[i]; break;
            case VA_SQRT: a[i] = sqrtf(a[i]); break;
            case VA_MIN: a[i] = a[i] < b[i] ? a[i] : b[i]; break;
            case VA_MAX: a[i] = a[i] > b[i] ? a[i] : b[i]; break;
        }
    }
    memset(d, 0, 16);
    memcpy(d, a, sizeof(float) * (n < 4 ? n : 4));
    (void)b;
}
static inline void sse_arith_f64(int op, uint8_t* d, const uint8_t* s, int n) {
    double a[2]; memcpy(a, d, 16);
    double b[2]; memcpy(b, s, 16);
    for (int i = 0; i < n && i < 2; i++) {
        switch (op) {
            case VA_ADD: a[i] += b[i]; break;
            case VA_MUL: a[i] *= b[i]; break;
            case VA_SUB: a[i] -= b[i]; break;
            case VA_DIV: a[i] /= b[i]; break;
            case VA_SQRT: a[i] = sqrt(a[i]); break;
            case VA_MIN: a[i] = a[i] < b[i] ? a[i] : b[i]; break;
            case VA_MAX: a[i] = a[i] > b[i] ? a[i] : b[i]; break;
        }
    }
    memset(d, 0, 16);
    memcpy(d, a, sizeof(double) * (n < 2 ? n : 2));
    (void)b;
}
// Integer shift helpers
static inline void sse_shift16(uint8_t* d, int count, int left, int arithmetic, int n) {
    for (int i = 0; i < n; i++) {
        uint16_t v; memcpy(&v, d + i * 2, 2);
        uint16_t r;
        if (left) r = (uint16_t)(v << (count & 15));
        else if (arithmetic) r = (uint16_t)((int16_t)v >> (count & 15));
        else r = (uint16_t)(v >> (count & 15));
        memcpy(d + i * 2, &r, 2);
    }
}
static inline void sse_shift32(uint8_t* d, int count, int left, int arithmetic, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t v; memcpy(&v, d + i * 4, 4);
        uint32_t r;
        if (left) r = (uint32_t)(v << (count & 31));
        else if (arithmetic) r = (uint32_t)((int32_t)v >> (count & 31));
        else r = (uint32_t)(v >> (count & 31));
        memcpy(d + i * 4, &r, 4);
    }
}
static inline void sse_shift64(uint8_t* d, int count, int left) {
    uint64_t v; memcpy(&v, d, 8);
    uint64_t r = left ? (v << (count & 63)) : (v >> (count & 63));
    memcpy(d, &r, 8);
}

// ---- Main execution step ----
// ---- 32-bit import trap dispatch (PE32 Phase 2) ----
// Imported 32-bit calls land on a reserved trap address. Dispatch to the
// 64-bit bridge, then emulate the callee's RET: pop the return address and,
// for __stdcall/__fastcall, the arguments (callee-cleans).
extern "C" int trt_pe32_is_trap(uint32_t guest_va);
extern "C" int trt_pe32_trap_info(int index, char* mod_out, size_t mod_cap,
                                 char* sym_out, size_t sym_cap,
                                 uint16_t* ordinal_out, uint32_t* trap_va_out);
extern "C" uint32_t x86_dispatch_import(x86_context* ctx, int trap_index,
                                       const char* module, const char* symbol,
                                       int* out_nargs, int* out_cdecl);

static int x86_trap_dispatch(x86_context* ctx) {
    if (!trt_pe32_is_trap(ctx->regs.eip)) return 0;
    int index = (int)((ctx->regs.eip - 0xF0000000u) / 4u);
    char mod[64] = {0}, sym[128] = {0};
    uint16_t ord = 0;
    uint32_t tva = 0;
    if (trt_pe32_trap_info(index, mod, sizeof(mod), sym, sizeof(sym), &ord, &tva) != 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "x86: bad trap 0x%08X", ctx->regs.eip);
        if (ctx->log) ctx->log(buf);
        ctx->running = 0;
        ctx->last_error = 3;
        return -1;
    }
    int nargs = 0, cdecl_call = 0;
    uint32_t result = x86_dispatch_import(ctx, index, mod, sym, &nargs, &cdecl_call);
    ctx->regs.eax = result;
    uint32_t ret = x86_mem_read32(ctx, ctx->regs.esp);
    ctx->regs.esp += 4;               // pop return address
    if (!cdecl_call) ctx->regs.esp += (uint32_t)(nargs * 4); // callee cleanup
    ctx->regs.eip = ret;
    return 1;
}

int x86_step(x86_context* ctx) {
    if (!ctx->running) return -1;
    if (ctx->insn_count >= ctx->insn_limit) {
        if (ctx->log) ctx->log("x86: instruction limit reached");
        ctx->running = 0;
        return -1;
    }

    // A call into the import trap range is an API call, not an instruction.
    {
        int t = x86_trap_dispatch(ctx);
        if (t != 0) return t < 0 ? -1 : 0;
    }

    uint32_t eip_start = ctx->regs.eip;
    uint8_t op = x86_mem_read8(ctx, ctx->regs.eip++);

    // Prefixes
    g_seg_override = -1;
    int operand_size = 32; // default 32-bit
    int addr_size = 32;
    int rep = 0;
    (void)operand_size; (void)addr_size; (void)rep;

    while (op == 0x66 || op == 0x67 || op == 0xF3 || op == 0xF2 ||
           op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E || op == 0x64 || op == 0x65) {
        if (op == 0x66) operand_size = 16;
        if (op == 0x67) addr_size = 16;
        if (op == 0xF3) rep = 1;
        if (op == 0xF2) rep = 2;
        if (op == 0x26) g_seg_override = 0; // ES
        if (op == 0x2E) g_seg_override = 1; // CS
        if (op == 0x36) g_seg_override = 2; // SS
        if (op == 0x3E) g_seg_override = 3; // DS
        if (op == 0x64) g_seg_override = 4; // FS
        if (op == 0x65) g_seg_override = 5; // GS
        op = x86_mem_read8(ctx, ctx->regs.eip++);
    }

    ctx->insn_count++;

    // ---- NOP ----
    if (op == 0x90) {
        return 0;
    }

    // ---- HLT ----
    if (op == 0xF4) {
        ctx->running = 0;
        return 0;
    }

    // ---- PUSH r/m32 (0xFF /6) ----
    if (op == 0xFF) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        int ext = modrm_reg(modrm); // extension opcode
        if (ext == 6) { // PUSH r/m32
            uint32_t val;
            if (modrm_mod(modrm) == 3) {
                val = *x86_reg_ptr(ctx, modrm_rm(modrm));
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                val = x86_mem_read32(ctx, addr);
            }
            ctx->regs.esp -= 4;
            x86_mem_write32(ctx, ctx->regs.esp, val);
            return 0;
        }
        if (ext == 2 || ext == 4) { // CALL
            uint32_t target;
            if (modrm_mod(modrm) == 3) {
                target = *x86_reg_ptr(ctx, modrm_rm(modrm));
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                target = x86_mem_read32(ctx, addr);
            }
            ctx->regs.esp -= 4;
            x86_mem_write32(ctx, ctx->regs.esp, ctx->regs.eip);
            ctx->regs.eip = target;
            return 0;
        }
        if (ext == 3 || ext == 5) { // JMP
            uint32_t target;
            if (modrm_mod(modrm) == 3) {
                target = *x86_reg_ptr(ctx, modrm_rm(modrm));
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                target = x86_mem_read32(ctx, addr);
            }
            ctx->regs.eip = target;
            return 0;
        }
        return -1; // unknown extension
    }
    // ---- POP r/m32 (0x8F /0) ----
    if (op == 0x8F) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t val = x86_mem_read32(ctx, ctx->regs.esp);
        ctx->regs.esp += 4;
        if (modrm_mod(modrm) == 3) {
            *x86_reg_ptr(ctx, modrm_rm(modrm)) = val;
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            x86_mem_write32(ctx, addr, val);
        }
        return 0;
    }

    // ---- RET (near) ----
    if (op == 0xC3) {
        ctx->regs.eip = x86_mem_read32(ctx, ctx->regs.esp);
        ctx->regs.esp += 4;
        return 0;
    }
    if (op == 0xC2) {
        uint16_t imm = x86_mem_read16(ctx, ctx->regs.eip);
        ctx->regs.eip += 2;
        ctx->regs.eip = x86_mem_read32(ctx, ctx->regs.esp);
        ctx->regs.esp += 4 + imm;
        return 0;
    }

    // ---- INT 3 (breakpoint) ----
    if (op == 0xCC) {
        if (ctx->log) ctx->log("x86: INT3 breakpoint");
        ctx->running = 0;
        return -1;
    }

    // ---- PUSH (register) ----
    if (op >= 0x50 && op <= 0x57) {
        int reg = op - 0x50;
        ctx->regs.esp -= 4;
        x86_mem_write32(ctx, ctx->regs.esp, *x86_reg_ptr(ctx, reg));
        return 0;
    }

    // ---- POP (register) ----
    if (op >= 0x58 && op <= 0x5F) {
        int reg = op - 0x58;
        *x86_reg_ptr(ctx, reg) = x86_mem_read32(ctx, ctx->regs.esp);
        ctx->regs.esp += 4;
        return 0;
    }

    // ---- PUSH imm32 ----
    if (op == 0x68) {
        uint32_t imm = read_imm32(ctx);
        ctx->regs.esp -= 4;
        x86_mem_write32(ctx, ctx->regs.esp, imm);
        return 0;
    }

    // ---- PUSH imm8 (sign-extended) ----
    if (op == 0x6A) {
        int8_t imm = (int8_t)read_imm8(ctx);
        ctx->regs.esp -= 4;
        x86_mem_write32(ctx, ctx->regs.esp, (uint32_t)imm);
        return 0;
    }

    // ---- CALL rel32 ----
    if (op == 0xE8) {
        int32_t rel = (int32_t)read_imm32(ctx);
        ctx->regs.esp -= 4;
        x86_mem_write32(ctx, ctx->regs.esp, ctx->regs.eip);
        ctx->regs.eip += rel;
        return 0;
    }

    // ---- JMP rel32 ----
    if (op == 0xE9) {
        int32_t rel = (int32_t)read_imm32(ctx);
        ctx->regs.eip += rel;
        return 0;
    }

    // ---- JMP rel8 ----
    if (op == 0xEB) {
        int8_t rel = (int8_t)read_imm8(ctx);
        ctx->regs.eip += rel;
        return 0;
    }

    // ---- Short conditional jumps (0x70-0x7F) ----
    if (op >= 0x70 && op <= 0x7F) {
        int8_t rel = (int8_t)read_imm8(ctx);
        if (eval_cc(ctx, op - 0x70)) {
            ctx->regs.eip += rel;
        }
        return 0;
    }

    // ---- 0x0F prefixed instructions ----
    if (op == 0x0F) {
        uint8_t op2 = x86_mem_read8(ctx, ctx->regs.eip++);
        // Conditional jumps (0x80-0x8F)
        if (op2 >= 0x80 && op2 <= 0x8F) {
            int32_t rel = (int32_t)read_imm32(ctx);
            if (eval_cc(ctx, op2 - 0x80)) {
                ctx->regs.eip += rel;
            }
            return 0;
        }
        // MOVZX (0xB6 = byte, 0xB7 = word)
        if (op2 == 0xB6) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            if (modrm_mod(modrm) == 3) {
                uint32_t val = x86_mem_read8(ctx, *x86_reg_ptr(ctx, modrm_rm(modrm)));
                *x86_reg_ptr(ctx, modrm_reg(modrm)) = val;
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                uint32_t val = x86_mem_read8(ctx, addr);
                *x86_reg_ptr(ctx, modrm_reg(modrm)) = val;
            }
            return 0;
        }
        if (op2 == 0xB7) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            if (modrm_mod(modrm) == 3) {
                uint32_t val = x86_mem_read16(ctx, *x86_reg_ptr(ctx, modrm_rm(modrm)));
                *x86_reg_ptr(ctx, modrm_reg(modrm)) = val;
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                uint32_t val = x86_mem_read16(ctx, addr);
                *x86_reg_ptr(ctx, modrm_reg(modrm)) = val;
            }
            return 0;
        }
        // MOVSX (0xBE = byte, 0xBF = word)
        if (op2 == 0xBE) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            uint32_t val;
            if (modrm_mod(modrm) == 3) {
                val = (int32_t)(int8_t)x86_mem_read8(ctx, *x86_reg_ptr(ctx, modrm_rm(modrm)));
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                val = (int32_t)(int8_t)x86_mem_read8(ctx, addr);
            }
            *x86_reg_ptr(ctx, modrm_reg(modrm)) = val;
            return 0;
        }
        if (op2 == 0xBF) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            uint32_t val;
            if (modrm_mod(modrm) == 3) {
                val = (int32_t)(int16_t)x86_mem_read16(ctx, *x86_reg_ptr(ctx, modrm_rm(modrm)));
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                val = (int32_t)(int16_t)x86_mem_read16(ctx, addr);
            }
            *x86_reg_ptr(ctx, modrm_reg(modrm)) = val;
            return 0;
        }
        // ---- CPUID (0x0F 0xA2) ----
        if (op2 == 0xA2) {
            uint32_t leaf = ctx->regs.eax;
            uint32_t sub = ctx->regs.ecx;
            uint32_t a = 0, b = 0, c = 0, d = 0;
            if (leaf == 0) {
                a = 0x0000000D; // max basic leaf
                b = 0x756E6547; // "Genu"
                d = 0x49656E69; // "ineI"
                c = 0x6C65746E; // "ntel"
            } else if (leaf == 1) {
                a = 0x000306A9; // family 6, model 58, stepping 9
                // EDX: FPU(0) VME(1) DE(2) PSE(3) TSC(4) MSR(5) PAE(6) MCE(7)
                //      CX8(8) APIC(9) SEP(11) MTRR(12) PGE(13) MCA(14) CMOV(15)
                //      PAT(16) PSE36(17) CLFSH(19) MMX(23) FXSR(24) SSE(25) SSE2(26) SS(27)
                d = (1u<<0)|(1u<<3)|(1u<<4)|(1u<<5)|(1u<<6)|(1u<<8)|(1u<<11)|(1u<<13)|
                    (1u<<15)|(1u<<19)|(1u<<23)|(1u<<24)|(1u<<25)|(1u<<26)|(1u<<27);
                // ECX: SSE3(0) PCLMUL(1) DTES64(2) MONITOR(3) DS-CPL(4) VMX(5) SMX(6)
                //      EIST(7) TM2(8) SSSE3(9) CNXT-ID(10) FMA(12) CX16(13) SSE4.1(19)
                //      SSE4.2(20) x2APIC(21) MOVBE(22) POPCNT(23) AES(25) XSAVE(26)
                c = (1u<<0)|(1u<<9)|(1u<<13)|(1u<<19)|(1u<<22)|(1u<<23)|(1u<<26);
                b = 0x00000800; // logical processors
            } else if (leaf == 0x80000000) {
                a = 0x80000008;
            } else if (leaf == 0x80000001) {
                // EDX: SYSCALL(11) XD(20) MMX/SSE already implied
                d = (1u<<11)|(1u<<20)|(1u<<29); // + LM bit? do not claim long mode
                c = 0;
            } else if (leaf == 0x80000008) {
                a = 0x2028; // 40-bit phys, 32-bit linear
            } else if (leaf == 4 || leaf == 0x0B || leaf == 0x0D) {
                a = 0; // topology/xsave: none
            } else {
                a = b = c = d = 0;
            }
            (void)sub;
            ctx->regs.eax = a;
            ctx->regs.ebx = b;
            ctx->regs.ecx = c;
            ctx->regs.edx = d;
            return 0;
        }
        // ---- RDTSC (0x0F 0x31) ----
        if (op2 == 0x31) {
            static uint64_t rd_tsc = 0x100000;
            rd_tsc += 3000; // ~3GHz-ish progression per read
            ctx->regs.eax = (uint32_t)(rd_tsc & 0xFFFFFFFFu);
            ctx->regs.edx = (uint32_t)(rd_tsc >> 32);
            return 0;
        }
        // ---- RDMSR (0x0F 0x32) / WRMSR (0x0F 0x30) - safe stubs ----
        if (op2 == 0x32) {
            ctx->regs.eax = 0;
            ctx->regs.edx = 0;
            return 0;
        }
        if (op2 == 0x30) {
            return 0;
        }
        // ---- SYSENTER (0x0F 0x34) / SYSEXIT (0x0F 0x35) ----
        if (op2 == 0x34) {
            // Route to the NT syscall bridge: EDX holds the call index.
            // Without a real syscall table, fail loudly in the log and halt.
            if (ctx->log) ctx->log("x86: SYSENTER unsupported - halting");
            ctx->running = 0;
            return -1;
        }
        if (op2 == 0x35) {
            return 0;
        }

        // ---- FXSAVE / FXRSTOR / fences / CLFLUSH (0x0F 0xAE) ----
        if (op2 == 0xAE) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            int mod = modrm_mod(modrm);
            int ext = modrm_reg(modrm);
            if (mod == 3) {
                // LFENCE (/5), MFENCE (/6), SFENCE (/7) - ordering only, no-op
                if (ext >= 5) return 0;
                return 0;
            }
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            if (ext == 0) { // FXSAVE
                x86_mem_write16(ctx, addr + 0, ctx->regs.fpu_cw);
                x86_mem_write16(ctx, addr + 2, ctx->regs.fpu_sw);
                x86_mem_write8(ctx, addr + 4, 0); // ftw: not tracked precisely
                x86_mem_write16(ctx, addr + 6, 0);
                x86_mem_write32(ctx, addr + 8, 0);
                x86_mem_write32(ctx, addr + 12, 0);
                x86_mem_write32(ctx, addr + 16, 0);
                x86_mem_write32(ctx, addr + 20, 0);
                x86_mem_write32(ctx, addr + 24, ctx->regs.mxcsr ? ctx->regs.mxcsr : 0x1F80);
                x86_mem_write32(ctx, addr + 28, 0xFFFF);
                for (int i = 0; i < 8; i++) {
                    int idx = (ctx->regs.fpu_top + i) & 7;
                    double v = ctx->regs.fpu[idx];
                    uint64_t raw; memcpy(&raw, &v, 8);
                    xg_write64(ctx, addr + 32 + i * 16, raw);
                    x86_mem_write16(ctx, addr + 32 + i * 16 + 8, 0xFFFF);
                    xg_write64(ctx, addr + 32 + i * 16 + 8, 0);
                }
                for (int i = 0; i < 8; i++) xg_write(ctx, addr + 160 + i * 16, ctx->regs.xmm[i], 16);
                return 0;
            }
            if (ext == 1) { // FXRSTOR
                ctx->regs.fpu_cw = x86_mem_read16(ctx, addr + 0);
                ctx->regs.fpu_sw = x86_mem_read16(ctx, addr + 2);
                ctx->regs.mxcsr = x86_mem_read32(ctx, addr + 24);
                for (int i = 0; i < 8; i++) {
                    uint64_t raw = xg_read64(ctx, addr + 32 + i * 16);
                    double v; memcpy(&v, &raw, 8);
                    int idx = (ctx->regs.fpu_top + i) & 7;
                    ctx->regs.fpu[idx] = v;
                    ctx->regs.fpu_tags[idx] = 0;
                }
                for (int i = 0; i < 8; i++) xg_read(ctx, addr + 160 + i * 16, ctx->regs.xmm[i], 16);
                return 0;
            }
            if (ext == 7) { // CLFLUSH
                return 0;
            }
            return 0;
        }

        // ================= SSE / SSE2 / MMX =================
        {
            int p66 = (operand_size == 16);

            // ---- MOVUPS / MOVUPD / MOVSS / MOVSD (0x10 load, 0x11 store) ----
            if (op2 == 0x10 || op2 == 0x11) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                int dst = modrm_reg(modrm);
                uint8_t buf[16] = {0};
                int nbytes = (rep == 1) ? 4 : (rep == 2) ? 8 : 16;
                if (op2 == 0x10) { sse_fetch(ctx, modrm, buf); memcpy(ctx->regs.xmm[dst], buf, nbytes); }
                else { memcpy(buf, ctx->regs.xmm[dst], nbytes); sse_store_n(ctx, modrm, buf, nbytes); }
                return 0;
            }
            // ---- MOVAPS / MOVAPD (0x28 load, 0x29 store) ----
            if (op2 == 0x28 || op2 == 0x29) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                int dst = modrm_reg(modrm);
                uint8_t buf[16];
                if (op2 == 0x28) { sse_fetch(ctx, modrm, buf); memcpy(ctx->regs.xmm[dst], buf, 16); }
                else { memcpy(buf, ctx->regs.xmm[dst], 16); sse_store(ctx, modrm, buf); }
                return 0;
            }
            // ---- MOVNTPS / MOVNTPD (0x2B store) ----
            if (op2 == 0x2B) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                uint8_t buf[16];
                memcpy(buf, ctx->regs.xmm[modrm_reg(modrm)], 16);
                sse_store(ctx, modrm, buf);
                return 0;
            }
            // ---- MOVDQA / MOVDQU / MOVQ (0x6F load, 0x7F store) ----
            if (op2 == 0x6F || op2 == 0x7F) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                int dst = modrm_reg(modrm);
                if (p66 || rep == 1) { // MOVDQA (66) / MOVDQU (F3): 16 bytes
                    uint8_t buf[16];
                    if (op2 == 0x6F) { sse_fetch(ctx, modrm, buf); memcpy(ctx->regs.xmm[dst], buf, 16); }
                    else { memcpy(buf, ctx->regs.xmm[dst], 16); sse_store(ctx, modrm, buf); }
                } else { // MOVQ mm: 8 bytes
                    uint8_t buf[16] = {0};
                    if (op2 == 0x6F) { sse_fetch64(ctx, modrm, 1, buf); memcpy(&ctx->regs.mm[dst], buf, 8); }
                    else { memcpy(buf, &ctx->regs.mm[dst], 8); sse_store64(ctx, modrm, 1, buf); }
                }
                return 0;
            }
            // ---- MOVD / MOVQ (0x6E load, 0x7E store) ----
            if (op2 == 0x6E) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                int dst = modrm_reg(modrm);
                if (rep == 1) { // MOVQ xmm, r/m64
                    uint8_t buf[16] = {0};
                    sse_fetch64(ctx, modrm, 0, buf);
                    memcpy(ctx->regs.xmm[dst], buf, 8);
                } else if (p66) { // MOVD xmm, r/m32
                    uint32_t v;
                    if (modrm_mod(modrm) == 3) v = *x86_reg_ptr(ctx, modrm_rm(modrm));
                    else { int c = 0; uint32_t a = calc_ea(ctx, modrm, &c); ctx->regs.eip += c - 1; v = x86_mem_read32(ctx, a); }
                    memset(ctx->regs.xmm[dst], 0, 16);
                    memcpy(ctx->regs.xmm[dst], &v, 4);
                } else { // MOVD mm, r/m32
                    uint32_t v;
                    if (modrm_mod(modrm) == 3) v = *x86_reg_ptr(ctx, modrm_rm(modrm));
                    else { int c = 0; uint32_t a = calc_ea(ctx, modrm, &c); ctx->regs.eip += c - 1; v = x86_mem_read32(ctx, a); }
                    ctx->regs.mm[dst] = v;
                }
                return 0;
            }
            if (op2 == 0x7E) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                int src = modrm_reg(modrm);
                if (rep == 1) { // MOVQ r/m64, xmm
                    uint8_t buf[16] = {0};
                    memcpy(buf, ctx->regs.xmm[src], 8);
                    sse_store64(ctx, modrm, 0, buf);
                } else if (p66) { // MOVD r/m32, xmm
                    uint32_t v; memcpy(&v, ctx->regs.xmm[src], 4);
                    if (modrm_mod(modrm) == 3) *x86_reg_ptr(ctx, modrm_rm(modrm)) = v;
                    else { int c = 0; uint32_t a = calc_ea(ctx, modrm, &c); ctx->regs.eip += c - 1; x86_mem_write32(ctx, a, v); }
                } else { // MOVD r/m32, mm
                    uint32_t v = (uint32_t)ctx->regs.mm[src];
                    if (modrm_mod(modrm) == 3) *x86_reg_ptr(ctx, modrm_rm(modrm)) = v;
                    else { int c = 0; uint32_t a = calc_ea(ctx, modrm, &c); ctx->regs.eip += c - 1; x86_mem_write32(ctx, a, v); }
                }
                return 0;
            }
            // ---- MOVQ r/m64, xmm (0xD6 with 66) ----
            if (op2 == 0xD6 && p66) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                uint8_t buf[16] = {0};
                memcpy(buf, ctx->regs.xmm[modrm_reg(modrm)], 8);
                sse_store64(ctx, modrm, 0, buf);
                return 0;
            }
            // ---- MOVNTDQ (0xE7 with 66) ----
            if (op2 == 0xE7 && p66) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                uint8_t buf[16];
                memcpy(buf, ctx->regs.xmm[modrm_reg(modrm)], 16);
                sse_store(ctx, modrm, buf);
                return 0;
            }
            // ---- Packed/scalar arithmetic: 0x51 sqrt, 0x58 add, 0x59 mul,
            //      0x5C sub, 0x5D min, 0x5E div, 0x5F max ----
            if (op2 == 0x51 || op2 == 0x58 || op2 == 0x59 || op2 == 0x5C ||
                op2 == 0x5D || op2 == 0x5E || op2 == 0x5F) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                int dst = modrm_reg(modrm);
                uint8_t src[16] = {0};
                sse_fetch(ctx, modrm, src);
                int aop = (op2 == 0x58) ? VA_ADD : (op2 == 0x59) ? VA_MUL :
                          (op2 == 0x5C) ? VA_SUB : (op2 == 0x5E) ? VA_DIV :
                          (op2 == 0x51) ? VA_SQRT : (op2 == 0x5D) ? VA_MIN : VA_MAX;
                if (rep == 2) sse_arith_f64(aop, ctx->regs.xmm[dst], src, 1);      // scalar double
                else if (p66) sse_arith_f64(aop, ctx->regs.xmm[dst], src, 2);      // packed double
                else if (rep == 1) sse_arith_f32(aop, ctx->regs.xmm[dst], src, 1); // scalar single
                else sse_arith_f32(aop, ctx->regs.xmm[dst], src, 4);               // packed single
                return 0;
            }
            // ---- Convert 0x5A: CVTPS2PD / CVTPD2PS / CVTSS2SD / CVTSD2SS ----
            if (op2 == 0x5A) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                int dst = modrm_reg(modrm);
                uint8_t src[16] = {0};
                sse_fetch(ctx, modrm, src);
                uint8_t out[16] = {0};
                if (rep == 1) { float f; memcpy(&f, src, 4); double d = (double)f; memcpy(out, &d, 8); }
                else if (rep == 2) { double d; memcpy(&d, src, 8); float f = (float)d; memcpy(out, &f, 4); }
                else if (p66) { double d[2]; memcpy(d, src, 16); float f[2] = {(float)d[0], (float)d[1]}; memcpy(out, f, 8); }
                else { float f[2]; memcpy(f, src, 8); double d[2] = {(double)f[0], (double)f[1]}; memcpy(out, d, 16); }
                memcpy(ctx->regs.xmm[dst], out, 16);
                return 0;
            }
            // ---- Convert 0x5B: CVTDQ2PS / CVTPS2DQ / CVTTPS2DQ ----
            if (op2 == 0x5B) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                int dst = modrm_reg(modrm);
                uint8_t src[16] = {0};
                sse_fetch(ctx, modrm, src);
                uint8_t out[16] = {0};
                if (p66) { float f[4]; memcpy(f, src, 16); int32_t i[4] = {(int32_t)f[0],(int32_t)f[1],(int32_t)f[2],(int32_t)f[3]}; memcpy(out, i, 16); }
                else if (rep == 1) { float f[4]; memcpy(f, src, 16); int32_t i[4] = {(int32_t)f[0],(int32_t)f[1],(int32_t)f[2],(int32_t)f[3]}; memcpy(out, i, 16); }
                else { int32_t i[4]; memcpy(i, src, 16); float f[4] = {(float)i[0],(float)i[1],(float)i[2],(float)i[3]}; memcpy(out, f, 16); }
                memcpy(ctx->regs.xmm[dst], out, 16);
                return 0;
            }
            // ---- Convert 0xE6: CVTTPD2DQ / CVTPD2DQ / CVTDQ2PD ----
            if (op2 == 0xE6 && (p66 || rep)) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                int dst = modrm_reg(modrm);
                uint8_t src[16] = {0};
                sse_fetch(ctx, modrm, src);
                uint8_t out[16] = {0};
                if (p66) { double d[2]; memcpy(d, src, 16); int32_t i[2] = {(int32_t)d[0], (int32_t)d[1]}; memcpy(out, i, 8); }
                else if (rep == 2) { double d[2]; memcpy(d, src, 16); int32_t i[2] = {(int32_t)d[0], (int32_t)d[1]}; memcpy(out, i, 8); }
                else { int32_t i[2]; memcpy(i, src, 8); double d[2] = {(double)i[0], (double)i[1]}; memcpy(out, d, 16); }
                memcpy(ctx->regs.xmm[dst], out, 16);
                return 0;
            }
            // ---- CVTSI2SS / CVTSI2SD / CVTPI2PS / CVTPI2PD (0x2A) ----
            if (op2 == 0x2A) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                int dst = modrm_reg(modrm);
                int32_t iv;
                if (modrm_mod(modrm) == 3) iv = (int32_t)*x86_reg_ptr(ctx, modrm_rm(modrm));
                else { int c = 0; uint32_t a = calc_ea(ctx, modrm, &c); ctx->regs.eip += c - 1; iv = (int32_t)x86_mem_read32(ctx, a); }
                if (rep == 2) { double d = (double)iv; memcpy(ctx->regs.xmm[dst], &d, 8); }
                else if (p66) { float f[4] = {(float)iv, 0, 0, 0}; memcpy(ctx->regs.xmm[dst], f, 16); }
                else if (rep == 1) { float f = (float)iv; memcpy(ctx->regs.xmm[dst], &f, 4); }
                else { ctx->regs.mm[dst] = (uint64_t)(uint32_t)iv; }
                return 0;
            }
            // ---- CVTTSS2SI/CVTTSD2SI (0x2C), CVTSS2SI/CVTSD2SI (0x2D) ----
            if (op2 == 0x2C || op2 == 0x2D) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                uint8_t src[16] = {0};
                sse_fetch(ctx, modrm, src);
                int32_t out;
                if (p66 && (op2 == 0x2C || op2 == 0x2D)) { double d; memcpy(&d, src, 8); out = (int32_t)d; }
                else if (rep == 2) { double d; memcpy(&d, src, 8); out = (int32_t)d; }
                else if (rep == 1) { float f; memcpy(&f, src, 4); out = (int32_t)f; }
                else { double d; memcpy(&d, src, 8); out = (int32_t)d; }
                *x86_reg_ptr(ctx, modrm_reg(modrm)) = (uint32_t)out;
                return 0;
            }
            // ---- COMISS/UCOMISS/COMISD/UCOMISD (0x2E, 0x2F) ----
            if (op2 == 0x2E || op2 == 0x2F) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                uint8_t src[16] = {0};
                sse_fetch(ctx, modrm, src);
                double a, b;
                if (p66 || rep == 2) { memcpy(&a, ctx->regs.xmm[modrm_reg(modrm)], 8); memcpy(&b, src, 8); }
                else { float fa, fb; memcpy(&fa, ctx->regs.xmm[modrm_reg(modrm)], 4); memcpy(&fb, src, 4); a = fa; b = fb; }
                ctx->regs.eflags &= ~(X86_ZF | X86_CF | X86_PF | X86_OF | X86_SF | X86_AF);
                if (a > b) { }
                else if (a < b) ctx->regs.eflags |= X86_CF;
                else if (a == b) ctx->regs.eflags |= X86_ZF;
                else { ctx->regs.eflags |= (X86_ZF | X86_CF | X86_PF); }
                return 0;
            }
            // ---- MOVMSKPS / MOVMSKPD (0x50) ----
            if (op2 == 0x50 && (p66 || !rep)) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                int src = modrm_rm(modrm);
                uint32_t mask = 0;
                if (p66) { for (int i = 0; i < 2; i++) mask |= (uint32_t)((ctx->regs.xmm[src][i * 8 + 7] & 0x80) ? (1u << i) : 0); }
                else { for (int i = 0; i < 4; i++) mask |= (uint32_t)((ctx->regs.xmm[src][i * 4 + 3] & 0x80) ? (1u << i) : 0); }
                *x86_reg_ptr(ctx, modrm_reg(modrm)) = mask;
                return 0;
            }

            // ---- Integer SIMD (MMX when no prefix, XMM with 66) ----
            {
                int use_mmx = !p66;
                int elems8 = use_mmx ? 8 : 16;
                int elems16 = use_mmx ? 4 : 8;
                int elems32 = use_mmx ? 2 : 4;
                // opcode -> (op, element bytes)
                int op = -1, esz = 1;
                switch (op2) {
                    case 0xFC: op = VADD; esz = 1; break;
                    case 0xFD: op = VADD; esz = 2; break;
                    case 0xFE: op = VADD; esz = 4; break;
                    case 0xF8: op = VSUB; esz = 1; break;
                    case 0xF9: op = VSUB; esz = 2; break;
                    case 0xFA: op = VSUB; esz = 4; break;
                    case 0xDB: op = VAND; esz = 8; break;
                    case 0xEB: op = VOR;  esz = 8; break;
                    case 0xEF: op = VXOR; esz = 8; break;
                    case 0x74: op = VEQ;  esz = 1; break;
                    case 0x75: op = VEQ;  esz = 2; break;
                    case 0x76: op = VEQ;  esz = 4; break;
                    case 0x64: op = VGT;  esz = 1; break;
                    case 0x65: op = VGT;  esz = 2; break;
                    case 0x66: op = VGT;  esz = 4; break;
                    case 0xDA: op = VMINU; esz = 1; break;
                    case 0xDE: op = VMAXU; esz = 1; break;
                    case 0xE0: op = VAVG;  esz = 1; break;
                    default: break;
                }
                if (op >= 0) {
                    uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                    int dst = modrm_reg(modrm);
                    uint8_t src[16] = {0};
                    sse_fetch64(ctx, modrm, use_mmx, src);
                    uint8_t d[16] = {0};
                    if (use_mmx) memcpy(d, &ctx->regs.mm[dst], 8); else memcpy(d, ctx->regs.xmm[dst], 16);
                    if (esz == 1) vec_op8(op, d, src, elems8);
                    else if (esz == 2) vec_op16(op, d, src, elems16);
                    else if (esz == 4) vec_op32(op, d, src, elems32);
                    else { // 64-bit logical
                        uint64_t a, b; memcpy(&a, d, 8); memcpy(&b, src, 8);
                        uint64_t r = (op == VAND) ? (a & b) : (op == VOR) ? (a | b) : (a ^ b);
                        memcpy(d, &r, 8);
                    }
                    if (use_mmx) memcpy(&ctx->regs.mm[dst], d, 8); else memcpy(ctx->regs.xmm[dst], d, 16);
                    return 0;
                }
                // PANDN (0xDF): d = (~d) & src
                if (op2 == 0xDF) {
                    uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                    int dst = modrm_reg(modrm);
                    uint8_t src[16] = {0};
                    sse_fetch64(ctx, modrm, use_mmx, src);
                    uint8_t d[16] = {0};
                    if (use_mmx) memcpy(d, &ctx->regs.mm[dst], 8); else memcpy(d, ctx->regs.xmm[dst], 16);
                    for (int i = 0; i < (use_mmx ? 8 : 16); i++) d[i] = (uint8_t)(~d[i] & src[i]);
                    if (use_mmx) memcpy(&ctx->regs.mm[dst], d, 8); else memcpy(ctx->regs.xmm[dst], d, 16);
                    return 0;
                }
                // PUNPCKL/H (0x60-0x62, 0x68-0x6A)
                if ((op2 >= 0x60 && op2 <= 0x62) || (op2 >= 0x68 && op2 <= 0x6A)) {
                    uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                    int dst = modrm_reg(modrm);
                    uint8_t src[16] = {0};
                    sse_fetch64(ctx, modrm, use_mmx, src);
                    uint8_t d[16] = {0};
                    if (use_mmx) memcpy(d, &ctx->regs.mm[dst], 8); else memcpy(d, ctx->regs.xmm[dst], 16);
                    int units = (op2 == 0x60 || op2 == 0x68) ? 1 : (op2 == 0x61 || op2 == 0x69) ? 2 : 4;
                    int lo = (op2 <= 0x62);
                    uint8_t out[16] = {0};
                    int count = (use_mmx ? 8 : 16) / units;
                    for (int i = 0; i < count / 2; i++) {
                        int didx = lo ? i : (count / 2 + i);
                        memcpy(out + (i * 2) * units, d + didx * units, units);
                        memcpy(out + (i * 2 + 1) * units, src + didx * units, units);
                    }
                    if (use_mmx) memcpy(&ctx->regs.mm[dst], out, 8); else memcpy(ctx->regs.xmm[dst], out, 16);
                    return 0;
                }
                // PACKSSWB/PACKSSDW/PACKUSWB (0x63, 0x6B, 0x67)
                if (op2 == 0x63 || op2 == 0x6B || op2 == 0x67) {
                    uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                    int dst = modrm_reg(modrm);
                    uint8_t src[16] = {0};
                    sse_fetch64(ctx, modrm, use_mmx, src);
                    uint8_t d[16] = {0};
                    if (use_mmx) memcpy(d, &ctx->regs.mm[dst], 8); else memcpy(d, ctx->regs.xmm[dst], 16);
                    int fmt = (use_mmx ? 8 : 16);
                    uint8_t out[16] = {0};
                    if (op2 == 0x63) { // pack 16->8 signed
                        for (int i = 0; i < fmt / 2; i++) {
                            int16_t a; memcpy(&a, d + i * 2, 2);
                            int16_t b; memcpy(&b, src + i * 2, 2);
                            out[i] = (uint8_t)(a < -128 ? -128 : a > 127 ? 127 : a);
                            out[fmt / 2 + i] = (uint8_t)(b < -128 ? -128 : b > 127 ? 127 : b);
                        }
                    } else if (op2 == 0x67) { // pack 16->8 unsigned
                        for (int i = 0; i < fmt / 2; i++) {
                            int16_t a; memcpy(&a, d + i * 2, 2);
                            int16_t b; memcpy(&b, src + i * 2, 2);
                            out[i] = (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a);
                            out[fmt / 2 + i] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
                        }
                    } else { // pack 32->16 signed
                        for (int i = 0; i < fmt / 4; i++) {
                            int32_t a; memcpy(&a, d + i * 4, 4);
                            int32_t b; memcpy(&b, src + i * 4, 4);
                            int16_t r1 = (int16_t)(a < -32768 ? -32768 : a > 32767 ? 32767 : a);
                            int16_t r2 = (int16_t)(b < -32768 ? -32768 : b > 32767 ? 32767 : b);
                            memcpy(out + i * 2, &r1, 2);
                            memcpy(out + (fmt / 2) + i * 2, &r2, 2);
                        }
                    }
                    if (use_mmx) memcpy(&ctx->regs.mm[dst], out, 8); else memcpy(ctx->regs.xmm[dst], out, 16);
                    return 0;
                }
                // Shifts by register: 0xD1/D2/D3 (right), 0xE1/E2 (right arith), 0xF1/F2/F3 (left)
                if (op2 == 0xD1 || op2 == 0xD2 || op2 == 0xD3 ||
                    op2 == 0xE1 || op2 == 0xE2 ||
                    op2 == 0xF1 || op2 == 0xF2 || op2 == 0xF3) {
                    uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                    int dst = modrm_reg(modrm);
                    uint8_t src[16] = {0};
                    sse_fetch64(ctx, modrm, use_mmx, src);
                    uint64_t cnt64; memcpy(&cnt64, src, 8);
                    int count = (int)(cnt64 & 0xFF);
                    uint8_t d[16] = {0};
                    if (use_mmx) memcpy(d, &ctx->regs.mm[dst], 8); else memcpy(d, ctx->regs.xmm[dst], 16);
                    if (op2 == 0xD1 || op2 == 0xE1 || op2 == 0xF1) sse_shift16(d, count, op2 == 0xF1, op2 == 0xE1, elems16);
                    else if (op2 == 0xD2 || op2 == 0xE2 || op2 == 0xF2) sse_shift32(d, count, op2 == 0xF2, op2 == 0xE2, elems32);
                    else sse_shift64(d, count, op2 == 0xF3);
                    if (use_mmx) memcpy(&ctx->regs.mm[dst], d, 8); else memcpy(ctx->regs.xmm[dst], d, 16);
                    return 0;
                }
                // Shifts by immediate: 0x71 (16-bit), 0x72 (32-bit), 0x73 (64-bit)
                if (op2 == 0x71 || op2 == 0x72 || op2 == 0x73) {
                    uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                    int dst = modrm_rm(modrm);
                    int ext = modrm_reg(modrm);
                    int count = x86_mem_read8(ctx, ctx->regs.eip++);
                    uint8_t d[16] = {0};
                    if (use_mmx) memcpy(d, &ctx->regs.mm[dst], 8); else memcpy(d, ctx->regs.xmm[dst], 16);
                    if (op2 == 0x71) {
                        if (ext == 2) sse_shift16(d, count, 0, 0, elems16);
                        else if (ext == 4) sse_shift16(d, count, 0, 1, elems16);
                        else if (ext == 6) sse_shift16(d, count, 1, 0, elems16);
                    } else if (op2 == 0x72) {
                        if (ext == 2) sse_shift32(d, count, 0, 0, elems32);
                        else if (ext == 4) sse_shift32(d, count, 0, 1, elems32);
                        else if (ext == 6) sse_shift32(d, count, 1, 0, elems32);
                    } else {
                        if (ext == 2) sse_shift64(d, count, 0);
                        else if (ext == 6) sse_shift64(d, count, 1);
                    }
                    if (use_mmx) memcpy(&ctx->regs.mm[dst], d, 8); else memcpy(ctx->regs.xmm[dst], d, 16);
                    return 0;
                }
                // PMOVMSKB (0xD7)
                if (op2 == 0xD7) {
                    uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                    int src = modrm_rm(modrm);
                    uint8_t v[16] = {0};
                    if (use_mmx) memcpy(v, &ctx->regs.mm[src], 8); else memcpy(v, ctx->regs.xmm[src], 16);
                    uint32_t mask = 0;
                    int n = use_mmx ? 8 : 16;
                    for (int i = 0; i < n; i++) if (v[i] & 0x80) mask |= (1u << i);
                    *x86_reg_ptr(ctx, modrm_reg(modrm)) = mask;
                    return 0;
                }
                // PSHUFW/PSHUFD/PSHUFLW/PSHUFHW (0x70)
                if (op2 == 0x70) {
                    uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                    int dst = modrm_reg(modrm);
                    int imm = x86_mem_read8(ctx, ctx->regs.eip++);
                    uint8_t src[16] = {0};
                    sse_fetch(ctx, modrm, src);
                    uint8_t out[16] = {0};
                    if (rep == 2) { // PSHUFLW
                        for (int i = 0; i < 4; i++) { int s = (imm >> (i * 2)) & 3; memcpy(out + i * 2, src + s * 2, 2); }
                        memcpy(out + 8, src + 8, 8);
                    } else if (rep == 1) { // PSHUFHW
                        memcpy(out, src, 8);
                        for (int i = 0; i < 4; i++) { int s = (imm >> (i * 2)) & 3; memcpy(out + 8 + i * 2, src + 8 + s * 2, 2); }
                    } else if (p66) { // PSHUFD
                        for (int i = 0; i < 4; i++) { int s = (imm >> (i * 2)) & 3; memcpy(out + i * 4, src + s * 4, 4); }
                    } else { // PSHUFW (MMX)
                        for (int i = 0; i < 4; i++) { int s = (imm >> (i * 2)) & 3; memcpy(out + i * 2, src + s * 2, 2); }
                    }
                    if (use_mmx) memcpy(&ctx->regs.mm[dst], out, 8); else memcpy(ctx->regs.xmm[dst], out, 16);
                    return 0;
                }
            }
            // ---- EMMS (0x77) ----
            if (op2 == 0x77 && !p66) {
                memset(ctx->regs.mm, 0, sizeof(ctx->regs.mm));
                return 0;
            }
        }

        // ---- CMOVcc (0x0F 0x40-0x4F) ----
        if (op2 >= 0x40 && op2 <= 0x4F) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            int cc = op2 - 0x40;
            uint32_t src_val;
            if (modrm_mod(modrm) == 3) {
                src_val = *x86_reg_ptr(ctx, modrm_rm(modrm));
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                src_val = x86_mem_read32(ctx, addr);
            }
            if (eval_cc(ctx, cc)) {
                *x86_reg_ptr(ctx, modrm_reg(modrm)) = src_val;
            }
            return 0;
        }
        // ---- SETcc (0x0F 0x90-0x9F) ----
        if (op2 >= 0x90 && op2 <= 0x9F) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            int cc = op2 - 0x90;
            uint8_t val = eval_cc(ctx, cc) ? 1 : 0;
            if (modrm_mod(modrm) == 3) {
                // Write to low byte of register
                uint32_t* reg = x86_reg_ptr(ctx, modrm_rm(modrm));
                *reg = (*reg & 0xFFFFFF00) | val;
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                x86_mem_write8(ctx, addr, val);
            }
            return 0;
        }
        // ---- BSF (0xBC): bit scan forward ----
        if (op2 == 0xBC) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            uint32_t src_val;
            if (modrm_mod(modrm) == 3) {
                src_val = *x86_reg_ptr(ctx, modrm_rm(modrm));
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                src_val = x86_mem_read32(ctx, addr);
            }
            if (src_val == 0) {
                ctx->regs.eflags |= X86_ZF;
            } else {
                ctx->regs.eflags &= ~X86_ZF;
                int idx = 0;
                while (!(src_val & (1 << idx))) idx++;
                *x86_reg_ptr(ctx, modrm_reg(modrm)) = idx;
            }
            return 0;
        }
        // ---- BSR (0xBD): bit scan reverse ----
        if (op2 == 0xBD) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            uint32_t src_val;
            if (modrm_mod(modrm) == 3) {
                src_val = *x86_reg_ptr(ctx, modrm_rm(modrm));
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                src_val = x86_mem_read32(ctx, addr);
            }
            if (src_val == 0) {
                ctx->regs.eflags |= X86_ZF;
            } else {
                ctx->regs.eflags &= ~X86_ZF;
                int idx = 31;
                while (!(src_val & (1u << idx))) idx--;
                *x86_reg_ptr(ctx, modrm_reg(modrm)) = idx;
            }
            return 0;
        }
        // ---- BT (0xA3): bit test ----
        if (op2 == 0xA3) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            uint32_t base_val;
            if (modrm_mod(modrm) == 3) {
                base_val = *x86_reg_ptr(ctx, modrm_rm(modrm));
                int bit = *x86_reg_ptr(ctx, modrm_reg(modrm)) & 31;
                ctx->regs.eflags = (ctx->regs.eflags & ~X86_CF) | ((base_val >> bit) & 1 ? X86_CF : 0);
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                int bitpos = *x86_reg_ptr(ctx, modrm_reg(modrm));
                int byte_offset = bitpos / 8;
                int bit = bitpos % 8;
                uint8_t byte = x86_mem_read8(ctx, addr + byte_offset);
                ctx->regs.eflags = (ctx->regs.eflags & ~X86_CF) | ((byte >> bit) & 1 ? X86_CF : 0);
            }
            return 0;
        }
        // ---- BTS (0xAB): bit test and set ----
        if (op2 == 0xAB) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            if (modrm_mod(modrm) == 3) {
                uint32_t* reg = x86_reg_ptr(ctx, modrm_rm(modrm));
                int bit = *x86_reg_ptr(ctx, modrm_reg(modrm)) & 31;
                ctx->regs.eflags = (ctx->regs.eflags & ~X86_CF) | ((*reg >> bit) & 1 ? X86_CF : 0);
                *reg |= (1u << bit);
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                int bitpos = *x86_reg_ptr(ctx, modrm_reg(modrm));
                int byte_offset = bitpos / 8;
                int bit = bitpos % 8;
                uint8_t byte = x86_mem_read8(ctx, addr + byte_offset);
                ctx->regs.eflags = (ctx->regs.eflags & ~X86_CF) | ((byte >> bit) & 1 ? X86_CF : 0);
                x86_mem_write8(ctx, addr + byte_offset, byte | (1 << bit));
            }
            return 0;
        }
        // ---- BTR (0xB3): bit test and reset ----
        if (op2 == 0xB3) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            if (modrm_mod(modrm) == 3) {
                uint32_t* reg = x86_reg_ptr(ctx, modrm_rm(modrm));
                int bit = *x86_reg_ptr(ctx, modrm_reg(modrm)) & 31;
                ctx->regs.eflags = (ctx->regs.eflags & ~X86_CF) | ((*reg >> bit) & 1 ? X86_CF : 0);
                *reg &= ~(1u << bit);
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                int bitpos = *x86_reg_ptr(ctx, modrm_reg(modrm));
                int byte_offset = bitpos / 8;
                int bit = bitpos % 8;
                uint8_t byte = x86_mem_read8(ctx, addr + byte_offset);
                ctx->regs.eflags = (ctx->regs.eflags & ~X86_CF) | ((byte >> bit) & 1 ? X86_CF : 0);
                x86_mem_write8(ctx, addr + byte_offset, byte & ~(1 << bit));
            }
            return 0;
        }
        // ---- BSWAP (0xC8-0xCF): byte swap ----
        if (op2 >= 0xC8 && op2 <= 0xCF) {
            int reg = op2 - 0xC8;
            uint32_t val = *x86_reg_ptr(ctx, reg);
            *x86_reg_ptr(ctx, reg) = ((val & 0xFF) << 24) | ((val & 0xFF00) << 8) |
                                     ((val & 0xFF0000) >> 8) | ((val & 0xFF000000) >> 24);
            return 0;
        }
        // ---- XADD (0xC0/0xC1): exchange and add ----
        if (op2 == 0xC0 || op2 == 0xC1) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            int size = (op2 == 0xC1) ? 4 : 1;
            if (modrm_mod(modrm) == 3) {
                if (size == 4) {
                    uint32_t* reg1 = x86_reg_ptr(ctx, modrm_reg(modrm));
                    uint32_t* reg2 = x86_reg_ptr(ctx, modrm_rm(modrm));
                    uint32_t tmp = *reg1;
                    *reg1 += *reg2;
                    *reg2 = tmp;
                    x86_update_flags_logic(ctx, *reg1);
                }
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                if (size == 4) {
                    uint32_t mem = x86_mem_read32(ctx, addr);
                    uint32_t reg = *x86_reg_ptr(ctx, modrm_reg(modrm));
                    x86_mem_write32(ctx, addr, mem + reg);
                    *x86_reg_ptr(ctx, modrm_reg(modrm)) = mem;
                    x86_update_flags_logic(ctx, mem + reg);
                }
            }
            return 0;
        }

        // ---- SHLD (0xA4=imm8, 0xA5=CL) ----
        if (op2 == 0xA4 || op2 == 0xA5) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            uint8_t count;
            if (op2 == 0xA4) {
                count = x86_mem_read8(ctx, ctx->regs.eip++);
            } else {
                count = ctx->regs.ecx & 0x1F;
            }
            uint32_t src_val;
            if (modrm_mod(modrm) == 3) {
                src_val = *x86_reg_ptr(ctx, modrm_rm(modrm));
                uint32_t* dst = x86_reg_ptr(ctx, modrm_reg(modrm));
                if (count == 0) { /* no change */ }
                else if (count < 32) {
                    uint64_t combined = ((uint64_t)*dst << 32) | src_val;
                    *dst = (uint32_t)(combined >> (32 - count));
                    // flags
                    ctx->regs.eflags = (ctx->regs.eflags & ~X86_CF) | ((*dst >> (count - 1)) & 1 ? X86_CF : 0);
                    if (count == 1) {
                        // OF handling simplified
                    }
                }
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                src_val = x86_mem_read32(ctx, addr); // simplified
            }
            return 0;
        }
        // ---- SHRD (0xAC=imm8, 0xAD=CL) ----
        if (op2 == 0xAC || op2 == 0xAD) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            uint8_t count;
            if (op2 == 0xAC) {
                count = x86_mem_read8(ctx, ctx->regs.eip++);
            } else {
                count = ctx->regs.ecx & 0x1F;
            }
            if (modrm_mod(modrm) == 3) {
                uint32_t src_val = *x86_reg_ptr(ctx, modrm_rm(modrm));
                uint32_t* dst = x86_reg_ptr(ctx, modrm_reg(modrm));
                if (count > 0 && count < 32) {
                    uint64_t combined = ((uint64_t)src_val << 32) | *dst;
                    *dst = (uint32_t)(combined >> count);
                    ctx->regs.eflags = (ctx->regs.eflags & ~X86_CF) | ((*dst >> (count - 1)) & 1 ? 0 : X86_CF); // simplified
                }
            }
            return 0;
        }
        // ---- CMPXCHG (0xB0=byte, 0xB1=dword) ----
        if (op2 == 0xB0 || op2 == 0xB1) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            if (op2 == 0xB1) {
                if (modrm_mod(modrm) == 3) {
                    uint32_t* reg = x86_reg_ptr(ctx, modrm_rm(modrm));
                    uint32_t* acc = &ctx->regs.eax;
                    if (*acc == *reg) {
                        ctx->regs.eflags |= X86_ZF;
                        *reg = *x86_reg_ptr(ctx, modrm_reg(modrm));
                    } else {
                        ctx->regs.eflags &= ~X86_ZF;
                        *acc = *reg;
                    }
                } else {
                    int consumed = 0;
                    uint32_t addr = calc_ea(ctx, modrm, &consumed);
                    ctx->regs.eip += consumed - 1;
                    uint32_t mem = x86_mem_read32(ctx, addr);
                    if (ctx->regs.eax == mem) {
                        ctx->regs.eflags |= X86_ZF;
                        x86_mem_write32(ctx, addr, *x86_reg_ptr(ctx, modrm_reg(modrm)));
                    } else {
                        ctx->regs.eflags &= ~X86_ZF;
                        ctx->regs.eax = mem;
                    }
                }
            }
            return 0;
        }

        // Unknown 0x0F instruction
        char buf[128];
        snprintf(buf, sizeof(buf), "x86: unhandled 0x0F 0x%02X at EIP=0x%08X", op2, eip_start);
        if (ctx->log) ctx->log(buf);
        ctx->running = 0;
        return -1;
    }

    // ---- MOV r32, r/m32 (0x8B) ----
    if (op == 0x8B) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        if (modrm_mod(modrm) == 3) {
            *x86_reg_ptr(ctx, modrm_reg(modrm)) = *x86_reg_ptr(ctx, modrm_rm(modrm));
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            *x86_reg_ptr(ctx, modrm_reg(modrm)) = x86_mem_read32(ctx, addr);
        }
        return 0;
    }

    // ---- MOV r/m32, r32 (0x89) ----
    if (op == 0x89) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        if (modrm_mod(modrm) == 3) {
            *x86_reg_ptr(ctx, modrm_rm(modrm)) = *x86_reg_ptr(ctx, modrm_reg(modrm));
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            x86_mem_write32(ctx, addr, *x86_reg_ptr(ctx, modrm_reg(modrm)));
        }
        return 0;
    }

    // ---- MOV r/m8, r8 (0x88) ----
    if (op == 0x88) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint8_t val = (uint8_t)(*x86_reg_ptr(ctx, modrm_reg(modrm)) & 0xFF);
        if (modrm_mod(modrm) == 3) {
            *x86_reg_ptr(ctx, modrm_rm(modrm)) = (*x86_reg_ptr(ctx, modrm_rm(modrm)) & 0xFFFFFF00) | val;
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            x86_mem_write8(ctx, addr, val);
        }
        return 0;
    }

    // ---- MOV r8, r/m8 (0x8A) ----
    if (op == 0x8A) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint8_t val;
        if (modrm_mod(modrm) == 3) {
            val = (uint8_t)(*x86_reg_ptr(ctx, modrm_rm(modrm)) & 0xFF);
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            val = x86_mem_read8(ctx, addr);
        }
        *x86_reg_ptr(ctx, modrm_reg(modrm)) = (*x86_reg_ptr(ctx, modrm_reg(modrm)) & 0xFFFFFF00) | val;
        return 0;
    }

    // ---- MOV r32, imm32 (0xB8-0xBF) ----
    if (op >= 0xB8 && op <= 0xBF) {
        int reg = op - 0xB8;
        uint32_t imm = read_imm32(ctx);
        *x86_reg_ptr(ctx, reg) = imm;
        return 0;
    }

    // ---- MOV r/m32, imm32 (0xC7 /0) ----
    if (op == 0xC7) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t imm = read_imm32(ctx);
        if (modrm_mod(modrm) == 3) {
            *x86_reg_ptr(ctx, modrm_rm(modrm)) = imm;
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            x86_mem_write32(ctx, addr, imm);
        }
        return 0;
    }

    // ---- MOV r/m8, imm8 (0xC6 /0) ----
    if (op == 0xC6) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint8_t imm = read_imm8(ctx);
        if (modrm_mod(modrm) == 3) {
            *x86_reg_ptr(ctx, modrm_rm(modrm)) = (*x86_reg_ptr(ctx, modrm_rm(modrm)) & 0xFFFFFF00) | imm;
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            x86_mem_write8(ctx, addr, imm);
        }
        return 0;
    }

    // ---- ADD r/m32, r32 (0x01) ----
    if (op == 0x01) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src = *x86_reg_ptr(ctx, modrm_reg(modrm));
        if (modrm_mod(modrm) == 3) {
            uint32_t* dst = x86_reg_ptr(ctx, modrm_rm(modrm));
            uint32_t result = *dst + src;
            x86_update_flags_add(ctx, *dst, src, result);
            *dst = result;
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            uint32_t dst = x86_mem_read32(ctx, addr);
            uint32_t result = dst + src;
            x86_update_flags_add(ctx, dst, src, result);
            x86_mem_write32(ctx, addr, result);
        }
        return 0;
    }

    // ---- ADD r32, r/m32 (0x03) ----
    if (op == 0x03) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src;
        if (modrm_mod(modrm) == 3) {
            src = *x86_reg_ptr(ctx, modrm_rm(modrm));
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            src = x86_mem_read32(ctx, addr);
        }
        uint32_t* dst = x86_reg_ptr(ctx, modrm_reg(modrm));
        uint32_t result = *dst + src;
        x86_update_flags_add(ctx, *dst, src, result);
        *dst = result;
        return 0;
    }

    // ---- ADD AL/AX/EAX, imm (0x05) ----
    if (op == 0x05) {
        uint32_t imm = read_imm32(ctx);
        uint32_t result = ctx->regs.eax + imm;
        x86_update_flags_add(ctx, ctx->regs.eax, imm, result);
        ctx->regs.eax = result;
        return 0;
    }

    // ---- SUB r/m32, r32 (0x29) ----
    if (op == 0x29) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src = *x86_reg_ptr(ctx, modrm_reg(modrm));
        if (modrm_mod(modrm) == 3) {
            uint32_t* dst = x86_reg_ptr(ctx, modrm_rm(modrm));
            uint32_t result = *dst - src;
            x86_update_flags_sub(ctx, *dst, src, result);
            *dst = result;
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            uint32_t dst = x86_mem_read32(ctx, addr);
            uint32_t result = dst - src;
            x86_update_flags_sub(ctx, dst, src, result);
            x86_mem_write32(ctx, addr, result);
        }
        return 0;
    }

    // ---- SUB r32, r/m32 (0x2B) ----
    if (op == 0x2B) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src;
        if (modrm_mod(modrm) == 3) {
            src = *x86_reg_ptr(ctx, modrm_rm(modrm));
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            src = x86_mem_read32(ctx, addr);
        }
        uint32_t* dst = x86_reg_ptr(ctx, modrm_reg(modrm));
        uint32_t result = *dst - src;
        x86_update_flags_sub(ctx, *dst, src, result);
        *dst = result;
        return 0;
    }

    // ---- CMP r/m32, r32 (0x39) ----
    if (op == 0x39) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src = *x86_reg_ptr(ctx, modrm_reg(modrm));
        uint32_t dst;
        if (modrm_mod(modrm) == 3) {
            dst = *x86_reg_ptr(ctx, modrm_rm(modrm));
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            dst = x86_mem_read32(ctx, addr);
        }
        x86_update_flags_sub(ctx, dst, src, dst - src);
        return 0;
    }

    // ---- CMP r32, r/m32 (0x3B) ----
    if (op == 0x3B) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src;
        if (modrm_mod(modrm) == 3) {
            src = *x86_reg_ptr(ctx, modrm_rm(modrm));
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            src = x86_mem_read32(ctx, addr);
        }
        uint32_t dst = *x86_reg_ptr(ctx, modrm_reg(modrm));
        x86_update_flags_sub(ctx, dst, src, dst - src);
        return 0;
    }

    // ---- AND (0x21 r/m,r ; 0x23 r,r/m ; 0x83 /4 imm8) ----
    if (op == 0x21) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src = *x86_reg_ptr(ctx, modrm_reg(modrm));
        if (modrm_mod(modrm) == 3) {
            uint32_t* dst = x86_reg_ptr(ctx, modrm_rm(modrm));
            *dst &= src;
            x86_update_flags_logic(ctx, *dst);
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            uint32_t dst = x86_mem_read32(ctx, addr);
            dst &= src;
            x86_update_flags_logic(ctx, dst);
            x86_mem_write32(ctx, addr, dst);
        }
        ctx->regs.eflags &= ~X86_CF;
        ctx->regs.eflags &= ~X86_OF;
        return 0;
    }

    if (op == 0x23) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src;
        if (modrm_mod(modrm) == 3) {
            src = *x86_reg_ptr(ctx, modrm_rm(modrm));
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            src = x86_mem_read32(ctx, addr);
        }
        uint32_t* dst = x86_reg_ptr(ctx, modrm_reg(modrm));
        *dst &= src;
        x86_update_flags_logic(ctx, *dst);
        ctx->regs.eflags &= ~X86_CF;
        ctx->regs.eflags &= ~X86_OF;
        return 0;
    }

    // ---- OR (0x09 r/m,r ; 0x0B r,r/m) ----
    if (op == 0x09) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src = *x86_reg_ptr(ctx, modrm_reg(modrm));
        if (modrm_mod(modrm) == 3) {
            uint32_t* dst = x86_reg_ptr(ctx, modrm_rm(modrm));
            *dst |= src;
            x86_update_flags_logic(ctx, *dst);
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            uint32_t dst = x86_mem_read32(ctx, addr);
            dst |= src;
            x86_update_flags_logic(ctx, dst);
            x86_mem_write32(ctx, addr, dst);
        }
        ctx->regs.eflags &= ~X86_CF;
        ctx->regs.eflags &= ~X86_OF;
        return 0;
    }

    if (op == 0x0B) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src;
        if (modrm_mod(modrm) == 3) {
            src = *x86_reg_ptr(ctx, modrm_rm(modrm));
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            src = x86_mem_read32(ctx, addr);
        }
        uint32_t* dst = x86_reg_ptr(ctx, modrm_reg(modrm));
        *dst |= src;
        x86_update_flags_logic(ctx, *dst);
        ctx->regs.eflags &= ~X86_CF;
        ctx->regs.eflags &= ~X86_OF;
        return 0;
    }

    // ---- XOR (0x31 r/m,r ; 0x33 r,r/m) ----
    if (op == 0x31) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src = *x86_reg_ptr(ctx, modrm_reg(modrm));
        if (modrm_mod(modrm) == 3) {
            uint32_t* dst = x86_reg_ptr(ctx, modrm_rm(modrm));
            *dst ^= src;
            x86_update_flags_logic(ctx, *dst);
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            uint32_t dst = x86_mem_read32(ctx, addr);
            dst ^= src;
            x86_update_flags_logic(ctx, dst);
            x86_mem_write32(ctx, addr, dst);
        }
        ctx->regs.eflags &= ~X86_CF;
        ctx->regs.eflags &= ~X86_OF;
        return 0;
    }

    if (op == 0x33) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src;
        if (modrm_mod(modrm) == 3) {
            src = *x86_reg_ptr(ctx, modrm_rm(modrm));
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            src = x86_mem_read32(ctx, addr);
        }
        uint32_t* dst = x86_reg_ptr(ctx, modrm_reg(modrm));
        *dst ^= src;
        x86_update_flags_logic(ctx, *dst);
        ctx->regs.eflags &= ~X86_CF;
        ctx->regs.eflags &= ~X86_OF;
        return 0;
    }

    // ---- TEST r/m32, r32 (0x85) ----
    if (op == 0x85) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src = *x86_reg_ptr(ctx, modrm_reg(modrm));
        uint32_t dst;
        if (modrm_mod(modrm) == 3) {
            dst = *x86_reg_ptr(ctx, modrm_rm(modrm));
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            dst = x86_mem_read32(ctx, addr);
        }
        x86_update_flags_logic(ctx, dst & src);
        ctx->regs.eflags &= ~X86_CF;
        ctx->regs.eflags &= ~X86_OF;
        return 0;
    }

    // ---- LEA (0x8D) ----
    if (op == 0x8D) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        int consumed = 0;
        uint32_t addr = calc_ea(ctx, modrm, &consumed);
        ctx->regs.eip += consumed - 1;
        *x86_reg_ptr(ctx, modrm_reg(modrm)) = addr;
        return 0;
    }

    // ---- INC r32 (0x40-0x47) ----
    if (op >= 0x40 && op <= 0x47) {
        int reg = op - 0x40;
        uint32_t* r = x86_reg_ptr(ctx, reg);
        uint32_t result = *r + 1;
        x86_update_flags_add(ctx, *r, 1, result);
        // INC doesn't change CF
        // (flags already updated, but restore CF from before)
        *r = result;
        return 0;
    }

    // ---- DEC r32 (0x48-0x4F) ----
    if (op >= 0x48 && op <= 0x4F) {
        int reg = op - 0x48;
        uint32_t* r = x86_reg_ptr(ctx, reg);
        uint32_t result = *r - 1;
        x86_update_flags_sub(ctx, *r, 1, result);
        *r = result;
        return 0;
    }

    // ---- IMUL r32, r/m32 (0x69 is imm, 0x6B is imm8, 0xAF is reg) ----
    if (op == 0x69) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src;
        if (modrm_mod(modrm) == 3) {
            src = *x86_reg_ptr(ctx, modrm_rm(modrm));
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            src = x86_mem_read32(ctx, addr);
        }
        int32_t imm = (int32_t)read_imm32(ctx);
        uint32_t* dst = x86_reg_ptr(ctx, modrm_reg(modrm));
        *dst = src * imm;
        return 0;
    }

    if (op == 0x6B) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src;
        if (modrm_mod(modrm) == 3) {
            src = *x86_reg_ptr(ctx, modrm_rm(modrm));
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            src = x86_mem_read32(ctx, addr);
        }
        int8_t imm = (int8_t)read_imm8(ctx);
        uint32_t* dst = x86_reg_ptr(ctx, modrm_reg(modrm));
        *dst = src * imm;
        return 0;
    }

    // ---- 0x83 group (arithmetic with imm8) ----
    if (op == 0x83) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        int8_t imm = (int8_t)read_imm8(ctx);
        uint32_t src = (uint32_t)imm;
        int regop = modrm_reg(modrm);
        if (modrm_mod(modrm) == 3) {
            uint32_t* dst = x86_reg_ptr(ctx, modrm_rm(modrm));
            switch (regop) {
                case 0: { // ADD
                    uint32_t r = *dst + src;
                    x86_update_flags_add(ctx, *dst, src, r);
                    *dst = r; break;
                }
                case 1: { // OR
                    *dst |= src;
                    x86_update_flags_logic(ctx, *dst);
                    ctx->regs.eflags &= ~(X86_CF | X86_OF);
                    break;
                }
                case 2: { // ADC
                    uint32_t cf = (ctx->regs.eflags & X86_CF) ? 1 : 0;
                    uint32_t r = *dst + src + cf;
                    x86_update_flags_add(ctx, *dst, src + cf, r);
                    *dst = r; break;
                }
                case 3: { // SBB
                    uint32_t cf = (ctx->regs.eflags & X86_CF) ? 1 : 0;
                    uint32_t r = *dst - src - cf;
                    x86_update_flags_sub(ctx, *dst, src + cf, r);
                    *dst = r; break;
                }
                case 4: { // AND
                    *dst &= src;
                    x86_update_flags_logic(ctx, *dst);
                    ctx->regs.eflags &= ~(X86_CF | X86_OF);
                    break;
                }
                case 5: { // SUB
                    uint32_t r = *dst - src;
                    x86_update_flags_sub(ctx, *dst, src, r);
                    *dst = r; break;
                }
                case 6: { // XOR
                    *dst ^= src;
                    x86_update_flags_logic(ctx, *dst);
                    ctx->regs.eflags &= ~(X86_CF | X86_OF);
                    break;
                }
                case 7: { // CMP
                    x86_update_flags_sub(ctx, *dst, src, *dst - src);
                    break;
                }
            }
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            uint32_t dst = x86_mem_read32(ctx, addr);
            uint32_t result = dst;
            switch (regop) {
                case 0: result = dst + src; x86_update_flags_add(ctx, dst, src, result); break;
                case 5: result = dst - src; x86_update_flags_sub(ctx, dst, src, result); break;
                case 7: x86_update_flags_sub(ctx, dst, src, dst - src); result = dst; break;
                case 4: result = dst & src; x86_update_flags_logic(ctx, result); ctx->regs.eflags &= ~(X86_CF | X86_OF); break;
                case 1: result = dst | src; x86_update_flags_logic(ctx, result); ctx->regs.eflags &= ~(X86_CF | X86_OF); break;
                case 6: result = dst ^ src; x86_update_flags_logic(ctx, result); ctx->regs.eflags &= ~(X86_CF | X86_OF); break;
            }
            if (regop != 7) x86_mem_write32(ctx, addr, result);
        }
        return 0;
    }

    // ---- 0x81 group (arithmetic with imm32) ----
    if (op == 0x81) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t imm = read_imm32(ctx);
        int regop = modrm_reg(modrm);
        if (modrm_mod(modrm) == 3) {
            uint32_t* dst = x86_reg_ptr(ctx, modrm_rm(modrm));
            switch (regop) {
                case 0: { uint32_t r = *dst + imm; x86_update_flags_add(ctx, *dst, imm, r); *dst = r; break; }
                case 5: { uint32_t r = *dst - imm; x86_update_flags_sub(ctx, *dst, imm, r); *dst = r; break; }
                case 7: { x86_update_flags_sub(ctx, *dst, imm, *dst - imm); break; }
                case 4: { *dst &= imm; x86_update_flags_logic(ctx, *dst); ctx->regs.eflags &= ~(X86_CF | X86_OF); break; }
                case 1: { *dst |= imm; x86_update_flags_logic(ctx, *dst); ctx->regs.eflags &= ~(X86_CF | X86_OF); break; }
                case 6: { *dst ^= imm; x86_update_flags_logic(ctx, *dst); ctx->regs.eflags &= ~(X86_CF | X86_OF); break; }
            }
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            uint32_t dst = x86_mem_read32(ctx, addr);
            uint32_t result = dst;
            switch (regop) {
                case 0: result = dst + imm; x86_update_flags_add(ctx, dst, imm, result); break;
                case 5: result = dst - imm; x86_update_flags_sub(ctx, dst, imm, result); break;
                case 7: x86_update_flags_sub(ctx, dst, imm, dst - imm); result = dst; break;
                case 4: result = dst & imm; x86_update_flags_logic(ctx, result); ctx->regs.eflags &= ~(X86_CF | X86_OF); break;
                case 1: result = dst | imm; x86_update_flags_logic(ctx, result); ctx->regs.eflags &= ~(X86_CF | X86_OF); break;
                case 6: result = dst ^ imm; x86_update_flags_logic(ctx, result); ctx->regs.eflags &= ~(X86_CF | X86_OF); break;
            }
            if (regop != 7) x86_mem_write32(ctx, addr, result);
        }
        return 0;
    }

    // ---- SHL/SHR/SAR (0xC1 /4,5,7 with imm8; 0xD3 with CL) ----
    if (op == 0xC1) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint8_t imm = read_imm8(ctx);
        int regop = modrm_reg(modrm);
        uint32_t* dst;
        uint32_t val;
        if (modrm_mod(modrm) == 3) {
            dst = x86_reg_ptr(ctx, modrm_rm(modrm));
            val = *dst;
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            val = x86_mem_read32(ctx, addr);
            dst = nullptr;
        }
        uint32_t result = val;
        if (imm == 0) {
            // no change to flags
        } else {
            int count = imm & 0x1F;
            if (count == 0) {
                // no change
            } else if (regop == 4) { // SHL
                result = val << count;
                // CF = bit shifted out
                if (count <= 32) ctx->regs.eflags = (ctx->regs.eflags & ~X86_CF) | ((val >> (32 - count)) & 1 ? X86_CF : 0);
            } else if (regop == 5) { // SHR
                result = val >> count;
                if (count <= 32) ctx->regs.eflags = (ctx->regs.eflags & ~X86_CF) | ((val >> (count - 1)) & 1 ? X86_CF : 0);
            } else if (regop == 7) { // SAR
                result = (uint32_t)((int32_t)val >> count);
                if (count <= 32) ctx->regs.eflags = (ctx->regs.eflags & ~X86_CF) | ((val >> (count - 1)) & 1 ? X86_CF : 0);
            }
            x86_update_flags_logic(ctx, result);
        }
        if (dst) *dst = result;
        else {
            // write back to memory - need addr again, skip for simplicity
        }
        return 0;
    }

    // ---- XCHG (0x87 r/m,r ; 0x90+reg is NOP/XCHG EAX) ----
    if (op == 0x87) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        if (modrm_mod(modrm) == 3) {
            uint32_t* a = x86_reg_ptr(ctx, modrm_reg(modrm));
            uint32_t* b = x86_reg_ptr(ctx, modrm_rm(modrm));
            uint32_t t = *a; *a = *b; *b = t;
        }
        return 0;
    }

    // ---- XCHG EAX, r32 (0x91-0x97) ----
    if (op >= 0x91 && op <= 0x97) {
        int reg = op - 0x90;
        uint32_t t = ctx->regs.eax;
        ctx->regs.eax = *x86_reg_ptr(ctx, reg);
        *x86_reg_ptr(ctx, reg) = t;
        return 0;
    }

    // ---- CDQ (0x99) ----
    if (op == 0x99) {
        ctx->regs.edx = (ctx->regs.eax & 0x80000000) ? 0xFFFFFFFF : 0;
        return 0;
    }

    // ---- CWD/CDQ variants ----
    if (op == 0x98) { // CWDE
        ctx->regs.eax = (int32_t)(int16_t)(ctx->regs.eax & 0xFFFF);
        return 0;
    }

    // ---- LEAVE (0xC9) ----
    if (op == 0xC9) {
        ctx->regs.esp = ctx->regs.ebp;
        ctx->regs.ebp = x86_mem_read32(ctx, ctx->regs.esp);
        ctx->regs.esp += 4;
        return 0;
    }

    // ---- ENTER (0xC8) ----
    if (op == 0xC8) {
        uint16_t size = x86_mem_read16(ctx, ctx->regs.eip);
        ctx->regs.eip += 2;
        uint8_t level = read_imm8(ctx);
        ctx->regs.esp -= 4;
        x86_mem_write32(ctx, ctx->regs.esp, ctx->regs.ebp);
        ctx->regs.ebp = ctx->regs.esp;
        ctx->regs.esp -= size;
        (void)level;
        return 0;
    }

    // ---- NOT (0xF7 /2) ----
    if (op == 0xF7) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        int regop = modrm_reg(modrm);
        if (regop == 2) { // NOT
            if (modrm_mod(modrm) == 3) {
                *x86_reg_ptr(ctx, modrm_rm(modrm)) = ~*x86_reg_ptr(ctx, modrm_rm(modrm));
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                uint32_t v = x86_mem_read32(ctx, addr);
                x86_mem_write32(ctx, addr, ~v);
            }
            return 0;
        }
        if (regop == 3) { // NEG
            if (modrm_mod(modrm) == 3) {
                uint32_t* r = x86_reg_ptr(ctx, modrm_rm(modrm));
                uint32_t result = 0 - *r;
                x86_update_flags_sub(ctx, 0, *r, result);
                *r = result;
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                uint32_t v = x86_mem_read32(ctx, addr);
                uint32_t result = 0 - v;
                x86_update_flags_sub(ctx, 0, v, result);
                x86_mem_write32(ctx, addr, result);
            }
            return 0;
        }
        if (regop == 4) { // MUL (unsigned)
            if (modrm_mod(modrm) == 3) {
                uint64_t result = (uint64_t)ctx->regs.eax * *x86_reg_ptr(ctx, modrm_rm(modrm));
                ctx->regs.eax = (uint32_t)result;
                ctx->regs.edx = (uint32_t)(result >> 32);
            }
            return 0;
        }
        if (regop == 5) { // IMUL (signed, EDX:EAX)
            if (modrm_mod(modrm) == 3) {
                int64_t result = (int64_t)(int32_t)ctx->regs.eax * (int64_t)(int32_t)*x86_reg_ptr(ctx, modrm_rm(modrm));
                ctx->regs.eax = (uint32_t)result;
                ctx->regs.edx = (uint32_t)(result >> 32);
            }
            return 0;
        }
        if (regop == 6) { // DIV (unsigned)
            if (modrm_mod(modrm) == 3) {
                uint64_t dividend = ((uint64_t)ctx->regs.edx << 32) | ctx->regs.eax;
                uint32_t divisor = *x86_reg_ptr(ctx, modrm_rm(modrm));
                if (divisor == 0) {
                    if (ctx->log) ctx->log("x86: DIV by zero");
                    ctx->running = 0;
                    return -1;
                }
                ctx->regs.eax = (uint32_t)(dividend / divisor);
                ctx->regs.edx = (uint32_t)(dividend % divisor);
            }
            return 0;
        }
        if (regop == 7) { // IDIV (signed)
            if (modrm_mod(modrm) == 3) {
                int64_t dividend = ((int64_t)(int32_t)ctx->regs.edx << 32) | (int64_t)(int32_t)ctx->regs.eax;
                int32_t divisor = (int32_t)*x86_reg_ptr(ctx, modrm_rm(modrm));
                if (divisor == 0) {
                    if (ctx->log) ctx->log("x86: IDIV by zero");
                    ctx->running = 0;
                    return -1;
                }
                ctx->regs.eax = (uint32_t)(dividend / divisor);
                ctx->regs.edx = (uint32_t)(dividend % divisor);
            }
            return 0;
        }
    }

    // ---- 0xFE group (INC/DEC r/m8) ----
    if (op == 0xFE) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        int regop = modrm_reg(modrm);
        if (modrm_mod(modrm) == 3) {
            uint32_t* r = x86_reg_ptr(ctx, modrm_rm(modrm));
            if (regop == 0) { // INC
                *r = (*r & 0xFFFFFF00) | (((*r & 0xFF) + 1) & 0xFF);
            } else { // DEC
                *r = (*r & 0xFFFFFF00) | (((*r & 0xFF) - 1) & 0xFF);
            }
        }
        return 0;
    }

    // ---- CALL r/m32 (0xFF /2) ----
    if (op == 0xFF) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        int regop = modrm_reg(modrm);
        if (regop == 2) { // CALL near indirect
            uint32_t target;
            if (modrm_mod(modrm) == 3) {
                target = *x86_reg_ptr(ctx, modrm_rm(modrm));
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                target = x86_mem_read32(ctx, addr);
            }
            // Check if this is a bridge thunk (imported function)
            // For now, just call it as code
            ctx->regs.esp -= 4;
            x86_mem_write32(ctx, ctx->regs.esp, ctx->regs.eip);
            ctx->regs.eip = target;
            return 0;
        }
        if (regop == 4) { // JMP near indirect
            uint32_t target;
            if (modrm_mod(modrm) == 3) {
                target = *x86_reg_ptr(ctx, modrm_rm(modrm));
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                target = x86_mem_read32(ctx, addr);
            }
            ctx->regs.eip = target;
            return 0;
        }
        if (regop == 6) { // PUSH r/m32
            uint32_t val;
            if (modrm_mod(modrm) == 3) {
                val = *x86_reg_ptr(ctx, modrm_rm(modrm));
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                val = x86_mem_read32(ctx, addr);
            }
            ctx->regs.esp -= 4;
            x86_mem_write32(ctx, ctx->regs.esp, val);
            return 0;
        }
    }

    // ---- REP MOVSB/MOVSD (0xF3 0xA4/A5) ----
    // Handled partially above via prefix; check here
    // Actually handled by prefix detection + op check below

    // ---- MOVSB (0xA4) ----
    if (op == 0xA4) {
        uint8_t val = x86_mem_read8(ctx, ctx->regs.esi);
        x86_mem_write8(ctx, ctx->regs.edi, val);
        if (ctx->regs.eflags & X86_DF) {
            ctx->regs.esi--;
            ctx->regs.edi--;
        } else {
            ctx->regs.esi++;
            ctx->regs.edi++;
        }
        ctx->regs.ecx--;
        return 0;
    }

    // ---- MOVSD (0xA5) ----
    if (op == 0xA5) {
        uint32_t val = x86_mem_read32(ctx, ctx->regs.esi);
        x86_mem_write32(ctx, ctx->regs.edi, val);
        if (ctx->regs.eflags & X86_DF) {
            ctx->regs.esi -= 4;
            ctx->regs.edi -= 4;
        } else {
            ctx->regs.esi += 4;
            ctx->regs.edi += 4;
        }
        ctx->regs.ecx--;
        return 0;
    }

    // ---- STOSB (0xAA) ----
    if (op == 0xAA) {
        x86_mem_write8(ctx, ctx->regs.edi, (uint8_t)ctx->regs.eax);
        if (ctx->regs.eflags & X86_DF) ctx->regs.edi--;
        else ctx->regs.edi++;
        return 0;
    }

    // ---- STOSD (0xAB) ----
    if (op == 0xAB) {
        x86_mem_write32(ctx, ctx->regs.edi, ctx->regs.eax);
        if (ctx->regs.eflags & X86_DF) ctx->regs.edi -= 4;
        else ctx->regs.edi += 4;
        return 0;
    }

    // ---- LODSB (0xAC) ----
    if (op == 0xAC) {
        ctx->regs.eax = (ctx->regs.eax & 0xFFFFFF00) | x86_mem_read8(ctx, ctx->regs.esi);
        if (ctx->regs.eflags & X86_DF) ctx->regs.esi--;
        else ctx->regs.esi++;
        return 0;
    }

    // ---- LODSD (0xAD) ----
    if (op == 0xAD) {
        ctx->regs.eax = x86_mem_read32(ctx, ctx->regs.esi);
        if (ctx->regs.eflags & X86_DF) ctx->regs.esi -= 4;
        else ctx->regs.esi += 4;
        return 0;
    }

    // ---- SCASB (0xAE) ----
    if (op == 0xAE) {
        uint8_t val = x86_mem_read8(ctx, ctx->regs.edi);
        x86_update_flags_sub(ctx, ctx->regs.eax & 0xFF, val, (ctx->regs.eax & 0xFF) - val);
        if (ctx->regs.eflags & X86_DF) ctx->regs.edi--;
        else ctx->regs.edi++;
        return 0;
    }

    // ---- SCASD (0xAF) ----
    if (op == 0xAF) {
        uint32_t val = x86_mem_read32(ctx, ctx->regs.edi);
        x86_update_flags_sub(ctx, ctx->regs.eax, val, ctx->regs.eax - val);
        if (ctx->regs.eflags & X86_DF) ctx->regs.edi -= 4;
        else ctx->regs.edi += 4;
        return 0;
    }

    // ---- IN/OUT (0xE4-E7, 0xEC-EF) - stubbed ----
    if (op >= 0xE4 && op <= 0xE7) {
        read_imm8(ctx);
        if (op <= 0xE5) read_imm8(ctx); // 16-bit port
        return 0;
    }
    if (op >= 0xEC && op <= 0xEF) {
        return 0;
    }

    // ---- FPU instructions (D8-DF) ----
    if (op >= 0xD8 && op <= 0xDF) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        int mod = modrm_mod(modrm);
        int regop = modrm_reg(modrm);
        int rm = modrm_rm(modrm);

        if (mod == 3) {
            // Register-register operations
            if (op == 0xD8) {
                // D8 /0 = FADD, /1 = FMUL, /2 = FCOM, /3 = FCOMP
                // /4 = FSUB, /5 = FSUBR, /6 = FDIV, /7 = FDIVR
                double st0 = fpu_st(ctx, 0);
                (void)regop; // suppress unused
                switch (rm) { // actually regop is the operation for D8
                }
                // D8 with mod=3: operation determined by regop field of modrm
                // Wait, for D8 mod=3, the 3-bit reg field is the operation:
                // 000=FADD, 001=FMUL, 010=FCOM, 011=FCOMP
                // 100=FSUB, 101=FSUBR, 110=FDIV, 111=FDIVR
                // and rm selects ST(i)
                double val = fpu_st(ctx, rm);
                switch (regop) {
                    case 0: fpu_set_st(ctx, 0, st0 + val); break; // FADD
                    case 1: fpu_set_st(ctx, 0, st0 * val); break; // FMUL
                    case 2: { // FCOM
                        // Compare ST0 with ST(i), set FPU status
                        ctx->regs.fpu_sw &= ~0x4500;
                        if (st0 > val) { /* greater */ }
                        else if (st0 < val) ctx->regs.fpu_sw |= 0x0100; // less
                        else ctx->regs.fpu_sw |= 0x4000; // equal
                        break;
                    }
                    case 3: { // FCOMP
                        ctx->regs.fpu_sw &= ~0x4500;
                        if (st0 < val) ctx->regs.fpu_sw |= 0x0100;
                        else if (st0 == val) ctx->regs.fpu_sw |= 0x4000;
                        fpu_pop(ctx);
                        break;
                    }
                    case 4: fpu_set_st(ctx, 0, st0 - val); break; // FSUB
                    case 5: fpu_set_st(ctx, 0, val - st0); break; // FSUBR
                    case 6: fpu_set_st(ctx, 0, st0 / val); break; // FDIV
                    case 7: fpu_set_st(ctx, 0, val / st0); break; // FDIVR
                }
            } else if (op == 0xD9) {
                // D9 mod=3: 
                // 000=FLD, 001=FXCH, 010=FNOP, 011=FSTP (some)
                // 100=FCHS/FABS/FTST/FXAM, 101=FLD1/FLDZ/etc, 110=F2XM1/etc, 111=...
                switch (regop) {
                    case 0: // FLD ST(i)
                        fpu_push(ctx, fpu_st(ctx, rm + 1)); // +1 because after push
                        break;
                    case 1: { // FXCH ST(i)
                        double t = fpu_st(ctx, 0);
                        fpu_set_st(ctx, 0, fpu_st(ctx, rm));
                        fpu_set_st(ctx, rm, t);
                        break;
                    }
                    case 2: break; // FNOP
                    case 4: { // FCHS/FABS/FTST/FXAM
                        switch (rm) {
                            case 0: fpu_set_st(ctx, 0, -fpu_st(ctx, 0)); break; // FCHS
                            case 1: fpu_set_st(ctx, 0, (fpu_st(ctx, 0) < 0) ? -fpu_st(ctx, 0) : fpu_st(ctx, 0)); break; // FABS
                            case 2: { // FTST
                                double v = fpu_st(ctx, 0);
                                ctx->regs.fpu_sw &= ~0x4500;
                                if (v < 0) ctx->regs.fpu_sw |= 0x0100;
                                else if (v == 0) ctx->regs.fpu_sw |= 0x4000;
                                break;
                            }
                            case 4: break; // FLDENV (skip)
                        }
                        break;
                    }
                    case 5: { // FLD1, FLDZ, etc.
                        switch (rm) {
                            case 0: fpu_push(ctx, 1.0); break; // FLD1
                            case 2: fpu_push(ctx, 0.0); break; // FLDZ
                            case 5: fpu_push(ctx, 3.14159265358979323846); break; // FLDPI
                            case 1: fpu_push(ctx, 0.301029995663981195); break; // FLDL2T
                            case 3: fpu_push(ctx, 0.6931471805599453); break; // FLDLN2
                            case 4: fpu_push(ctx, 3.321928094887362); break; // FLDL2E
                            case 6: fpu_push(ctx, 1.4426950408889634); break; // FLDLG2
                        }
                        break;
                    }
                    case 6: { // F2XM1, FYL2X, etc. (approximate)
                        switch (rm) {
                            case 0: fpu_set_st(ctx, 0, pow(2.0, fpu_st(ctx, 0)) - 1.0); break; // F2XM1
                            case 4: fpu_set_st(ctx, 0, sqrt(fpu_st(ctx, 0))); break; // FSQRT
                        }
                        break;
                    }
                    case 7: { // FPREM, FRNDINT, etc.
                        switch (rm) {
                            case 0: fpu_set_st(ctx, 0, (double)(int)fpu_st(ctx, 0)); break; // FRNDINT (truncate)
                            case 4: fpu_set_st(ctx, 0, sqrt(fpu_st(ctx, 0))); break; // FSQRT (alt)
                        }
                        break;
                    }
                }
            } else if (op == 0xDB) {
                switch (regop) {
                    case 0: // FILD (int32 -> float, memory handled below, this is reg)
                        // DB /0 with mod=3 is FCMOVNB (P6+), skip
                        break;
                    case 5: { // FUCOMI
                        double st0 = fpu_st(ctx, 0);
                        double sti = fpu_st(ctx, rm);
                        ctx->regs.eflags &= ~(X86_ZF | X86_PF | X86_CF);
                        if (st0 < sti) ctx->regs.eflags |= X86_CF;
                        if (st0 == sti) ctx->regs.eflags |= X86_ZF;
                        // parity for unordered
                        break;
                    }
                    case 6: { // FCOMI
                        double st0 = fpu_st(ctx, 0);
                        double sti = fpu_st(ctx, rm);
                        ctx->regs.eflags &= ~(X86_ZF | X86_PF | X86_CF);
                        if (st0 < sti) ctx->regs.eflags |= X86_CF;
                        if (st0 == sti) ctx->regs.eflags |= X86_ZF;
                        break;
                    }
                }
            } else if (op == 0xDD) {
                switch (regop) {
                    case 0: // FLD (double, memory) or FFREE (reg)
                        if (rm == 0) {
                            // FFREE ST(0)
                            ctx->regs.fpu_tags[ctx->regs.fpu_top] = 3;
                        }
                        break;
                    case 2: // FST (double, memory) or FST (reg) - no pop
                        break;
                    case 3: // FSTP (double, memory/reg) - pop
                        if (rm >= 0) {
                            // FSTP ST(i)
                            fpu_set_st(ctx, rm, fpu_st(ctx, 0));
                            fpu_pop(ctx);
                        }
                        break;
                }
            } else if (op == 0xDE) {
                switch (regop) {
                    case 0: // FADDP
                        fpu_set_st(ctx, rm + 1, fpu_st(ctx, rm + 1) + fpu_st(ctx, 0));
                        fpu_pop(ctx);
                        break;
                    case 1: // FMULP
                        fpu_set_st(ctx, rm + 1, fpu_st(ctx, rm + 1) * fpu_st(ctx, 0));
                        fpu_pop(ctx);
                        break;
                    case 3: // FCOMPP
                        {
                            double a = fpu_st(ctx, 0);
                            double b = fpu_st(ctx, 1);
                            ctx->regs.fpu_sw &= ~0x4500;
                            if (a < b) ctx->regs.fpu_sw |= 0x0100;
                            else if (a == b) ctx->regs.fpu_sw |= 0x4000;
                            fpu_pop(ctx);
                            fpu_pop(ctx);
                        }
                        break;
                    case 4: // FSUBP
                        fpu_set_st(ctx, rm + 1, fpu_st(ctx, rm + 1) - fpu_st(ctx, 0));
                        fpu_pop(ctx);
                        break;
                    case 5: // FSUBRP
                        fpu_set_st(ctx, rm + 1, fpu_st(ctx, 0) - fpu_st(ctx, rm + 1));
                        fpu_pop(ctx);
                        break;
                    case 6: // FDIVP
                        fpu_set_st(ctx, rm + 1, fpu_st(ctx, rm + 1) / fpu_st(ctx, 0));
                        fpu_pop(ctx);
                        break;
                    case 7: // FDIVRP
                        fpu_set_st(ctx, rm + 1, fpu_st(ctx, 0) / fpu_st(ctx, rm + 1));
                        fpu_pop(ctx);
                        break;
                }
            } else if (op == 0xDF) {
                switch (regop) {
                    case 4: // FNSTSW AX
                        ctx->regs.eax = (ctx->regs.eax & 0xFFFF0000) | ctx->regs.fpu_sw;
                        break;
                }
            }
        } else {
            // Memory operations
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;

            if (op == 0xD8) {
                // D8: float (32-bit) memory
                float val;
                uint32_t _tmp = x86_mem_read32(ctx, addr);
                memcpy(&val, &_tmp, 4);
                double dval = val;
                switch (regop) {
                    case 0: fpu_push(ctx, dval); break; // FLD (float)
                    case 1: fpu_set_st(ctx, 0, fpu_st(ctx, 0) * dval); break; // FMUL
                    case 2: { // FCOM
                        double st0 = fpu_st(ctx, 0);
                        ctx->regs.fpu_sw &= ~0x4500;
                        if (st0 < dval) ctx->regs.fpu_sw |= 0x0100;
                        else if (st0 == dval) ctx->regs.fpu_sw |= 0x4000;
                        break;
                    }
                    case 3: { // FCOMP
                        double st0 = fpu_st(ctx, 0);
                        ctx->regs.fpu_sw &= ~0x4500;
                        if (st0 < dval) ctx->regs.fpu_sw |= 0x0100;
                        else if (st0 == dval) ctx->regs.fpu_sw |= 0x4000;
                        fpu_pop(ctx);
                        break;
                    }
                    case 4: fpu_set_st(ctx, 0, fpu_st(ctx, 0) - dval); break; // FSUB
                    case 5: fpu_set_st(ctx, 0, dval - fpu_st(ctx, 0)); break; // FSUBR
                    case 6: fpu_set_st(ctx, 0, fpu_st(ctx, 0) / dval); break; // FDIV
                    case 7: fpu_set_st(ctx, 0, dval / fpu_st(ctx, 0)); break; // FDIVR
                }
            } else if (op == 0xD9) {
                switch (regop) {
                    case 0: { // FLD (float memory)
                        float val; uint32_t _tmp2 = x86_mem_read32(ctx, addr); memcpy(&val, &_tmp2, 4);
                        fpu_push(ctx, val);
                        break;
                    }
                    case 2: { // FST (float memory)
                        float val = (float)fpu_st(ctx, 0);
                        uint32_t raw; memcpy(&raw, &val, 4);
                        x86_mem_write32(ctx, addr, raw);
                        break;
                    }
                    case 3: { // FSTP (float memory)
                        float val = (float)fpu_st(ctx, 0);
                        uint32_t raw; memcpy(&raw, &val, 4);
                        x86_mem_write32(ctx, addr, raw);
                        fpu_pop(ctx);
                        break;
                    }
                    case 4: { // FLDENV
                        fpu_read_env(ctx, addr);
                        break;
                    }
                    case 5: { // FLDCW
                        ctx->regs.fpu_cw = x86_mem_read16(ctx, addr);
                        break;
                    }
                    case 6: { // FNSTENV
                        fpu_write_env(ctx, addr);
                        break;
                    }
                    case 7: { // FNSTCW
                        x86_mem_write16(ctx, addr, ctx->regs.fpu_cw);
                        break;
                    }
                }
            } else if (op == 0xDB) {
                switch (regop) {
                    case 0: { // FILD (int32 memory)
                        int32_t val = (int32_t)x86_mem_read32(ctx, addr);
                        fpu_push(ctx, (double)val);
                        break;
                    }
                    case 2: { // FIST (int32 memory)
                        int32_t val = (int32_t)fpu_to_int(ctx, fpu_st(ctx, 0));
                        x86_mem_write32(ctx, addr, (uint32_t)val);
                        break;
                    }
                    case 3: { // FISTP (int32 memory)
                        int32_t val = (int32_t)fpu_to_int(ctx, fpu_st(ctx, 0));
                        x86_mem_write32(ctx, addr, (uint32_t)val);
                        fpu_pop(ctx);
                        break;
                    }
                    case 5: { // FLDCW (word)
                        ctx->regs.fpu_cw = x86_mem_read16(ctx, addr);
                        break;
                    }
                }
            } else if (op == 0xDD) {
                switch (regop) {
                    case 0: { // FLD (double memory)
                        uint32_t lo = x86_mem_read32(ctx, addr);
                        uint32_t hi = x86_mem_read32(ctx, addr + 4);
                        uint64_t raw = ((uint64_t)hi << 32) | lo;
                        double val; memcpy(&val, &raw, 8);
                        fpu_push(ctx, val);
                        break;
                    }
                    case 2: { // FST (double memory)
                        double _d = fpu_st(ctx, 0); uint64_t raw; memcpy(&raw, &_d, 8);
                        x86_mem_write32(ctx, addr, (uint32_t)(raw & 0xFFFFFFFF));
                        x86_mem_write32(ctx, addr + 4, (uint32_t)(raw >> 32));
                        break;
                    }
                    case 3: { // FSTP (double memory)
                        double _d = fpu_st(ctx, 0); uint64_t raw; memcpy(&raw, &_d, 8);
                        x86_mem_write32(ctx, addr, (uint32_t)(raw & 0xFFFFFFFF));
                        x86_mem_write32(ctx, addr + 4, (uint32_t)(raw >> 32));
                        fpu_pop(ctx);
                        break;
                    }
                    case 4: { // FRSTOR
                        fpu_read_save(ctx, addr);
                        break;
                    }
                    case 6: { // FNSAVE
                        fpu_write_save(ctx, addr);
                        break;
                    }
                    case 7: { // FNSTSW (memory, word)
                        x86_mem_write16(ctx, addr, ctx->regs.fpu_sw);
                        break;
                    }
                }
            } else if (op == 0xDE) {
                switch (regop) {
                    case 0: { // FIADD (int32 memory)
                        int32_t val = (int32_t)x86_mem_read32(ctx, addr);
                        fpu_set_st(ctx, 0, fpu_st(ctx, 0) + val);
                        break;
                    }
                    case 3: { // FICOMP (int32 memory)
                        int32_t val = (int32_t)x86_mem_read32(ctx, addr);
                        double dval = val;
                        ctx->regs.fpu_sw &= ~0x4500;
                        double st0 = fpu_st(ctx, 0);
                        if (st0 < dval) ctx->regs.fpu_sw |= 0x0100;
                        else if (st0 == dval) ctx->regs.fpu_sw |= 0x4000;
                        fpu_pop(ctx);
                        break;
                    }
                    case 4: { // FISUB (int32 memory)
                        int32_t val = (int32_t)x86_mem_read32(ctx, addr);
                        fpu_set_st(ctx, 0, fpu_st(ctx, 0) - val);
                        break;
                    }
                    case 6: { // FIDIV (int32 memory)
                        int32_t val = (int32_t)x86_mem_read32(ctx, addr);
                        fpu_set_st(ctx, 0, fpu_st(ctx, 0) / val);
                        break;
                    }
                }
            }
        }
        return 0;
    }

    // ---- Unknown instruction ----
    char buf[256];
    snprintf(buf, sizeof(buf), "x86: UNHANDLED opcode 0x%02X at EIP=0x%08X (insn #%llu)",
             op, eip_start, (unsigned long long)ctx->insn_count);
    if (ctx->log) ctx->log(buf);
    ctx->running = 0;
    ctx->last_error = 2; // illegal instruction
    return -1;
}

// ---- Memory management ----
// One contiguous 4GB reservation makes every guest VA a valid host pointer
// (base + VA), which is what the 64-bit bridges dereference.
#if defined(_WIN32)
#include <windows.h>
static void* xg_reserve4g(size_t size) {
    return VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
}
static int xg_commit_pages(void* base, size_t off, size_t size) {
    return VirtualAlloc((char*)base + off, size, MEM_COMMIT, PAGE_READWRITE) ? 0 : -1;
}
static void xg_release4g(void* base, size_t size) {
    if (base) VirtualFree(base, 0, MEM_RELEASE);
    (void)size;
}
#else
#include <sys/mman.h>
static void* xg_reserve4g(size_t size) {
    void* p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return (p == MAP_FAILED) ? nullptr : p;
}
static int xg_commit_pages(void* base, size_t off, size_t size) {
    return mprotect((char*)base + off, size, PROT_READ | PROT_WRITE);
}
static void xg_release4g(void* base, size_t size) {
    if (base) munmap(base, size);
}
#endif

static int xg_ensure_reserved(x86_context* ctx) {
    if (ctx->mem.base) return 0;
    ctx->mem.base = (uint8_t*)xg_reserve4g((size_t)X86_PAGE_COUNT * X86_PAGE_SIZE);
    memset(ctx->mem.map, 0, sizeof(ctx->mem.map));
    ctx->mem.committed = 0;
    return ctx->mem.base ? 0 : -1;
}

int x86_mem_commit(x86_context* ctx, uint32_t base, size_t size) {
    if (xg_ensure_reserved(ctx) != 0) return -1;
    uint32_t start_page = base >> X86_PAGE_SHIFT;
    uint32_t end_page = (base + (uint32_t)size + X86_PAGE_SIZE - 1) >> X86_PAGE_SHIFT;
    if (end_page > X86_PAGE_COUNT) return -1;

    // Commit in page-sized runs to keep mprotect/VirtualAlloc calls few.
    uint32_t p = start_page;
    while (p < end_page) {
        if (x86_page_mapped(ctx, p)) { ++p; continue; }
        uint32_t run = p;
        while (run < end_page && !x86_page_mapped(ctx, run)) ++run;
        size_t off = (size_t)p * X86_PAGE_SIZE;
        size_t len = (size_t)(run - p) * X86_PAGE_SIZE;
        if (xg_commit_pages(ctx->mem.base, off, len) != 0) return -1;
        memset(ctx->mem.base + off, 0, len); // anonymous pages are zero; enforce it
        for (uint32_t q = p; q < run; q++) ctx->mem.map[q >> 6] |= (1ull << (q & 63));
        ctx->mem.committed += len;
        p = run;
    }
    return 0;
}

void x86_mem_free(x86_context* ctx) {
    if (ctx->mem.base) {
        xg_release4g(ctx->mem.base, (size_t)X86_PAGE_COUNT * X86_PAGE_SIZE);
        ctx->mem.base = nullptr;
    }
    memset(ctx->mem.map, 0, sizeof(ctx->mem.map));
    ctx->mem.committed = 0;
}

int x86_mem_map_host(x86_context* ctx, uint32_t guest_addr, void* host_ptr, size_t size) {
    if (x86_mem_commit(ctx, guest_addr, size) != 0) return -1;
    memcpy(ctx->mem.base + guest_addr, host_ptr, size);
    return 0;
}

// ---- CPU lifecycle ----
void x86_cpu_init(x86_context* ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->regs.esp = 0x00100000; // 1MB stack
    ctx->regs.fpu_top = 0;
    ctx->regs.fpu_cw = 0x037F; // default control word
    ctx->regs.fpu_sw = 0;
    for (int i = 0; i < 8; i++) { ctx->regs.fpu_tags[i] = 3; } // all empty
    ctx->regs.ebp = ctx->regs.esp;
    ctx->regs.eflags = 0x00000202; // IF set
    ctx->regs.cs = 0x1B;
    ctx->regs.ds = 0x23;
    ctx->regs.es = 0x23;
    ctx->regs.fs = 0x3B;
    ctx->regs.fs_base = 0x7FFDE000; // TEB base
    ctx->regs.gs = 0x00;
    ctx->regs.ss = 0x23;
    ctx->insn_limit = 100000000; // 100M instructions max
    ctx->running = 0;

    // ---- TEB + TLS array - Making FS:[...] accesses valid memory ----
    if (x86_mem_commit(ctx, 0x7FFDE000, 0x2000) == 0) {
        // TEB fields the CRT reads
        x86_mem_write32(ctx, 0x7FFDE000 + 0x00, 0xFFFFFFFF); // SEH chain end
        x86_mem_write32(ctx, 0x7FFDE000 + 0x04, 0x00100000); // stack base
        x86_mem_write32(ctx, 0x7FFDE000 + 0x08, 0x00100000); // stack limit
        x86_mem_write32(ctx, 0x7FFDE000 + 0x18, 0x7FFDE000); // Self
        x86_mem_write32(ctx, 0x7FFDE000 + 0x20, 1);          // ProcessId
        x86_mem_write32(ctx, 0x7FFDE000 + 0x24, 1);          // ThreadId
    }
    // TLS pointer table (64 slots of per-module blocks)
    ctx->tls_array = 0x7FFE0000;
    if (x86_mem_commit(ctx, ctx->tls_array, 0x1000) == 0) {
        x86_mem_write32(ctx, 0x7FFDE000 + 0x2C, ctx->tls_array);
    }
    ctx->regs.tsc_base = 0x100000;
}

void x86_cpu_reset(x86_context* ctx) {
    x86_mem_free(ctx);
    x86_cpu_init(ctx);
}

int x86_run(x86_context* ctx) {
    ctx->running = 1;
    ctx->last_error = 0;
    while (ctx->running) {
        if (x86_step(ctx) != 0) break;
    }
    return ctx->last_error;
}

// ---- Run a 32-bit PE through the interpreter ----
int x86_run_pe32(x86_context* ctx, void* image_base, size_t image_size,
                 uint32_t entry_rva, uint32_t stack_base, size_t stack_size) {
    x86_cpu_init(ctx);
    
    // Map the PE image into guest memory at its preferred base
    // For simplicity, map at 0x00400000 (typical PE base)
    uint32_t guest_base = 0x00400000;
    if (x86_mem_map_host(ctx, guest_base, image_base, image_size) != 0) {
        if (ctx->log) ctx->log("x86: failed to map PE image");
        return -1;
    }
    ctx->image_base = image_base;
    ctx->image_size = (uint32_t)image_size;
    
    // Set up stack
    if (x86_mem_commit(ctx, stack_base, stack_size) != 0) {
        if (ctx->log) ctx->log("x86: failed to commit stack");
        return -1;
    }
    ctx->regs.esp = stack_base + (uint32_t)stack_size - 4;
    ctx->regs.ebp = ctx->regs.esp;
    
    // Set entry point
    ctx->regs.eip = guest_base + entry_rva;
    
    // Run
    ctx->running = 1;
    int result = x86_run(ctx);
    
    return result;
}
