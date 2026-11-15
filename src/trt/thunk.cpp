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

#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr size_t k_thunk_stride = 64;
constexpr size_t k_thunks_per_page = 4096 / k_thunk_stride; // 64 slots per 4 KiB page

struct thunk_entry {
  std::string kind;
  std::string mod;
  std::string sym;
  void* code = nullptr;
};

std::mutex g_thunk_mu;
std::deque<thunk_entry> g_thunks;
std::unordered_map<std::string, size_t> g_thunk_map;
std::vector<unsigned char*> g_thunk_pages;

std::mutex g_backend_mu;
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

void write_stub_slot(unsigned char* code, u64 index, u64 dispatch_addr) {
  unsigned char* p = code;
  memcpy(p, k_stub_head, sizeof(k_stub_head)); p += sizeof(k_stub_head);
  memcpy(p, k_stub_arg_mov, sizeof(k_stub_arg_mov)); p += sizeof(k_stub_arg_mov);
  put_u64(p, index); p += 8;
  memcpy(p, k_stub_mov_r11, sizeof(k_stub_mov_r11)); p += sizeof(k_stub_mov_r11);
  put_u64(p, dispatch_addr); p += 8;
  memcpy(p, k_stub_call_r11, sizeof(k_stub_call_r11)); p += sizeof(k_stub_call_r11);
  memcpy(p, k_stub_tail, sizeof(k_stub_tail));
}

} // namespace

extern "C" {

void trt_core_set_backend(const trt_backend* be) {
  if (!be) return;
  std::lock_guard<std::mutex> lk(g_backend_mu);
  g_backend = *be;
  g_backend_set = true;
}

const trt_backend* trt_core_backend(void) {
  std::lock_guard<std::mutex> lk(g_backend_mu);
  return g_backend_set ? &g_backend : nullptr;
}

void trt_core_log(const char* line) {
  void (*log_fn)(const char*) = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_backend_mu);
    if (g_backend_set && g_backend.log) log_fn = g_backend.log;
  }
  if (log_fn) log_fn(line);
  else fprintf(stderr, "  [trt] %s\n", line);
}

// the target of every stub: names the symbol, returns 0
extern "C" u64 trt_thunk_dispatch(u64 index) {
  std::string kind, mod, sym;
  bool found = false;
  {
    std::lock_guard<std::mutex> lk(g_thunk_mu);
    if (index < (u64)g_thunks.size()) {
      const thunk_entry& e = g_thunks[(size_t)index];
      kind = e.kind;
      mod = e.mod;
      sym = e.sym;
      found = true;
    }
  }
  if (found) {
    char line[512];
    snprintf(line, sizeof(line), "trt: %s %s!%s called (unimplemented)",
             kind.c_str(), mod.c_str(), sym.c_str());
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

  std::string key;
  key.reserve(strlen(kind) + strlen(mod) + strlen(sym) + 2);
  key.append(kind).push_back('\x1f');
  key.append(mod).push_back('\x1f');
  key.append(sym);

  const trt_backend* backend = be ? be : trt_core_backend();
  void* created_code = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_thunk_mu);
    auto it = g_thunk_map.find(key);
    if (it != g_thunk_map.end()) return g_thunks[it->second].code;

    if (!backend || !backend->alloc || !backend->protect) return nullptr;

    const size_t index = g_thunks.size();
    const size_t page_idx = index / k_thunks_per_page;
    const size_t slot_idx = index % k_thunks_per_page;

    if (page_idx >= g_thunk_pages.size()) {
      // Pre-populate all 64 slots on a fresh 4 KiB page while RW, then flip RX once.
      unsigned char* page = (unsigned char*)backend->alloc(4096, 0);
      if (!page) return nullptr;
      const u64 base_idx = (u64)page_idx * k_thunks_per_page;
      const u64 dispatch_addr = (u64)(uintptr_t)&trt_thunk_dispatch;
      for (size_t s = 0; s < k_thunks_per_page; ++s) {
        write_stub_slot(page + s * k_thunk_stride, base_idx + s, dispatch_addr);
      }
      backend->protect(page, 4096, TRT_PROT_R | TRT_PROT_X);
      g_thunk_pages.push_back(page);
    }

    created_code = g_thunk_pages[page_idx] + slot_idx * k_thunk_stride;
    thunk_entry e;
    e.kind = kind;
    e.mod = mod;
    e.sym = sym;
    e.code = created_code;
    g_thunks.push_back(std::move(e));
    g_thunk_map.emplace(std::move(key), index);
  }

  char line[512];
  snprintf(line, sizeof(line), "trt: %s %s!%s -> fail-visible thunk",
           kind, mod, sym);
  trt_core_log(line);
  return created_code;
}

} // extern "C"
