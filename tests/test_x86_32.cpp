// tiramisu/tests/test_x86_32.cpp
// Gate for the clean-room x86-32 interpreter: execute REAL 32-bit machine
// code (hand-encoded from the public opcode map) and a named trap.
#include "../src/trt/x86_32.h"

#include <cstdio>
#include <cstring>

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { ++fails; printf("  FAIL: %s (line %d)\n", m, __LINE__); } } while (0)

static trt_x86_ctx make_ctx(u8* code, size_t n, u32 va) {
  static u8 mem[256];
  memset(mem, 0, sizeof(mem));
  memcpy(mem, code, n);
  trt_x86_ctx c{};
  c.mem = mem; c.mem_va = va; c.mem_size = sizeof(mem);
  c.eip = va;
  c.r[X86_ESP] = va + 252;  // 4-byte aligned stack, clear of the code
  return c;
}

static int trap_stop(trt_x86_ctx*, u32, void*) { return 42; }

int main() {
  // mov eax,5 ; mov ecx,3 ; add eax,ecx ; hlt
  u8 add_code[] = {0xb8,5,0,0,0, 0xb9,3,0,0,0, 0x01,0xc8, 0xf4};
  {
    auto c = make_ctx(add_code, sizeof(add_code), 0x1000);
    CHECK(trt_x86_run(&c, 64) == 0, "arithmetic guest halted");
    CHECK(c.r[X86_EAX] == 8, "add produced 8 in eax");
  }

  // mov eax,0 ; mov ecx,3 ; add eax,1 ; sub ecx,1 ; jne -8 ; hlt
  u8 loop_code[] = {0xb8,0,0,0,0, 0xb9,3,0,0,0, 0x83,0xc0,1,
                    0x83,0xe9,1, 0x75,0xf8, 0xf4};
  {
    auto c = make_ctx(loop_code, sizeof(loop_code), 0x2000);
    CHECK(trt_x86_run(&c, 256) == 0, "loop guest halted");
    CHECK(c.r[X86_EAX] == 3 && c.r[X86_ECX] == 0, "loop ran exactly 3 times");
  }

  // mov eax,1 ; call +5 ; add eax,2 ; hlt ; nop ; add eax,3 ; ret
  u8 call_code[] = {0xb8,1,0,0,0, 0xe8,5,0,0,0, 0x83,0xc0,2, 0xf4,
                    0x90, 0x83,0xc0,3, 0xc3};
  {
    auto c = make_ctx(call_code, sizeof(call_code), 0x3000);
    CHECK(trt_x86_run(&c, 64) == 0, "call/ret guest halted");
    CHECK(c.r[X86_EAX] == 6, "call/ret reached the callee and returned");
  }

  // Unsupported opcode must be a NAMED stop, never a silent success.
  u8 bad_code[] = {0x0f,0x0b, 0xf4};  // ud2
  {
    auto c = make_ctx(bad_code, sizeof(bad_code), 0x4000);
    CHECK(trt_x86_run(&c, 16) == 2 && c.stop_code == 0x4000,
          "unsupported opcode stops named at its address");
  }

  // EIP outside the window routes to the trap callback.
  {
    auto c = make_ctx(add_code, sizeof(add_code), 0x5000);
    c.eip = 0x9000;
    c.trap = &trap_stop;
    CHECK(trt_x86_run(&c, 16) == 1 && c.stop_code == 42,
          "out-of-window EIP takes the named trap path");
  }

  // rdtsc (0f 31) then hlt.
  {
    u8 code[] = {0x0f,0x31, 0xf4};
    auto c = make_ctx(code, sizeof(code), 0x6000);
    CHECK(trt_x86_run(&c, 16) == 0, "rdtsc guest halted");
  }
  // movzx eax, byte [0x6200] where the byte is 0x80 -> 0x00000080.
  {
    u8 code[] = {0x0f,0xb6,0x05,0x80,0x61,0x00,0x00, 0xf4};
    auto c = make_ctx(code, sizeof(code), 0x6100);
    // place the immediate at relative offset 0x100 in the ctx window
    // (make_ctx zeroes the window; write through the public helper)
    u8 val = 0x80;
    trt_x86_mem_write(&c, 0x6180, 1, &val);
    CHECK(trt_x86_run(&c, 16) == 0 && c.r[X86_EAX] == 0x80,
          "movzx zero-extended a real memory byte");
  }
  // shl eax,3: mov eax,1 ; c1 e0 03 ; hlt -> eax=8.
  {
    u8 code[] = {0xb8,1,0,0,0, 0xc1,0xe0,3, 0xf4};
    auto c = make_ctx(code, sizeof(code), 0x7000);
    CHECK(trt_x86_run(&c, 16) == 0 && c.r[X86_EAX] == 8,
          "immediate shift produced 8");
  }
  // mov al,0x42 ; hlt -> eax low byte 0x42.
  {
    u8 code[] = {0xb0,0x42, 0xf4};
    auto c = make_ctx(code, sizeof(code), 0x8000);
    CHECK(trt_x86_run(&c, 16) == 0 && (c.r[X86_EAX] & 0xff) == 0x42,
          "8-bit immediate mov wrote AL");
  }
  // loop: mov ecx,3 ; loop -2 ; hlt -> ecx=0.
  {
    u8 code[] = {0xb9,3,0,0,0, 0xe2,0xfe, 0xf4};
    auto c = make_ctx(code, sizeof(code), 0x9000);
    CHECK(trt_x86_run(&c, 32) == 0 && c.r[X86_ECX] == 0,
          "loop decremented ECX to zero and exited");
  }

  // 0x66 operand-size prefix: mov ax,imm16 preserves the upper half of eax.
  {
    u8 code[] = {0xb8,0x00,0x00,0xff,0xff, 0x66,0xb8,0x34,0x12, 0xf4};
    auto c = make_ctx(code, sizeof(code), 0xa000);
    CHECK(trt_x86_run(&c, 32) == 0 && c.r[X86_EAX] == 0xffff1234u,
          "66h prefix wrote only AX (upper EAX preserved)");
  }
  // 66 89 /r: mov word [mem], ax via a 16-bit memory write.
  {
    u8 code[] = {0x66,0x89,0x05,0x80,0xa1,0x00,0x00, 0xf4};
    auto c = make_ctx(code, sizeof(code), 0xa100);
    c.r[X86_EAX] = 0xffff5678u;
    CHECK(trt_x86_run(&c, 32) == 0, "66h word memory write halted");
    u16 w = 0; trt_x86_mem_read(&c, 0xa180, 2, &w);
    CHECK(w == 0x5678, "66h prefix wrote exactly 2 bytes to memory");
  }

  printf("  result: %s\n", fails ? "FAIL" : "ALL PASS");
  return fails ? 1 : 0;
}
