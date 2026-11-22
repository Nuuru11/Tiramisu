// tiramisu/src/trt/x86_cpu.h
// x86 (32-bit) CPU emulation core for Tiramisu.
//
// Provides register state, flat 4GB memory model, instruction decode/execute,
// and bridge thunking so 32-bit PE guests can run on the 64-bit UWP host.
//
// This is NOT a full emulator. It handles common integer instructions,
// memory access, control flow, and API call thunking. FPU/SSE and
// advanced features are stubbed where needed.
#ifndef TRT_X86_CPU_H
#define TRT_X86_CPU_H

#include "trt.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

// ---- x86 registers ----
struct x86_regs {
    // General purpose (32-bit)
    uint32_t eax, ebx, ecx, edx;
    uint32_t esi, edi, ebp, esp;
    // Instruction pointer
    uint32_t eip;
    // Flags (EFLAGS)
    uint32_t eflags;
    // Segment registers (mostly ignored, flat model)
    uint16_t cs, ds, es, fs, gs, ss;
    uint32_t fs_base; // FS segment base (for SEH/TEB)
    // FPU (x87) - 8 registers as doubles (host computation)
    double fpu[8];
    int fpu_top;  // top of stack index (0-7)
    uint16_t fpu_cw, fpu_sw, fpu_tw;
    // Tag word: 00=valid, 01=zero, 10=special, 11=empty
    uint8_t fpu_tags[8];
    // SSE - 8 registers, 128-bit
    uint8_t xmm[8][16];
    uint32_t mxcsr;
    // MMX - 8 registers, 64-bit (aliases x87 in real HW; kept separate here)
    uint64_t mm[8];
    // TLS slots (TEB ThreadLocalStoragePointer at FS:[0x2C])
    uint32_t tls_slot[64];
    uint8_t tls_used[64];
    // Time-stamp counter base for RDTSC
    uint64_t tsc_base;
};

// EFLAGS bits
#define X86_CF  (1 << 0)   // Carry
#define X86_PF  (1 << 2)   // Parity
#define X86_AF  (1 << 4)   // Auxiliary carry
#define X86_ZF  (1 << 6)   // Zero
#define X86_SF  (1 << 7)   // Sign
#define X86_TF  (1 << 8)   // Trap
#define X86_IF  (1 << 9)   // Interrupt enable
#define X86_DF  (1 << 10)  // Direction
#define X86_OF  (1 << 11)  // Overflow

// ---- Memory model ----
// Flat 4GB guest address space backed by ONE contiguous host reservation, so a
// guest VA is directly usable as a host pointer by the 64-bit bridges. Pages
// are committed lazily and tracked by a bitmap; uncommitted accesses read 0
// and writes are dropped (never a host crash).
#define X86_PAGE_SIZE 4096
#define X86_PAGE_COUNT (1 << 20)  // 4GB / 4KB
#define X86_PAGE_SHIFT 12
#define X86_MAP_WORDS (X86_PAGE_COUNT / 64)

struct x86_mem {
    uint8_t* base;            // 4GB reservation (nullptr before first commit)
    uint64_t map[X86_MAP_WORDS]; // committed-page bitmap (1 = readable/writable)
    size_t committed;         // total committed bytes
};

// ---- Execution context ----
struct x86_context {
    x86_regs regs;
    x86_mem mem;
    // Guest image base (host pointer to mapped PE)
    void* image_base;
    uint32_t image_size;
    // Entry point (RVA)
    uint32_t entry_rva;
    // Is the CPU running?
    int running;
    // Last error
    int last_error;
    // Instruction count (for debugging/timeouts)
    uint64_t insn_count;
    // Max instructions before forced stop
    uint64_t insn_limit;
    // Log callback
    void (*log)(const char*);
    // TLS array base address (guest)
    uint32_t tls_array;
};

// ---- Memory access helpers ----
static inline int x86_page_mapped(x86_context* ctx, uint32_t page) {
    return (ctx->mem.map[page >> 6] >> (page & 63)) & 1;
}
static inline uint8_t x86_mem_read8(x86_context* ctx, uint32_t addr) {
    if (!ctx->mem.base || !x86_page_mapped(ctx, addr >> X86_PAGE_SHIFT)) {
        ctx->last_error = 1; // page fault
        return 0;
    }
    return ctx->mem.base[addr];
}

static inline uint16_t x86_mem_read16(x86_context* ctx, uint32_t addr) {
    uint16_t v = x86_mem_read8(ctx, addr);
    v |= (uint16_t)x86_mem_read8(ctx, addr + 1) << 8;
    return v;
}

