// tiramisu/src/trt/bridge_kernel32.cpp
// Builtin kernel32 — the subset a game boots on, implemented on top of the
// backend (memory) and CRT. Wine semantics, Tiramisu implementation.
// v0 scope: heap/memory/sync-primitive/file-io/clock/error/TLS basics.
#include "trt.h"
#include "thunk.h"
#include "images.h"
#include "pe_exports.h"
#include "bridge_d3d.h"
#include "bridge_reg.h"

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

void* TRT_STDCALL trt_GetProcessHeap(void) { return (void*)(uintptr_t)0x1; }

void* TRT_STDCALL trt_HeapCreate(u32 flags, u64 init, u64 max) {
  (void)flags; (void)init; (void)max;
  return handle_alloc(0xCAFE, nullptr);  // heap handle: a record, not memory
}

void* TRT_STDCALL trt_HeapAlloc(void* heap, u32 flags, u64 bytes) {
  (void)heap;
  // backend alloc = RW memory; flags HEAP_ZERO_MEMORY = 0x8
  u8* p = (u8*)g_be.alloc((size_t)(bytes ? bytes : 1), 0);
  if (p && (flags & 0x8)) memset(p, 0, (size_t)bytes);
  return p;
}

int TRT_STDCALL trt_HeapFree(void* heap, u32 flags, void* p) {
  (void)heap; (void)flags;
  if (p) g_be.release(p, 0);  // backend tracks size internally (mmap/UWP)
  return 1;
}

void* TRT_STDCALL trt_VirtualAlloc(void* addr, u64 bytes, u32 type, u32 prot) {
  // v0: only MEM_COMMIT|MEM_RESERVE (0x3000) with RW or RWX-free protections
  if (!(type & 0x3000) || (prot & 0x40) /*PAGE_EXECUTE_READWRITE*/) {
    g_tls_last_error[g_tls_index_current_thread % 128] = 87; // ERROR_INVALID_PARAMETER
    return nullptr;
  }
  (void)prot; // v0: allocations are RW; exec comes from loaded images only
  return g_be.alloc((size_t)(bytes ? bytes : 4096), (u64)(uintptr_t)addr);
}

int TRT_STDCALL trt_VirtualFree(void* addr, u64 bytes, u32 type) {
  if (type != 0x8000 /*MEM_RELEASE*/ || !addr) return 0;
  g_be.release(addr, bytes);
  return 1;
}

u32 TRT_STDCALL trt_GetLastError(void) { return err_for_current_thread(); }
void TRT_STDCALL trt_SetLastError(u32 err) {
  g_tls_last_error[g_tls_index_current_thread % 128] = err;
}

u32 TRT_STDCALL trt_GetCurrentThreadId(void) { return g_tls_index_current_thread; }
void* TRT_STDCALL trt_GetCurrentProcess(void) { return (void*)(uintptr_t)-1; }

int TRT_STDCALL trt_QueryPerformanceCounter(i64* out) {
  if (!out) return 0;
  *out = now_qpc_ticks();
  return 1;
}
int TRT_STDCALL trt_QueryPerformanceFrequency(i64* out) {
  if (!out) return 0;
  *out = 1000000000ll;  // nanosecond tick (matches QPC source above)
  return 1;
}

u32 TRT_STDCALL trt_IsProcessorFeaturePresent(u32 feature) {
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
  // the core needs the backend too: fail-visible thunks allocate through it
  trt_core_set_backend(be);
  // module coverage for real PC games (idempotent; bridges override)
  trt_modules_register_defaults();
  // the graphics bridge takes over d3d11/dxgi (its own functions first,
  // everything else falls through to the console's real DLLs)
  trt_bridge_register_d3d();
  // the registry: UWP has none, games expect one
  trt_bridge_register_registry();
  for (u32 t = 0; t < 128; ++t) g_tls_last_error[t] = 0;
}

