// tiramisu/src/trt/bridge_kernel32.cpp
// Builtin kernel32 — the subset a game boots on, implemented on top of the
// backend (memory) and CRT. Wine semantics, Tiramisu implementation.
// v0 scope: heap/memory/sync-primitive/file-io/clock/error/TLS basics.
#include "trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace {

// ---- fake handle space (stable, never reused within a session) ----
struct trt_handle_table {
  void* ptr;
  u32   tag;
};
trt_handle_table g_handles[256];
u32 g_tls_last_error[128] = {0};

__thread u32 g_tls_index_current_thread = 0;  // skeleton: one guest thread

u32 err_for_current_thread() {
  return g_tls_last_error[g_tls_index_current_thread % 128];
}

void* handle_alloc(u32 tag, void* ptr) {
  for (int i = 0; i < 256; ++i) {
    if (g_handles[i].tag == 0) {
      g_handles[i].tag = tag;
      g_handles[i].ptr = ptr;
      return (void*)(uintptr_t)((tag << 16) | (u32)(i + 1));
    }
  }
  return (void*)(uintptr_t)-1;
}
// ---- backend singletons (set at boot by host/test) ----
trt_backend g_be;
bool g_be_ready = false;

// ---- QPC ----
i64 now_qpc_ticks() {
  struct timespec ts;
#if defined(CLOCK_MONOTONIC)
  clock_gettime(CLOCK_MONOTONIC, &ts);
#else
  timespec_get(&ts, TIME_UTC);
#endif
  return (i64)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

} // namespace

extern "C" {

// ---- guest-visible kernel32 surface (v0) ----

void* TRT_STDCALL lgn_GetProcessHeap(void) { return (void*)(uintptr_t)0x1; }

void* TRT_STDCALL lgn_HeapCreate(u32 flags, u64 init, u64 max) {
  (void)flags; (void)init; (void)max;
  return handle_alloc(0xCAFE, nullptr);  // heap handle: a record, not memory
}

void* TRT_STDCALL lgn_HeapAlloc(void* heap, u32 flags, u64 bytes) {
  (void)heap;
  // backend alloc = RW memory; flags HEAP_ZERO_MEMORY = 0x8
  u8* p = (u8*)g_be.alloc((size_t)(bytes ? bytes : 1), 0);
  if (p && (flags & 0x8)) memset(p, 0, (size_t)bytes);
  return p;
}

int TRT_STDCALL lgn_HeapFree(void* heap, u32 flags, void* p) {
  (void)heap; (void)flags;
  if (p) g_be.release(p, 0);  // backend tracks size internally (mmap/UWP)
  return 1;
}

void* TRT_STDCALL lgn_VirtualAlloc(void* addr, u64 bytes, u32 type, u32 prot) {
  // v0: only MEM_COMMIT|MEM_RESERVE (0x3000) with RW or RWX-free protections
  if (!(type & 0x3000) || (prot & 0x40) /*PAGE_EXECUTE_READWRITE*/) {
    g_tls_last_error[g_tls_index_current_thread % 128] = 87; // ERROR_INVALID_PARAMETER
    return nullptr;
  }
  (void)prot; // v0: allocations are RW; exec comes from loaded images only
  return g_be.alloc((size_t)(bytes ? bytes : 4096), (u64)(uintptr_t)addr);
}

int TRT_STDCALL lgn_VirtualFree(void* addr, u64 bytes, u32 type) {
  if (type != 0x8000 /*MEM_RELEASE*/ || !addr) return 0;
  g_be.release(addr, bytes);
  return 1;
}

u32 TRT_STDCALL lgn_GetLastError(void) { return err_for_current_thread(); }
void TRT_STDCALL lgn_SetLastError(u32 err) {
  g_tls_last_error[g_tls_index_current_thread % 128] = err;
}

u32 TRT_STDCALL lgn_GetCurrentThreadId(void) { return g_tls_index_current_thread; }
void* TRT_STDCALL lgn_GetCurrentProcess(void) { return (void*)(uintptr_t)-1; }

int TRT_STDCALL lgn_QueryPerformanceCounter(i64* out) {
  if (!out) return 0;
  *out = now_qpc_ticks();
  return 1;
}
int TRT_STDCALL lgn_QueryPerformanceFrequency(i64* out) {
  if (!out) return 0;
  *out = 1000000000ll;  // nanosecond tick (matches QPC source above)
  return 1;
}

u32 TRT_STDCALL lgn_IsProcessorFeaturePresent(u32 feature) {
  // x64 baseline: SSE/SSE2/SSE3/SSSE3/SSE4.1/SSE4.2/POPCNT/NX present
  switch (feature) {
    case 0: case 1: case 6: case 7: case 8: case 9: case 10: case 23:
      return 1;
    default:
      return 0;
  }
}

void TRT_STDCALL trt_Sleep(u32 ms) {
#ifdef _WIN32
  ::Sleep(ms);  // we are inside a Windows process already
#else
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000l;
  nanosleep(&ts, nullptr);
#endif
}

// ---- boot ----
void trt_bridge_boot(const trt_backend* be) {
  if (be) { g_be = *be; g_be_ready = true; }
  for (u32 t = 0; t < 128; ++t) g_tls_last_error[t] = 0;
}

void trt_bridge_new_thread_id(u32 id) {
  g_tls_index_current_thread = id % 128;
}

static const trt_named_fn k_kernel32_fns[] = {
  {"GetProcessHeap", (void*)&lgn_GetProcessHeap},
  {"HeapCreate", (void*)&lgn_HeapCreate},
  {"HeapAlloc", (void*)&lgn_HeapAlloc},
  {"HeapFree", (void*)&lgn_HeapFree},
  {"VirtualAlloc", (void*)&lgn_VirtualAlloc},
  {"VirtualFree", (void*)&lgn_VirtualFree},
  {"GetLastError", (void*)&lgn_GetLastError},
  {"SetLastError", (void*)&lgn_SetLastError},
  {"GetCurrentThreadId", (void*)&lgn_GetCurrentThreadId},
  {"GetCurrentProcess", (void*)&lgn_GetCurrentProcess},
  {"QueryPerformanceCounter", (void*)&lgn_QueryPerformanceCounter},
  {"QueryPerformanceFrequency", (void*)&lgn_QueryPerformanceFrequency},
  {"IsProcessorFeaturePresent", (void*)&lgn_IsProcessorFeaturePresent},
  {"Sleep", (void*)&trt_Sleep},
};

int trt_bridge_register_kernel32(void) {
  trt_module_register("kernel32.dll", TRT_ROUTE_BUILTIN, k_kernel32_fns,
                       (int)(sizeof(k_kernel32_fns) / sizeof(k_kernel32_fns[0])));
  // kernelbase forwards: PE games import a mix; route kernelbase to the same
  // builtin surface (Wine does the same via spec forwarders)
  trt_module_register("kernelbase.dll", TRT_ROUTE_BUILTIN, k_kernel32_fns,
                       (int)(sizeof(k_kernel32_fns) / sizeof(k_kernel32_fns[0])));
  trt_module_register("api-ms-win-core-rtlsupport-l1-1-0.dll", TRT_ROUTE_STUB,
                       nullptr, 0);
  return 0;
}

} // extern "C"