static inline uint32_t x86_mem_read32(x86_context* ctx, uint32_t addr) {
    uint32_t v = x86_mem_read8(ctx, addr);
    v |= (uint32_t)x86_mem_read8(ctx, addr + 1) << 8;
    v |= (uint32_t)x86_mem_read8(ctx, addr + 2) << 16;
    v |= (uint32_t)x86_mem_read8(ctx, addr + 3) << 24;
    return v;
}

static inline void x86_mem_write8(x86_context* ctx, uint32_t addr, uint8_t val) {
    if (!ctx->mem.base || !x86_page_mapped(ctx, addr >> X86_PAGE_SHIFT)) {
        ctx->last_error = 1;
        return;
    }
    ctx->mem.base[addr] = val;
}

static inline void x86_mem_write16(x86_context* ctx, uint32_t addr, uint16_t val) {
    x86_mem_write8(ctx, addr, (uint8_t)(val & 0xFF));
    x86_mem_write8(ctx, addr + 1, (uint8_t)((val >> 8) & 0xFF));
}

static inline void x86_mem_write32(x86_context* ctx, uint32_t addr, uint32_t val) {
    x86_mem_write8(ctx, addr, (uint8_t)(val & 0xFF));
    x86_mem_write8(ctx, addr + 1, (uint8_t)((val >> 8) & 0xFF));
    x86_mem_write8(ctx, addr + 2, (uint8_t)((val >> 16) & 0xFF));
    x86_mem_write8(ctx, addr + 3, (uint8_t)((val >> 24) & 0xFF));
}

// ---- Bulk guest-memory access (page-table backed, never host pointers) ----
static inline void xg_read(x86_context* ctx, uint32_t addr, void* out, size_t n) {
    uint8_t* p = (uint8_t*)out;
    for (size_t i = 0; i < n; i++) p[i] = x86_mem_read8(ctx, addr + (uint32_t)i);
}
static inline void xg_write(x86_context* ctx, uint32_t addr, const void* in, size_t n) {
    const uint8_t* p = (const uint8_t*)in;
    for (size_t i = 0; i < n; i++) x86_mem_write8(ctx, addr + (uint32_t)i, p[i]);
}
static inline uint64_t xg_read64(x86_context* ctx, uint32_t addr) {
    uint64_t v; xg_read(ctx, addr, &v, 8); return v;
}
static inline void xg_write64(x86_context* ctx, uint32_t addr, uint64_t v) {
    xg_write(ctx, addr, &v, 8);
}

// ---- Memory management ----
// Commit a range of guest memory (maps host pages)
int x86_mem_commit(x86_context* ctx, uint32_t base, size_t size);
// Free all committed memory
void x86_mem_free(x86_context* ctx);
// Map a host buffer into guest address space (zero-copy for PE image)
int x86_mem_map_host(x86_context* ctx, uint32_t guest_addr, void* host_ptr, size_t size);

// ---- Run a 32-bit PE through the interpreter ----
// Takes a loaded PE32 image (host pointer + size + entry RVA)
// and runs it through the x86 interpreter.
// Returns 0 on success, -1 on error.
int x86_run_pe32(x86_context* ctx, void* image_base, size_t image_size,
                 uint32_t entry_rva, uint32_t stack_base, size_t stack_size);

// ---- CPU lifecycle ----
// Initialize CPU state
void x86_cpu_init(x86_context* ctx);
// Reset CPU to initial state
void x86_cpu_reset(x86_context* ctx);
// Execute one instruction, return 0 on success, -1 on error/halt
int x86_step(x86_context* ctx);
// Run until halt or insn_limit
int x86_run(x86_context* ctx);

// ---- Helpers ----
// Get register by index (0=EAX,1=ECX,2=EDX,3=EBX,4=ESP,5=EBP,6=ESI,7=EDI)
uint32_t* x86_reg_ptr(x86_context* ctx, int idx);
// Update flags for arithmetic result
void x86_update_flags_add(x86_context* ctx, uint32_t a, uint32_t b, uint32_t result);
void x86_update_flags_sub(x86_context* ctx, uint32_t a, uint32_t b, uint32_t result);
void x86_update_flags_logic(x86_context* ctx, uint32_t result);

// ---- Bridge thunking ----
// When guest calls an imported function, this resolves and calls the 64-bit bridge
// Returns the 32-bit result
uint32_t x86_call_bridge(x86_context* ctx, const char* module, const char* symbol, uint32_t* args, int nargs);

#endif // TRT_X86_CPU_H
