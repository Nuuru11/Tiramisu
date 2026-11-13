// tiramisu/src/trt/thunk.cpp
// Per-symbol fail-visible thunks (see thunk.h).
//
// Why machine code: the thunk must be callable through the guest's IAT,
// so it has to be a real code address with a known calling convention.
// The stub is Win64-correct, which is the part that bites:
//
//   Win64 nonvolatile registers are rbx, rbp, rdi, rsi, rsp, r12-r15.
//   The guest keeps locals in those across a call, so a stub that
//   clobbers rdi corrupts the guest SILENTLY (this bit us once already).
//   We save/restore rdi+rsi around the handler and keep the stack
//   16-byte aligned for the handler's own ABI:
//
//   57                push rdi
//   56                push rsi
//   48 83 EC 08       sub  rsp, 8
//   48 BF/B9 <idx>    mov  rdi/rcx, idx     ; host ABI first-arg register
//   49 BB <handler>   mov  r11, handler
//   41 FF D3          call r11
//   48 83 C4 08       add  rsp, 8
//   5E                pop  rsi
//   5F                pop  rdi
//   C3                ret
//
// The handler then logs the symbol name and returns 0 in rax.
//
// W^X: the page is allocated RW through the backend, written, then flipped
// to RX - never RWX, exactly like the PE image seal path.
#include "thunk.h"

#include <stdio.h>
#include <string.h>

namespace {

struct thunk_entry {
  char kind[16];
  char mod[96];
  char sym[96];
  void* code;
};

thunk_entry g_thunks[128];
int g_thunk_count = 0;

trt_backend g_backend;
bool g_backend_set = false;

#ifdef _WIN32
// Win64 host ABI: first argument register is rcx (volatile - no save needed).
const unsigned char k_stub_arg_mov[] = {0x48, 0xB9};  // mov rcx, imm64
#else
// SysV host ABI (Linux test harness): first argument register is rdi, which
// is NONVOLATILE in Win64 - hence the explicit save/restore around it.
const unsigned char k_stub_arg_mov[] = {0x48, 0xBF};  // mov rdi, imm64
#endif
const unsigned char k_stub_head[] = {
    0x57,                    // push rdi
    0x56,                    // push rsi
    0x48, 0x83, 0xEC, 0x08,  // sub rsp, 8   (stack alignment for the call)
};
const unsigned char k_stub_mov_r11[] = {0x49, 0xBB};        // mov r11, imm64
const unsigned char k_stub_call_r11[] = {0x41, 0xFF, 0xD3}; // call r11
const unsigned char k_stub_tail[] = {
    0x48, 0x83, 0xC4, 0x08,  // add rsp, 8
    0x5E,                    // pop rsi
    0x5F,                    // pop rdi
    0xC3,                    // ret
};

void put_u64(unsigned char* p, u64 v) {
  for (int i = 0; i < 8; ++i) p[i] = (unsigned char)((v >> (i * 8)) & 0xFF);
}

} // namespace

extern "C" {

void trt_core_set_backend(const trt_backend* be) {
  if (be) { g_backend = *be; g_backend_set = true; }
}

const trt_backend* trt_core_backend(void) {
  return g_backend_set ? &g_backend : nullptr;
}

void trt_core_log(const char* line) {
  if (g_backend_set && g_backend.log) g_backend.log(line);
  else fprintf(stderr, "  [trt] %s\n", line);
}

// the target of every stub: names the symbol, returns 0
extern "C" u64 trt_thunk_dispatch(u64 index) {
  if (index < (u64)g_thunk_count) {
    thunk_entry& e = g_thunks[index];
    char line[256];
    snprintf(line, sizeof(line), "trt: %s %s!%s called (unimplemented)",
             e.kind, e.mod, e.sym);
    trt_core_log(line);
  } else {
    trt_core_log("trt: thunk called with a bad index");
  }
  return 0;
}

void* trt_thunk_make(const trt_backend* be, const char* kind, const char* mod,
                     const char* sym) {
  if (!kind) kind = "STUB";
  if (!mod) mod = "?";
  if (!sym) sym = "?";

  // reuse an identical thunk if we already made one
  for (int i = 0; i < g_thunk_count; ++i)
    if (strcmp(g_thunks[i].kind, kind) == 0 &&
        strcmp(g_thunks[i].mod, mod) == 0 &&
        strcmp(g_thunks[i].sym, sym) == 0)
      return g_thunks[i].code;

  if (g_thunk_count >= (int)(sizeof(g_thunks) / sizeof(g_thunks[0])))
    return nullptr;

  const trt_backend* backend = be ? be : trt_core_backend();
  if (!backend || !backend->alloc || !backend->protect) return nullptr;

  thunk_entry& e = g_thunks[g_thunk_count];
  memset(&e, 0, sizeof(e));
  snprintf(e.kind, sizeof(e.kind), "%s", kind);
  snprintf(e.mod, sizeof(e.mod), "%s", mod);
  snprintf(e.sym, sizeof(e.sym), "%s", sym);

  // one page is plenty for 128 stubs; allocate 4 KiB RW, write, flip RX
  unsigned char* page = (unsigned char*)backend->alloc(4096, 0);
  if (!page) return nullptr;

  const u64 index = (u64)g_thunk_count;
  unsigned char* code = page + (size_t)index * 64;  // 64-byte stride
  unsigned char* p = code;
  memcpy(p, k_stub_head, sizeof(k_stub_head)); p += sizeof(k_stub_head);
  memcpy(p, k_stub_arg_mov, sizeof(k_stub_arg_mov)); p += sizeof(k_stub_arg_mov);
  put_u64(p, index); p += 8;
  memcpy(p, k_stub_mov_r11, sizeof(k_stub_mov_r11)); p += sizeof(k_stub_mov_r11);
  put_u64(p, (u64)(uintptr_t)&trt_thunk_dispatch); p += 8;
  memcpy(p, k_stub_call_r11, sizeof(k_stub_call_r11)); p += sizeof(k_stub_call_r11);
  memcpy(p, k_stub_tail, sizeof(k_stub_tail)); p += sizeof(k_stub_tail);

  // W^X: RW -> RX, never RWX
  backend->protect(page, 4096, TRT_PROT_R | TRT_PROT_X);

  e.code = code;
  ++g_thunk_count;

  char line[256];
  snprintf(line, sizeof(line), "trt: %s %s!%s -> fail-visible thunk",
           kind, mod, sym);
  trt_core_log(line);
  return code;
}

} // extern "C"