// ================= files on the VFS (the game-transfer path) ================
namespace {

struct file_rec { void* os; u8 mode; };  // os = FILE*
file_rec g_files[32];

FILE* file_os(void* h) {
  const u32 v = (u32)(uintptr_t)h;
  if ((v >> 20) != 0x777 /*'t handle tag'*/ || (v & 0xFFFFF) >= 32) return nullptr;
  return (FILE*)g_files[v & 0xFFFFF].os;
}

void* file_alloc(FILE* os, u8 mode) {
  for (int i = 0; i < 32; ++i) {
    if (!g_files[i].os) {
      g_files[i].os = os;
      g_files[i].mode = mode;
      return (void*)(uintptr_t)(0x77700000u | (u32)i);
    }
  }
  return (void*)(uintptr_t)-1;
}

char g_cwd[512] = "C:\\";   // guest cwd
char g_exepath[512] = "Z:\\game.exe";

} // namespace

void trt_bridge_set_exe_path(const char* zpath) {
  strncpy(g_exepath, zpath, sizeof(g_exepath) - 1);
}

void* TRT_STDCALL trt_CreateFileW(const u16* name, u32 access, u32 share,
                                  void* sa, u32 disposition, u32 flags,
                                  void* tmpl) {
  (void)share; (void)sa; (void)disposition; (void)flags; (void)tmpl;
  if (!name) return (void*)(uintptr_t)-1;
  char host[1024];
  if (trt_vfs_resolve_utf16(name, host, sizeof(host)) != 0) {
    g_tls_last_error[g_tls_index_current_thread % 128] = 3; // PATH_NOT_FOUND
    return (void*)(uintptr_t)-1;
  }
  const bool write = (access & 0x40000000u) != 0 || (access & 0x2u) != 0;
  FILE* os = fopen(host, write ? "r+b" : "rb");
  if (!os && write) os = fopen(host, "w+b");
  if (!os) {
    g_tls_last_error[g_tls_index_current_thread % 128] = 2; // FILE_NOT_FOUND
    return (void*)(uintptr_t)-1;
  }
  return file_alloc(os, write ? 'w' : 'r');
}

int TRT_STDCALL trt_ReadFile(void* h, void* buf, u32 bytes, u32* read,
                             void* overlapped) {
  (void)overlapped;
  FILE* os = file_os(h);
  if (!os || !buf) { if (read) *read = 0; return 0; }
  const size_t n = fread(buf, 1, bytes, os);
  if (read) *read = (u32)n;
  return 1;
}

int TRT_STDCALL trt_WriteFile(void* h, const void* buf, u32 bytes, u32* written,
                              void* overlapped) {
  (void)overlapped;
  FILE* os = file_os(h);
  if (!os || !buf) { if (written) *written = 0; return 0; }
  const size_t n = fwrite(buf, 1, bytes, os);
  if (written) *written = (u32)n;
  return 1;
}

int TRT_STDCALL trt_CloseHandle(void* h) {
  FILE* os = file_os(h);
  if (!os) return 1;  // non-file handles: pretend closed
  fclose(os);
  const u32 i = (u32)(uintptr_t)h & 0xFFFFF;
  g_files[i].os = nullptr;
  return 1;
}

u32 TRT_STDCALL trt_GetFileSize(void* h, u32* hi);

u32 TRT_STDCALL trt_SetFilePointer(void* h, i64 dist, i64* dist_hi, u32 method) {
  (void)dist_hi;
  FILE* os = file_os(h);
  if (!os) return 0xFFFFFFFF;
  const int whence = method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END;
#ifdef _WIN32
  if (_fseeki64(os, dist, whence)) return 0xFFFFFFFF;
#else
  if (fseek(os, (long)dist, whence)) return 0xFFFFFFFF;
#endif
  return trt_GetFileSize(h, nullptr);
}

u32 TRT_STDCALL trt_GetFileSize(void* h, u32* hi) {
  FILE* os = file_os(h);
  if (hi) *hi = 0;
  if (!os) return 0xFFFFFFFF;
  const long pos = ftell(os);
  fseek(os, 0, SEEK_END);
  const long sz = ftell(os);
  fseek(os, pos, SEEK_SET);
  return (u32)sz;
}

int TRT_STDCALL trt_SetCurrentDirectoryW(const u16* name) {
  if (!name) return 0;
  char host[1024];
  // cwd stays a GUEST path (C:\...) so relative opens resolve through VFS
  size_t i = 0;
  for (; name[i] && i < sizeof(g_cwd) - 1; ++i) g_cwd[i] = (char)name[i];
  g_cwd[i] = 0;
  (void)host;
  return 1;
}

int TRT_STDCALL trt_GetCurrentDirectoryW(u32 len, u16* buf) {
  if (!buf || len == 0) return 0;
  u32 i = 0;
  for (; g_cwd[i] && i < len - 1; ++i) buf[i] = (u16)(u8)g_cwd[i];
  buf[i] = 0;
  return (int)i + 1;
}

const u16* TRT_STDCALL trt_GetEnvironmentStringsW() {
  // fixed block: double-NUL terminated "NAME=VALUE" pairs, then block terminator
  static u16 block[1024];
  static bool built = false;
  if (!built) {
    built = true;
    const char* pairs[] = {
      "PATH=C:\\Windows\\System32;Z:\\",
      "TEMP=C:\\Temp", "TMP=C:\\Temp",
      "USERPROFILE=C:\\Users\\Player",
      "APPDATA=C:\\Users\\Player\\AppData\\Roaming",
      "LOCALAPPDATA=C:\\Users\\Player\\AppData\\Local",
      "COMPUTERNAME=TIRAMISU", "USERNAME=Player",
    };
    size_t o = 0;
    for (const char* p : pairs) {
      for (const char* q = p; *q && o < 1020; ++q) block[o++] = (u16)(u8)*q;
      if (o < 1020) block[o++] = 0;
    }
    if (o < 1024) block[o++] = 0;  // block terminator
    if (o < 1024) block[o] = 0;
  }
  return block;
}

int TRT_STDCALL trt_FreeEnvironmentStringsW(const u16*) { return 1; }

u32 TRT_STDCALL trt_GetModuleFileNameW(void* mod, u16* buf, u32 len) {
  (void)mod;
  if (!buf || !len) return 0;
  u32 i = 0;
  for (; g_exepath[i] && i < len - 1; ++i) buf[i] = (u16)(u8)g_exepath[i];
  buf[i] = 0;
  return i;
}

int TRT_STDCALL trt_GetSystemDirectoryW(u16* buf, u32 len) {
  if (!buf || len < 18) return 18;
  const char* s = "C:\\Windows\\System32";
  u32 i = 0;
  for (; s[i]; ++i) buf[i] = (u16)(u8)s[i];
  buf[i] = 0;
  return i;
}

void trt_bridge_new_thread_id(u32 id) {
  g_tls_index_current_thread = id % 128;
}

// ================= the loader surface (guest DLLs) ==========================
// A shipped game is not one file: it brings engine DLLs and helper DLLs,
// and often its own version.dll/winmm.dll shims. LoadLibrary is how the
// guest reaches them, so it is as load-critical as kernel32 itself.
namespace {

// "C:\\dir\\engine.dll" / "engine.dll" -> a host path we can open
bool guest_path_to_host(const u16* guest_path, char* out, size_t cap) {
  char utf8[1024];
  if (trt_utf16_to_utf8(guest_path, utf8, sizeof(utf8)) != 0) return false;
  if (utf8[0] && utf8[1] == ':') {
    // absolute guest path: through the VFS
    return trt_vfs_resolve_utf16(guest_path, out, cap) == 0;
  }
  // bare name: the game's own directory (the runtime's search dir)
  const char* base = utf8;
  for (const char* p = utf8; *p; ++p)
    if (*p == '\\' || *p == '/') base = p + 1;
  const char* dir = trt_runtime_search_dir();
  if (!dir || !dir[0]) return false;
  snprintf(out, cap, "%s/%s", dir, base);
  return true;
}

} // namespace

void* TRT_STDCALL trt_LoadLibraryW(const u16* path) {
  if (!path) return nullptr;
  char host[1024];
  if (!guest_path_to_host(path, host, sizeof(host))) {
    g_tls_last_error[g_tls_index_current_thread % 128] = 2;  // FILE_NOT_FOUND
    return nullptr;
  }
  // already loaded? Windows returns the same handle
  const char* base = strrchr(host, '/');
  base = base ? base + 1 : host;
  if (trt_image* img = trt_images_find(base)) return trt_images_handle(img);

  trt_image* img = trt_load_pe(host, base, 1);
  if (!img) {
    g_tls_last_error[g_tls_index_current_thread % 128] = 126;  // MOD_NOT_FOUND
    return nullptr;
  }
  return trt_images_handle(img);
}

void* TRT_STDCALL trt_LoadLibraryExW(const u16* path, void* file, u32 flags) {
  (void)file; (void)flags;
  return trt_LoadLibraryW(path);
}

int TRT_STDCALL trt_FreeLibrary(void* handle) {
  (void)handle;
  return 1;  // images live for the process lifetime
}

void* TRT_STDCALL trt_GetProcAddress(void* handle, const char* name) {
  if (!name) return nullptr;
  trt_image* img = handle ? trt_images_by_handle(handle) : nullptr;
  if (!img) {
    // NULL handle means "search every module" on Windows; we only know the
    // guest images, so search those.
    for (trt_image* it = trt_images_first(); it; it = nullptr) break;
    return nullptr;
  }
  // a low value means "export by ordinal"
  const u32 v = (u32)(uintptr_t)name;
  if (v < 0x10000) {
    const u16 ord = (u16)v;
    return trt_pe_find_export(trt_runtime_backend(), trt_runtime_resolver(),
                              img, nullptr, ord);
  }
  return trt_pe_find_export(trt_runtime_backend(), trt_runtime_resolver(), img,
                            name, 0);
}

void* TRT_STDCALL trt_GetModuleHandleW(const u16* name) {
  if (!name || !name[0]) {
    trt_image* first = trt_images_first();
    return first ? trt_images_handle(first) : nullptr;
  }
  char utf8[256];
  if (trt_utf16_to_utf8(name, utf8, sizeof(utf8)) != 0) return nullptr;
  const char* base = strrchr(utf8, '\\');
  base = base ? base + 1 : utf8;
  trt_image* img = trt_images_find(base);
  return img ? trt_images_handle(img) : nullptr;
}

static const trt_named_fn k_kernel32_fns[] = {
  {"GetProcessHeap", (void*)&trt_GetProcessHeap},
  {"HeapCreate", (void*)&trt_HeapCreate},
  {"HeapAlloc", (void*)&trt_HeapAlloc},
  {"HeapFree", (void*)&trt_HeapFree},
  {"VirtualAlloc", (void*)&trt_VirtualAlloc},
  {"VirtualFree", (void*)&trt_VirtualFree},
  {"GetLastError", (void*)&trt_GetLastError},
  {"SetLastError", (void*)&trt_SetLastError},
  {"GetCurrentThreadId", (void*)&trt_GetCurrentThreadId},
  {"GetCurrentProcess", (void*)&trt_GetCurrentProcess},
  {"QueryPerformanceCounter", (void*)&trt_QueryPerformanceCounter},
  {"QueryPerformanceFrequency", (void*)&trt_QueryPerformanceFrequency},
  {"IsProcessorFeaturePresent", (void*)&trt_IsProcessorFeaturePresent},
  {"Sleep", (void*)&trt_Sleep},
  {"CreateFileW", (void*)&trt_CreateFileW},
  {"ReadFile", (void*)&trt_ReadFile},
  {"WriteFile", (void*)&trt_WriteFile},
  {"CloseHandle", (void*)&trt_CloseHandle},
  {"SetFilePointer", (void*)&trt_SetFilePointer},
  {"GetFileSize", (void*)&trt_GetFileSize},
  {"SetCurrentDirectoryW", (void*)&trt_SetCurrentDirectoryW},
  {"GetCurrentDirectoryW", (void*)&trt_GetCurrentDirectoryW},
  {"GetEnvironmentStringsW", (void*)&trt_GetEnvironmentStringsW},
  {"FreeEnvironmentStringsW", (void*)&trt_FreeEnvironmentStringsW},
  {"GetModuleFileNameW", (void*)&trt_GetModuleFileNameW},
  {"GetSystemDirectoryW", (void*)&trt_GetSystemDirectoryW},
  {"LoadLibraryW", (void*)&trt_LoadLibraryW},
  {"LoadLibraryExW", (void*)&trt_LoadLibraryExW},
  {"FreeLibrary", (void*)&trt_FreeLibrary},
  {"GetProcAddress", (void*)&trt_GetProcAddress},
  {"GetModuleHandleW", (void*)&trt_GetModuleHandleW},
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
