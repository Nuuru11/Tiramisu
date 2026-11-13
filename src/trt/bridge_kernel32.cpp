// tiramisu/src/trt/bridge_kernel32.cpp
// Builtin kernel32 — the subset a game boots on, implemented on top of the
// backend (memory) and CRT. Wine semantics, Tiramisu implementation.
// v0 scope: heap/memory/sync-primitive/file-io/clock/error/TLS basics.
#include "trt.h"
#include "thunk.h"
#include "images.h"
#include "apiset.h"
#include "pe_exports.h"
#include "bridge_d3d.h"
#include "bridge_reg.h"
#include "bridge_mem.h"
#include "bridge_thread.h"
#include "bridge_sys.h"
#include "bridge_winmm.h"
#include "bridge_d3d9.h"
#include "bridge_dsound.h"
#include "bridge_ddraw.h"
#include "bridge_d3dx9.h"
#include "bridge_d3d8.h"
#include "bridge_openal.h"
#include "bridge_ole.h"
#include "bridge_shell.h"
#include "bridge_shlwapi.h"
#include "bridge_mmedia.h"
#include "bridge_d3dx.h"
#include "bridge_dinput.h"
#include "bridge_audio.h"
#include "bridge_steam.h"
#include "bridge_comctl.h"
#include "bridge_gdi.h"
#include "bridge_wininet.h"
#include "bridge_psapi.h"
#include "bridge_iphlpapi.h"
#include "bridge_imm32.h"
#include "bridge_crypt32.h"
#include "bridge_dbghelp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if !defined(_WIN32)
#  include <unistd.h>
#  include <time.h>
#endif

namespace {

// ---- heap allocation registry: HeapSize/HeapReAlloc need the byte size of
// every block HeapAlloc handed out. A small pointer->size table is the
// honest runtime answer (Windows tracks this inside the heap).
struct heap_rec { void* p; size_t n; };
heap_rec g_heap_rec[512];
heap_rec* heap_rec_find(void* p) {
  if (!p) return nullptr;
  for (heap_rec& r : g_heap_rec) if (r.p == p) return &r;
  return nullptr;
}
void heap_rec_add(void* p, size_t n) {
  for (heap_rec& r : g_heap_rec)
    if (!r.p) { r.p = p; r.n = n; return; }
}
void heap_rec_del(void* p) {
  if (heap_rec* r = heap_rec_find(p)) { r->p = nullptr; r->n = 0; }
}

// ---- FLS (fiber-local storage): the MSVC CRT allocates FLS indices at
// startup. Honest v0: real indices + real values per slot; the NAMED gap is
// that FlsAlloc callbacks are NOT dispatched on thread exit (the offline
// runtime has no guest thread-exit walk) — memory lives as long as the test.
constexpr int TRT_MAX_FLS = 128;
struct fls_rec { bool used; void* val; };
fls_rec g_fls[TRT_MAX_FLS];

size_t wcslen16(const u16* s) { size_t n = 0; while (s && s[n]) ++n; return n; }

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
  if (p) heap_rec_add(p, (size_t)bytes);  // HeapSize/HeapReAlloc need this
  return p;
}

int TRT_STDCALL trt_HeapFree(void* heap, u32 flags, void* p) {
  (void)heap; (void)flags;
  heap_rec_del(p);  // drop the size record (HeapSize honesty after free)
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
  void* p = g_be.alloc((size_t)(bytes ? bytes : 4096), (u64)(uintptr_t)addr);
  if (!p && addr) {
    // the hint address is unavailable; a reservation's address is advisory
    // for the caller (it stores the returned base) — fall back to any base.
    p = g_be.alloc((size_t)(bytes ? bytes : 4096), 0);
  }
  return p;
}

int TRT_STDCALL trt_VirtualFree(void* addr, u64 bytes, u32 type) {
  // MEM_DECOMMIT (0x4000): decommit keeps the reservation alive; our flat
  // mmap backend commits pages lazily so decommit is an honest no-op.
  if (type == 0x4000) return 1;
  if (type != 0x8000 /*MEM_RELEASE*/ || !addr) return 0;
  g_be.release(addr, bytes);
  return 1;
}

u32 TRT_STDCALL trt_GetLastError(void) { return err_for_current_thread(); }
void TRT_STDCALL trt_SetLastError(u32 err) {
  g_tls_last_error[g_tls_index_current_thread % 128] = err;
}

u32 TRT_STDCALL trt_GetCurrentThreadId(void) { return g_tls_index_current_thread; }

// the monotonic clock games use for timing (GetTickCount64 since Win7)
u64 TRT_STDCALL trt_GetTickCount64(void) {
  const i64 t = now_qpc_ticks();
  // our QPC source is nanoseconds; Windows' GetTickCount64 is milliseconds
  return (u64)(t / 1000000ll);
}

u32 TRT_STDCALL trt_GetTickCount(void) { return (u32)trt_GetTickCount64(); }
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
  // x64 baseline: SSE/SSE2/SSE3/SSSE3/SSE4.1/SSE4.2/POPCNT/NX present.
  // STAGE 7 (OrTMM): 0x17 = PF_FASTFAIL_AVAILABLE — MSVC 2019+ CRT startup
  // queries it before __security_init; returning 0 sends _set_Fflag to
  // __fastfail(FAST_FAIL_FATAL_APP_EXIT) and the EXE dies at CRT-STARTUP
  // with int 0x29 (measured: OrTMM.exe SIGSEGV/UD at image+0xd1d4, the
  // `int $0x29` guarding call IsProcessorFeaturePresent(0x17)).
  // 0x17=PF_FASTFAIL_AVAILABLE, 0x14=PF_ARM_V8... (unused); also name the
  // common x64 extras real CRTs ask about.
  switch (feature) {
    case 0: case 1: case 6: case 7: case 8: case 9: case 10:
    case 0x14: case 0x17: case 0x18:  // 0x17=PF_FASTFAIL_AVAILABLE
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

// ---- address-based waiting (api-ms-win-core-synch-l1-2-0; UnityPlayer's
// bucket allocator waits on the bucket lock word through WaitOnAddress).
// STAGE 7 EXE boot: without these the import binds to a fail-visible thunk
// that returns 0 and the allocator's retry loop unwinds with a NULL block
// that the caller dereferences (measured live: Go Next demo.exe SIGSEGV at
// UnityPlayer+0x16da6a6, vtable call returned 0).
struct wait_addr_state {
  volatile long waiters;
};
static wait_addr_state g_wait_addr[64];

static wait_addr_state* wait_addr_slot(void* addr) {
  return &g_wait_addr[((uintptr_t)addr >> 4) & 63];
}

int TRT_STDCALL trt_WaitOnAddress(volatile void* addr, void* cmp,
                                  u32 size, u32 ms) {
  if (!addr || !cmp || (size != 1 && size != 2 && size != 4 && size != 8))
    return 0;  // FALSE: caller treats as failure and retries
  wait_addr_state* st = wait_addr_slot((void*)addr);
  (void)st;  // single-threaded boot: no true wait primitives needed; poll
  (void)ms;  // callers pass INFINITE or bounded ms; bounded poll below
  u32 waited = 0;
  while (waited < ms) {
    // compare the value at addr against the expected bytes
    long same = 0;
    switch (size) {
      case 1: same = *(volatile u8*)addr == *(u8*)cmp; break;
      case 2: same = *(volatile u16*)addr == *(u16*)cmp; break;
      case 4: same = *(volatile u32*)addr == *(u32*)cmp; break;
      case 8: same = *(volatile u64*)addr == *(u64*)cmp; break;
    }
    if (!same) return 1;  // TRUE: the value changed
#ifdef _WIN32
    ::Sleep(1);  // 1 ms poll
#else
    struct timespec ts{0, 1000000};  // 1 ms poll
    nanosleep(&ts, nullptr);
#endif
    ++waited;
  }
  return 1;  // Win32 semantics: spurious wake is legal; caller rechecks
}

void TRT_STDCALL trt_WakeByAddressSingle(volatile void* addr) {
  (void)addr;  // single-threaded boot: no parked threads to wake
}
void TRT_STDCALL trt_WakeByAddressAll(volatile void* addr) {
  (void)addr;
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
  // the engine-API batch: mapped files, threads, system information
  trt_bridge_register_memory();
  trt_bridge_register_threads();
  trt_bridge_register_system();
  // DirectInput 8: how most PC games read the mouse and keyboard
  trt_bridge_register_dinput();
  // XAudio2: how games play sound
  trt_bridge_register_audio();
  // winmm: the multimedia clock (timeGetTime) and legacy-device probes
  trt_bridge_register_winmm();
  // d3d9: the D3D9 -> D3D11 bridge (M5)
  trt_bridge_register_d3d9();
  // dsound: DirectSound -> the same console audio sink (M6 coverage)
  trt_bridge_register_dsound();
  // ddraw: DirectDraw -> backend present (M6 coverage)
  trt_bridge_register_ddraw();
  // d3dx9: D3DX math/textures/sprites (M6 coverage) — most-versions
  trt_bridge_register_d3dx9();
  // d3d8: the 2001-2004 generation on the same backend hooks (M7)
  trt_bridge_register_d3d8();
  // openal32: the Minecraft-generation audio API onto the same sink
  trt_bridge_register_openal();
  // ole32/oleaut32: COM boot + BSTR/variant/safearray (games call
  // CoInitialize at boot; console UWP ole32 lacks the full PC surface)
  trt_bridge_register_ole();
  // shell32: SHGetFolderPath/KnownFolderPath for save-game dirs
  trt_bridge_register_shell();
  // shlwapi: Path* string/file helpers
  trt_bridge_register_shlwapi();
  // msacm32/msvfw32/winspool: codec + printer boot probes (M6 coverage)
  trt_bridge_register_mmedia();
  // d3dx10/11: texture/shader helpers for the D3D10/11 generation (M7)
  trt_bridge_register_d3dx();
  // Steamworks: AUTH ONLY (no DRM bypass, ever)
  trt_bridge_register_steam();
  // comctl32: common-controls acceptance (OrTMM imports ordinal #380)
  trt_bridge_register_comctl();
  // gdi32: the GDI object/state surface (fonts, DC-coupled state)
  trt_bridge_register_gdi();
  // STAGE 5 boot determinism: the 7 UNCERTAIN console DLLs converted to
  // BUILTIN honest-runtime — the boot no longer depends on unproven
  // console presence (wininet/psapi/iphlpapi/imm32/crypt32/dbghelp/dbgcore)
  trt_bridge_register_wininet();
  trt_bridge_register_psapi();
  trt_bridge_register_iphlpapi();
  trt_bridge_register_imm32();
  trt_bridge_register_crypt32();
  trt_bridge_register_dbghelp();
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

// extern-C accessors for the gap bridge (bridge_kernel32_gap.cpp): the
// handle table lives here; cwd accessor sits next to g_cwd's definition
extern "C" FILE* trt_file_os(void* h) { return file_os(h); }

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

// extern-C accessor for the gap bridge (bridge_kernel32_gap.cpp)
extern "C" char* trt_gap_cwd(void) { return g_cwd; }

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
  // disposition: 1 CREATE_NEW, 2 CREATE_ALWAYS, 3 OPEN_EXISTING,
  // 4 OPEN_ALWAYS, 5 TRUNCATE_EXISTING
  const u32 disp = disposition ? disposition : 3;
  FILE* os = nullptr;
  if (disp == 1 || disp == 2) {
    os = fopen(host, "w+b");
  } else if (disp == 4) {
    os = fopen(host, "r+b");
    if (!os) os = fopen(host, "w+b");
  } else if (disp == 5) {
    os = fopen(host, "w+b");
  } else {
    os = fopen(host, write ? "r+b" : "rb");
  }
  if (!os && write) os = fopen(host, "r+b");
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

// the bytes behind one of the guest's file handles (used by the
// memory-mapping bridge: a mapping must carry the file's content)
void* trt_file_bytes(void* handle, size_t* size) {
  FILE* os = file_os(handle);
  if (!os) return nullptr;
  fflush(os);
  const long cur = ftell(os);
  fseek(os, 0, SEEK_END);
  const long n = ftell(os);
  fseek(os, 0, SEEK_SET);
  u8* buf = (u8*)malloc((size_t)(n > 0 ? n : 1));
  if (!buf) return nullptr;
  const size_t got = fread(buf, 1, (size_t)(n > 0 ? n : 0), os);
  fseek(os, cur, SEEK_SET);
  *size = got;
  return buf;
}

int TRT_STDCALL trt_sync_close(u64 handle);

int TRT_STDCALL trt_CloseHandle(void* h) {
  // a synchronisation object (event/mutex/thread) or a file?
  if (trt_sync_close((u64)(uintptr_t)h)) return 1;
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

// ---- synthetic system-module handles (STAGE 7, OrTMM) ---------------------
// The MSVC CRT startup (__crt_load_kernel32 / ucrt's own boot) calls
// LoadLibraryW("kernel32.dll") and probes api-set contract names
// (api-ms-win-core-synch-l1-2-0, api-ms-win-core-fibers-l1-1-1). On real
// Windows those SUCCEED — every one is loaded in-process. Here they must
// succeed too: a NULL return makes the CRT failfast (int 0x29) and the
// game dies before WinMain (measured: OrTMM.exe SIGSEGV at image+0xd1d4,
// the `int $0x29` guarding IsProcessorFeaturePresent(0x17)).
// The handle for a system module is a synthetic tag the image registry
// already understands (trt_images_by_handle); GetProcAddress on it resolves
// through the module table — the SAME surface the static import binder
// used, so the answers are identical.
namespace {
void* system_module_handle(const char* utf8_name) {
  if (!utf8_name || !utf8_name[0]) return nullptr;
  const char* base = strrchr(utf8_name, '\\');
  base = base ? base + 1 : utf8_name;
  // a guest image with this name wins (Windows: the loaded module list)
  if (trt_images_find(base)) return nullptr;  // caller falls through to images
  // LoadLibrary appends ".dll" when the name has no extension; the module
  // table registers names WITH the extension.
  char with_ext[96];
  if (!strchr(base, '.')) {
    snprintf(with_ext, sizeof(with_ext), "%s.dll", base);
    base = with_ext;
  }
  // api-set contract names resolve to their implementation module first
  if (const char* target = trt_apiset_target(base)) base = target;
  if (trt_module_route(base) == TRT_ROUTE_MISSING) return nullptr;
  void* h = trt_images_synthetic_handle(base);
  if (h) {
    char line[224];
    snprintf(line, sizeof(line),
             "kernel32: LoadLibrary/GetModuleHandle(%s) -> synthetic handle "
             "(system module, offline bridge surface)", base);
    trt_core_log(line);
  }
  return h;
}
} // namespace

void* TRT_STDCALL trt_LoadLibraryW(const u16* path) {
  if (!path) return nullptr;
  char host[1024];
  if (!guest_path_to_host(path, host, sizeof(host))) {
    // STAGE 7 (OrTMM): not a game-dir file — is it a known SYSTEM module
    // name ("kernel32", an api-set contract, "d3d11", ...)? Those load
    // fine on real Windows; a NULL here failfasts the CRT startup.
    char utf8[256];
    if (trt_utf16_to_utf8(path, utf8, sizeof(utf8)) == 0) {
      if (void* h = system_module_handle(utf8)) return h;
    }
    g_tls_last_error[g_tls_index_current_thread % 128] = 2;  // FILE_NOT_FOUND
    return nullptr;
  }
  // already loaded? Windows returns the same handle
  const char* base = strrchr(host, '/');
  base = base ? base + 1 : host;
  if (trt_image* img = trt_images_find(base)) return trt_images_handle(img);

  trt_image* img = trt_load_pe(host, base, 1);
  if (!img) {
    // STAGE 7 (dstoragecore): the game-dir file does not exist — a bare
    // system-module name ("advapi32", "kernelbase", ...) with no extension
    // resolves to <gamedir>/<name> above, which is exactly how Windows
    // treats bare names TOO (app dir first, then the system dirs). The
    // app-dir miss must fall through to the system surface: returning NULL
    // here failfasts the caller (measured: dstoragecore.dll int $0x29(7)
    // from its obfuscation runtime after LoadLibraryExW("advapi32") -> NULL
    // while lazy-resolving SystemFunction036).
    char utf8[256];
    if (trt_utf16_to_utf8(path, utf8, sizeof(utf8)) == 0) {
      if (void* h = system_module_handle(utf8)) return h;
    }
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
  // STAGE 7 (OrTMM): a synthetic system-module handle holds no PE image —
  // resolve through the module table, the SAME surface the static import
  // binder used (kernel32!RtlCaptureContext etc. live there, not in a file).
  if (handle && trt_images_is_synthetic(handle)) {
    const char* mod = trt_images_synth_name(handle);
    if (!mod) return nullptr;
    const u32 v = (u32)(uintptr_t)name;
    if (v >= 0x10000) {
      if (void* fn = trt_resolve_module(nullptr, mod, name, 0)) return fn;
      char line[256];
      snprintf(line, sizeof(line),
               "kernel32: GetProcAddress(%s!%s) -> NULL (not in the bridge "
               "surface — named gap, never a fake success)", mod, name);
      trt_core_log(line);
    }
    return nullptr;  // ordinal on a synthetic module: named gap, honest NULL
  }
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
  if (img) return trt_images_handle(img);
  // STAGE 7 (OrTMM): the CRT boot asks for "kernel32.dll" (and apiset
  // contract names) by handle before any file exists for them. A known
  // SYSTEM/BUILTIN module name gets the synthetic handle — a NULL here
  // failfasts the CRT before WinMain.
  return system_module_handle(base);
}

// ==================== console + child-process honesty =======================
// OrTMM (and every MSVC-runtime game) imports the console surface. The
// console has NO console for a UWP AppContainer process: everything here
// must say so HONESTLY (NULL/FALSE + a named last error), never a fake
// success the CRT would then trust.
//
// CreateProcessW: the console blocks child processes for game PEs
// (documented in docs/02-bridge-surface.md) — fail named with
// ERROR_ACCESS_DENIED.

namespace {

void console_log(const char* s) { trt_core_log(s); }

void console_no_console(const char* api, u32 err) {
  char line[128];
  snprintf(line, sizeof(line),
           "kernel32: %s -> no console (AppContainer); lasterr=%u", api, err);
  console_log(line);
  g_tls_last_error[g_tls_index_current_thread % 128] = err;
}

} // namespace

void* TRT_GUEST_ABI trt_GetStdHandle(u32 handle_id) {
  (void)handle_id;  // STD_INPUT/OUTPUT/ERROR all equally absent
  console_no_console("GetStdHandle", 6 /*ERROR_INVALID_HANDLE*/);
  return nullptr;
}

int TRT_GUEST_ABI trt_SetStdHandle(u32 handle_id, void* handle) {
  (void)handle_id; (void)handle;
  console_no_console("SetStdHandle", 6);
  return 0;  // FALSE
}

int TRT_GUEST_ABI trt_GetConsoleMode(void* console, u32* mode_out) {
  (void)console; (void)mode_out;
  console_no_console("GetConsoleMode", 6);
  return 0;  // FALSE
}

int TRT_GUEST_ABI trt_ReadConsoleW(void* console, void* buffer, u32 to_read,
                                  u32* read_out, void* ctrl) {
  (void)console; (void)buffer; (void)to_read; (void)read_out; (void)ctrl;
  console_no_console("ReadConsoleW", 6);
  return 0;  // FALSE
}

int TRT_GUEST_ABI trt_WriteConsoleW(void* console, const void* buffer, u32 to_write,
                                   u32* written_out, void* reserved) {
  (void)console; (void)buffer; (void)to_write; (void)written_out; (void)reserved;
  console_no_console("WriteConsoleW", 6);
  return 0;  // FALSE
}

int TRT_GUEST_ABI trt_SetConsoleCtrlHandler(void* handler, int add) {
  (void)handler; (void)add;
  console_no_console("SetConsoleCtrlHandler", 120 /*ERROR_CALL_NOT_IMPLEMENTED*/);
  return 0;  // FALSE: there is no console to attach a handler to
}

int TRT_GUEST_ABI trt_CreateProcessW(const u16* app, u16* cmdline, void* proc_attr,
                                    void* thread_attr, int inherit,
                                    u32 flags, void* env, const u16* dir,
                                    const void* startup, void* info_out) {
  char utf8[256];
  if (cmdline)
    trt_utf16_to_utf8(cmdline, utf8, sizeof(utf8));
  else if (app)
    trt_utf16_to_utf8(app, utf8, sizeof(utf8));
  else
    snprintf(utf8, sizeof(utf8), "(null)");
  char line[256];
  snprintf(line, sizeof(line),
           "kernel32: CreateProcessW('%s') -> ERROR_ACCESS_DENIED "
           "(console blocks child processes; documented)", utf8);
  console_log(line);
  g_tls_last_error[g_tls_index_current_thread % 128] = 5; // ACCESS_DENIED
  return 0;  // FALSE
}

// ==================== STAGE 7: EXE boot / MSVC CRT surface ==================
// A game EXE is not a DLL: before WinMain its CRT startup runs and calls a
// kernel32 surface the DLL corpus never touched (process/heap/FLS/SEH).
// Every answer here is honest-minimal and NAMED, never silent.

// process-exit trap hooks: NULL on the console (real exit), the offline EXE
// boot harness sets them to longjmp back into the harness. C-linkage globals
// (declared in trt.h) so the harness can install them.
void (*trt_boot_exit_trap)(int code) = nullptr;
void (*trt_boot_raise_trap)(u32 code) = nullptr;

namespace {

// STAGE 7 named depth marker: the boot harness greps the ring for these.
void boot_log(const char* s) { trt_core_log(s); }

// GetStartupInfoW must FILL the STARTUPINFOW (the CRT reads dwY/dwXCountChars
// etc. to decide console dimensions; a zeroed struct is a valid "no
// console" answer, cb must be sizeof(STARTUPINFOW) = 104 on x64).
void fill_startup_info_w(void* si) {
  if (!si) return;
  memset(si, 0, 104);
  ((u32*)si)[0] = 104;  // cb
  boot_log("kernel32: GetStartupInfoW -> zeroed STARTUPINFOW (no console)");
}

void fill_startup_info_a(void* si) {
  fill_startup_info_w(si);
  boot_log("kernel32: GetStartupInfoA -> zeroed STARTUPINFOA (no console)");
}

// FindFirstFileExW / FindNextFileW / FindClose over an EMPTY directory set:
// the CRT only calls these for wildcard CRT file walks. INVALID_HANDLE_VALUE
// + ERROR_FILE_NOT_FOUND is the honest answer for "no matches".
void* find_first_file(const u16* name, void* found) {
  char utf8[512];
  if (name) trt_utf16_to_utf8(name, utf8, sizeof(utf8));
  else snprintf(utf8, sizeof(utf8), "(null)");
  char line[640];
  snprintf(line, sizeof(line),
           "kernel32: FindFirstFileExW('%s') -> INVALID_HANDLE_VALUE "
           "(empty VFS dir set; lasterr=2)", utf8);
  boot_log(line);
  (void)found;
  g_tls_last_error[g_tls_index_current_thread % 128] = 2;  // FILE_NOT_FOUND
  return (void*)(uintptr_t)-1;  // INVALID_HANDLE_VALUE
}

} // namespace

// ExitProcess: REAL process-termination semantics. On the console (host
// build) there is genuinely nothing after this call. In the offline boot
// harness the trap hook longjmps back to the harness so the run continues.
// NAMED either way — "BOOT EXITED" is the harness's report.
void TRT_STDCALL trt_ExitProcess(u32 code) {
  char line[128];
  snprintf(line, sizeof(line), "kernel32: ExitProcess(%u) called", code);
  boot_log(line);
  if (trt_boot_exit_trap) { trt_boot_exit_trap((int)code); }
  // console path: a UWP process really does end here
#ifdef _WIN32
  ::ExitProcess(code);
#else
  _exit((int)(code & 0xFF));
#endif
}

int TRT_STDCALL trt_TerminateProcess(void* proc, u32 code) {
  (void)proc;
  char line[128];
  snprintf(line, sizeof(line), "kernel32: TerminateProcess(%u) called", code);
  boot_log(line);
  if (trt_boot_exit_trap) { trt_boot_exit_trap((int)code); }
#ifdef _WIN32
  ::TerminateProcess(::GetCurrentProcess(), code);
#else
  _exit((int)(code & 0xFF));
#endif
  return 1;
}

void* TRT_STDCALL trt_GetStartupInfoW(void* si) {
  fill_startup_info_w(si);
  return si;  // void WINAPI GetStartupInfoW: returns nothing (0 in rax)
}

void* TRT_STDCALL trt_GetStartupInfoA(void* si) {
  fill_startup_info_a(si);
  return si;
}

const char* TRT_STDCALL trt_GetCommandLineA(void) {
  return "\"Z:\\game.exe\"";  // argv[0], quoted like Windows does
}

const u16* TRT_STDCALL trt_GetCommandLineW(void) {
  static u16 cmd[32];
  static bool built = false;
  if (!built) {
    built = true;
    const char* s = "Z:\\game.exe";
    u32 i = 0;
    cmd[i++] = '"';
    for (const char* p = s; *p; ++p) cmd[i++] = (u16)(u8)*p;
    cmd[i++] = '"';
    cmd[i] = 0;
  }
  return cmd;
}

// ---- heap registry integration (HeapSize / HeapReAlloc) ----
u64 TRT_STDCALL trt_HeapSize(void* heap, u32 flags, void* p) {
  (void)heap; (void)flags;
  if (heap_rec* r = heap_rec_find(p)) return (u64)r->n;
  // not ours: Windows would consult the heap; honest answer is 0 + named log
  boot_log("kernel32: HeapSize on a block HeapAlloc did not record -> 0");
  return 0;
}

void* TRT_STDCALL trt_HeapReAlloc(void* heap, u32 flags, void* p, u64 bytes) {
  (void)heap;
  if (!p) return trt_HeapAlloc(heap, flags, bytes);
  const heap_rec* r = heap_rec_find(p);
  const size_t old = r ? r->n : 0;
  u8* np = (u8*)g_be.alloc((size_t)(bytes ? bytes : 1), 0);
  if (!np) { g_tls_last_error[g_tls_index_current_thread % 128] = 8; return nullptr; }
  if (flags & 0x8) memset(np, 0, (size_t)bytes);
  else if (old) memcpy(np, p, old < bytes ? old : (size_t)bytes);
  heap_rec_add(np, (size_t)bytes);
  if (p) g_be.release(p, 0);
  return np;
}

u64 TRT_STDCALL trt_HeapQueryInformation(void*, u32, void*, void*, u64, u64*) {
  return 0;  // HeapInformation classes are not implemented: honest 0
}

// ---- FLS (fiber-local storage; the MSVC CRT's fiber slots) ----
// FlsGetValue2 is a real kernel32 export (Win8+): the callback-free FLS read
// the UCRT's per-thread locale storage uses. Measured live (OrTMM boot): the
// UCRT resolves it BY NAME through the synthetic kernel32 handle
// (lea r9,[.rdata "FlsGetValue2"] -> GetProcAddress path) and its locale
// getter at image+0x1b248 retries HeapAlloc(968) forever when the read stays
// NULL -> the 716k-mmap alloc storm. Same slot semantics as FlsGetValue.
void* TRT_STDCALL trt_FlsGetValue(u32 idx);  // defined below

void* TRT_STDCALL trt_FlsGetValue2(u32 idx) {
  return trt_FlsGetValue(idx);
}

u32 TRT_STDCALL trt_FlsAlloc(void* cb) {
  (void)cb;  // callback dispatch on thread exit is NOT implemented (named in
             // the header comment on g_fls)
  for (int i = 0; i < TRT_MAX_FLS; ++i)
    if (!g_fls[i].used) { g_fls[i].used = true; g_fls[i].val = nullptr; return (u32)i + 1; }
  boot_log("kernel32: FlsAlloc table full -> FLS_OUT_OF_INDEXES");
  return 0xFFFFFFFFu;  // FLS_OUT_OF_INDEXES
}

void* TRT_STDCALL trt_FlsGetValue(u32 idx) {
  if (idx == 0 || idx > TRT_MAX_FLS || !g_fls[idx - 1].used) return nullptr;
  return g_fls[idx - 1].val;
}

int TRT_STDCALL trt_FlsSetValue(u32 idx, void* val) {
  if (idx == 0 || idx > TRT_MAX_FLS || !g_fls[idx - 1].used) return 0;
  g_fls[idx - 1].val = val;
  return 1;
}

int TRT_STDCALL trt_FlsFree(u32 idx) {
  if (idx == 0 || idx > TRT_MAX_FLS || !g_fls[idx - 1].used) return 0;
  g_fls[idx - 1].used = false;
  g_fls[idx - 1].val = nullptr;
  return 1;
}

// ---- code pages / string type / locale-ish strings (CRT init probes) ----
int TRT_STDCALL trt_GetCPInfo(u32 codepage, void* cpinfo) {
  if (!cpinfo) return 0;
  // Win64 CPINFO is 4+2+12=18 bytes (+2 pad => sizeof 20; winnls.h:
  // MAX_DEFAULTCHAR=2, MAX_LEADBYTES=12 — NOT 126; 126 is a misread of the
  // MAX_LEADBYTES=12 — NOT 126; 126 is a misread of the lead-RANGE byte
  // pairs). Writing more here corrupts the caller's stack: the MSVC CRT
  // passes a frame-local CPINFO with its /GS cookie 0x18 bytes past the
  // buffer start, and a 132-byte memset zeroed that cookie — every corpus
  // DLL + OrTMM died in __raise_securityfailure (int 0x29) at CRT locale
  // init from exactly this overwrite (pinned live under gdb: watchpoint on
  // the cookie slot fired inside this bridge at the 0x44-wide movups run).
  struct cpinfo_x { u32 max_size; u8 default_char[2]; u8 lead[12]; };
  static_assert(sizeof(cpinfo_x) == 20, "CPINFO is 18 bytes + 2 pad on x64");
  cpinfo_x* ci = (cpinfo_x*)cpinfo;
  memset(ci, 0, sizeof(cpinfo_x));
  ci->max_size = codepage == 65001 ? 4 : (codepage == 0 || codepage == 1252 ? 1 : 2);
  boot_log("kernel32: GetCPInfo answered (single/double-byte by codepage)");
  return 1;
}

u32 TRT_STDCALL trt_GetConsoleOutputCP(void) { return 437; }

int TRT_STDCALL trt_GetStringTypeW(u32 type, const u16* src, int count,
                                   u16* types) {
  (void)type;
  if (!src || count <= 0 || !types) return 0;
  for (int i = 0; i < count; ++i) {
    const u16 c = src[i];
    u16 t = 0;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) t = 0x0001;  // C1_UPPER|LOWER approx
    else if (c >= '0' && c <= '9') t = 0x0004;                          // C1_DIGIT
    else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') t = 0x0008;  // C1_SPACE
    else t = 0x0100;                                                     // C1_PUNCT-ish
    types[i] = t;
  }
  return 1;
}

int TRT_STDCALL trt_LCMapStringW(u32 lcid, u32 flags, const u16* src, int srclen,
                                 u16* dst, int dstlen) {
  (void)lcid;
  if (!src || (srclen < 0 && srclen != -1)) return 0;
  const int n = srclen == -1 ? (int)wcslen16(src) : srclen;
  if (!dst || dstlen <= 0) return n + 1;
  int i = 0;
  for (; i < n && i < dstlen - 1; ++i) {
    u16 c = src[i];
    if (flags & 0x200 /*LCMAP_LOWERCASE*/ && c >= 'A' && c <= 'Z') c += 32;
    if (flags & 0x100 /*LCMAP_UPPERCASE*/ && c >= 'a' && c <= 'z') c -= 32;
    dst[i] = c;
  }
  dst[i] = 0;
  return i;
}

int TRT_STDCALL trt_CompareStringW(u32 lcid, u32 flags, const u16* a, int alen,
                                   const u16* b, int blen) {
  (void)lcid; (void)flags;
  const int na = alen == -1 ? (int)wcslen16(a) : alen;
  const int nb = blen == -1 ? (int)wcslen16(b) : blen;
  for (int i = 0; i < na && i < nb; ++i)
    if (a[i] != b[i]) return a[i] < b[i] ? 1 : 3;  // CSTR_LESS_THAN / GREATER
  if (na == nb) return 2;                          // CSTR_EQUAL
  return na < nb ? 1 : 3;
}

int TRT_STDCALL trt_CompareStringEx(u32, u32 flags, const u16* a, int alen,
                                    const u16* b, int blen, void*, void*, void*) {
  return trt_CompareStringW(0, flags, a, alen, b, blen);
}

int TRT_STDCALL trt_LCMapStringEx(u32, u32 flags, const u16* src, int srclen,
                                  u16* dst, int dstlen, void*, void*, void*) {
  return trt_LCMapStringW(0, flags, src, srclen, dst, dstlen);
}

// ---- SEH / unwind family: the CRT registers a filter and RtlUnwind walks
// frames. Guest images carry .pdata (the loader records the exception
// directory per image), so CaptureContext/LookupFunctionEntry give the
// caller real register state and real RUNTIME_FUNCTION entries. Full
// RtlVirtualUnwind prologue emulation is still not implemented (named 0);
// RaiseException in the boot harness is fatal (trap hook) — on the console
// the real RtlRaiseException applies.
#include <setjmp.h>

// x64 CONTEXT layout offsets we fill (only what guests actually read):
// Rip=0xF8 Rsp=0x98 Rbp=0xA0 Rax=0x78 Rbx=0x90 Rcx=0x80 Rdx=0x88
// Rsi=0xB0 Rdi=0xB8 R8..R15=0xB8+8*n (R8=0xB8..R15=0xF0), CsfFlags/Rflags=0x44
void* TRT_STDCALL trt_RtlCaptureContext(void* ctx) {
  u8* c = (u8*)ctx;
  if (!c) return ctx;
  memset(c, 0, 0x4d0);  // sizeof(CONTEXT) x64
  // CaptureContext runs in the caller's frame: Rip is the return address
  // (the instruction after the call), Rsp points just past it.
  u64* rip = (u64*)(c + 0xF8);
  *rip = (u64)__builtin_return_address(0);
  *(u64*)(c + 0x98) = (u64)__builtin_frame_address(0) + 8;
  *(u64*)(c + 0xA0) = (u64)__builtin_frame_address(0);
  *(u32*)(c + 0x44) = 0x10000B;  // CONTEXT_CONTROL|INTEGER|SEGMENT flags
  char line[128];
  snprintf(line, sizeof(line), "kernel32: RtlCaptureContext -> rip=0x%llx",
           (unsigned long long)*rip);
  boot_log(line);
  return ctx;
}

// RUNTIME_FUNCTION: 3×u32 (BeginRva, EndRva, UnwindRva)
typedef struct trt_runtime_fn {
  u32 begin, end, unwind;
} trt_runtime_fn;

// Walk every loaded guest image's exception directory (.pdata) and return
// the RUNTIME_FUNCTION covering ctrlpc (base_out = image base), exactly what
// RtlLookupFunctionEntry does on Windows for code with unwind info.
void* TRT_STDCALL trt_RtlLookupFunctionEntry(u64 ctrlpc, u64* base_out,
                                             void* history) {
  (void)history;
  if (base_out) *base_out = 0;
  if (!ctrlpc) return nullptr;
  for (int i = 0, n = trt_images_count(); i < n; ++i) {
    trt_image* img = trt_images_nth(i);
    if (!img || !img->base || !img->pdata || !img->pdata_count) continue;
    const u64 base = (u64)(uintptr_t)img->base;
    if (ctrlpc < base || ctrlpc >= base + img->size_of_image) continue;
    // binary search the sorted RUNTIME_FUNCTION table
    u32 lo = 0, hi = img->pdata_count;
    const trt_runtime_fn* fns = (const trt_runtime_fn*)img->pdata;
    while (lo < hi) {
      const u32 mid = (lo + hi) / 2;
      const u64 fn_start = base + fns[mid].begin;
      const u64 fn_end = base + fns[mid].end;
      if (ctrlpc < fn_start) { hi = mid; }
      else if (ctrlpc >= fn_end) { lo = mid + 1; }
      else {
        if (base_out) *base_out = base;
        return (void*)&fns[mid];
      }
    }
    // inside the image but no unwind entry: real Windows still answers with
    // the image base so callers can compute RVAs
    if (base_out) *base_out = base;
    return nullptr;
  }
  return nullptr;
}

u64 TRT_STDCALL trt_RtlVirtualUnwind(u32, u64, u64, void*, void*, void*, u64*) {
  return 0;  // no frame walked (prologue emulation not implemented — named)
}

void* TRT_STDCALL trt_RtlPcToFileHeader(u64 pc, u64* base_out) {
  if (base_out) *base_out = 0;
  return nullptr;  // pc is not in any known image (guest images are unknown
                   // to the host unwinder in this offline model)
}

u64 TRT_STDCALL trt_RtlUnwindEx(u64 target, u64 frame, void* ctx, u64) {
  boot_log("kernel32: RtlUnwindEx reached -> SEH unwind NOT walked (named)");
  (void)target; (void)frame; (void)ctx;
  return 0;
}

u64 TRT_STDCALL trt_RtlUnwind(u64 target, void* frame, void* ctx, u64) {
  boot_log("kernel32: RtlUnwind reached -> SEH unwind NOT walked (named)");
  (void)target; (void)frame; (void)ctx;
  return 0;
}

// RaiseException: fatal in the boot harness (trap hook longjmps), named here.
void TRT_STDCALL trt_RaiseException(u32 code, u32 flags, u32 count, u64* args) {
  char line[192];
  snprintf(line, sizeof(line),
           "kernel32: RaiseException(code=0x%08x flags=%u count=%u arg0=0x%llx)",
           code, flags, count, count ? (unsigned long long)args[0] : 0ull);
  boot_log(line);
  if (trt_boot_raise_trap) { trt_boot_raise_trap(code); }
  // no trap installed: the exception is unhandled and the process ends
  boot_log("kernel32: RaiseException unhandled -> terminating");
#ifdef _WIN32
  ::RaiseException(code, flags, count, (const ULONG_PTR*)(uintptr_t)args);
#else
  _exit(42);
#endif
}

void* TRT_STDCALL trt_SetUnhandledExceptionFilter(void* filter) {
  (void)filter;
  boot_log("kernel32: SetUnhandledExceptionFilter registered (records only; "
           "the offline runtime does not walk guest SEH)");
  return nullptr;  // the previous filter: none
}

void* TRT_STDCALL trt_UnhandledExceptionFilter(void* info) {
  (void)info;
  boot_log("kernel32: UnhandledExceptionFilter -> EXCEPTION_CONTINUE_SEARCH "
           "(no console filter for guest exceptions)");
  return (void*)(uintptr_t)0;  // EXCEPTION_CONTINUE_SEARCH
}

int TRT_STDCALL trt_IsDebuggerPresent(void) { return 0; }

void* TRT_STDCALL trt_EncodePointer(void* p) {
  // pointer obfuscation: identity is a legal (if weak) encoder — the value
  // round-trips through DecodePointer, which is all the CRT requires
  return p;
}

void* TRT_STDCALL trt_DecodePointer(void* p) { return p; }

int TRT_STDCALL trt_InitializeSListHead(void* head) {
  if (!head) return 0;
  memset(head, 0, 16);  // SLIST_HEADER x64: depth(4)+sequence(4)+next(8)
  return 1;
}

int TRT_STDCALL trt_InitializeCriticalSectionAndSpinCount(void* cs, u32 spin) {
  if (!cs) return 0;
  memset(cs, 0, 40);  // RTL_CRITICAL_SECTION x64
  ((u32*)cs)[3] = spin;
  return 1;
}

int TRT_STDCALL trt_InitializeCriticalSectionEx(void* cs, u32 spin, u32 flags) {
  (void)flags;
  return trt_InitializeCriticalSectionAndSpinCount(cs, spin);
}

// ---- SRW locks + condition variables (UnityCrashHandler64 CRT/threads) ----
void* TRT_STDCALL trt_AcquireSRWLockExclusive(void* srw) {
  if (srw) ((u64*)srw)[0] = 1;  // locked marker (single guest thread model)
  return srw;
}

void* TRT_STDCALL trt_ReleaseSRWLockExclusive(void* srw) {
  if (srw) ((u64*)srw)[0] = 0;
  return srw;
}

void* TRT_STDCALL trt_TryAcquireSRWLockExclusive(void* srw) {
  u64* p = (u64*)srw;
  if (p && *p == 0) { *p = 1; return (void*)(uintptr_t)1; }
  return (void*)(uintptr_t)0;
}

int TRT_STDCALL trt_SleepConditionVariableSRW(void* cv, void* srw, u32 ms,
                                              u32 flags) {
  (void)cv; (void)flags;
  trt_AcquireSRWLockExclusive(srw);
  if (ms == 0xFFFFFFFFu) {
    // infinite: with no waker the honest named answer is a timeout-fail
    boot_log("kernel32: SleepConditionVariableSRW(INFINITE) -> no waker, "
             "returns FALSE (lasterr=146)");
    g_tls_last_error[g_tls_index_current_thread % 128] = 146;
    return 0;
  }
  trt_Sleep(ms);
  return 0;
}

void TRT_STDCALL trt_WakeAllConditionVariable(void* cv) { (void)cv; }
void TRT_STDCALL trt_WakeConditionVariable(void* cv) { (void)cv; }

// ---- file surface extensions the CRT init uses ----
void* TRT_STDCALL trt_FindFirstFileExW(const u16* name, void* info, u32 flags) {
  (void)info; (void)flags;
  return find_first_file(name, nullptr);
}

int TRT_STDCALL trt_FindNextFileW(void* handle, void* info) {
  (void)handle; (void)info;
  g_tls_last_error[g_tls_index_current_thread % 128] = 2;  // FILE_NOT_FOUND
  return 0;  // no more files
}

int TRT_STDCALL trt_FindClose(void* handle) {
  (void)handle;
  return 1;
}

void* TRT_STDCALL trt_FindFirstFileW(const u16* name, void* info) {
  (void)info;
  return find_first_file(name, nullptr);
}

int TRT_STDCALL trt_GetFileAttributesExW(const u16* name, u32 level, void* info) {
  if (!name || !info) return 0;
  char host[1024];
  if (trt_vfs_resolve_utf16(name, host, sizeof(host)) != 0) {
    g_tls_last_error[g_tls_index_current_thread % 128] = 2;
    return 0;
  }
  (void)level;
  // WIN32_FILE_ATTRIBUTE_DATA: attributes, times (8+8+8), size hi+lo
  u32* d = (u32*)info;
  memset(d, 0, 36);
  d[0] = 0x80;  // FILE_ATTRIBUTE_NORMAL
  FILE* f = fopen(host, "rb");
  if (f) {
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fclose(f);
    d[9] = (u32)sz;          // nFileSizeLow
    d[10] = 0;               // nFileSizeHigh
  }
  return 1;
}

int TRT_STDCALL trt_SetFilePointerEx(void* h, i64 dist, i64* new_pos, u32 method) {
  FILE* os = file_os(h);
  if (!os) return 0;
  const int whence = method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END;
  if (fseek(os, (long)dist, whence)) return 0;
  if (new_pos) *new_pos = (i64)ftell(os);
  return 1;
}

int TRT_STDCALL trt_FlushFileBuffers(void* h) {
  FILE* os = file_os(h);
  if (!os) return 0;
  return fflush(os) == 0 ? 1 : 0;
}

u32 TRT_STDCALL trt_GetFileType(void* h) {
  // FILE_TYPE_DISK = 1 (every VFS handle is a disk file; no console/pipe)
  return file_os(h) ? 1u : 0u;
}

u32 TRT_STDCALL trt_GetCurrentProcessId(void) { return 4242; }
u32 TRT_STDCALL trt_GetProcessId(void* proc) {
  (void)proc;
  return 4242;
}

int TRT_STDCALL trt_GetModuleHandleExW(u32 flags, const u16* name, void** out) {
  (void)flags;
  if (!out) return 0;
  *out = name ? trt_GetModuleHandleW(name) : nullptr;
  return *out ? 1 : 0;
}

void* TRT_STDCALL trt_GetModuleHandleA(const char* name) {
  if (!name) return trt_GetModuleHandleW(nullptr);
  u16 w[256];
  u32 i = 0;
  for (; name[i] && i < 255; ++i) w[i] = (u16)(u8)name[i];
  w[i] = 0;
  return trt_GetModuleHandleW(w);
}

// GetSystemTime = SYSTEMTIME (8 words); the CRT only compares it, a real
// conversion of the same clock is the honest answer
int TRT_STDCALL trt_GetSystemTimeAsFileTime(u64* out) {
  if (!out) return 0;
  // Unix epoch -> FILETIME (1601) : +11644473600 seconds
#ifdef _WIN32
  FILETIME ftnow;
  ::GetSystemTimeAsFileTime(&ftnow);
  *out = ((u64)ftnow.dwHighDateTime << 32) | ftnow.dwLowDateTime;
  return 1;
#else
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  i64 ft = ((i64)ts.tv_sec + 11644473600ll) * 10000000ll + ts.tv_nsec / 100;
  *out = (u64)ft;
  return 1;
#endif
}

void TRT_STDCALL trt_GetSystemTime(u16* st) {
  if (!st) return;
  u64 ft = 0;
  trt_GetSystemTimeAsFileTime(&ft);
  // FILETIME -> SYSTEMTIME (days since 1601 -> y/m/d h:m:s.ms)
  i64 t = (i64)ft / 10000000ll;                    // whole seconds
  u16 ms = (u16)(((i64)ft % 10000000ll) / 10000);
  i64 days = t / 86400; i64 secs = t % 86400;
  // civil-from-days (Howard Hinnant's algorithm)
  days += 719468;
  i64 era = (days >= 0 ? days : days - 146096) / 146097;
  i64 doe = days - era * 146097;
  i64 yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
  i64 y = yoe + era * 400;
  i64 doy = doe - (365*yoe + yoe/4 - yoe/100);
  i64 mp = (5*doy + 2)/153;
  i64 d = doy - (153*mp+2)/5 + 1;
  i64 m = mp < 10 ? mp+3 : mp-9;
  y += (m <= 2);
  u16 w[8];
  w[0] = (u16)y; w[1] = (u16)m; w[2] = (u16)(d - 1) % 7 + 0; w[3] = (u16)d;
  w[4] = (u16)(secs/3600); w[5] = (u16)((secs%3600)/60); w[6] = (u16)(secs%60);
  w[7] = ms;
  for (int i = 0; i < 8; ++i) st[i] = w[i];
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
  {"GetTickCount", (void*)&trt_GetTickCount},
  {"GetTickCount64", (void*)&trt_GetTickCount64},
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
  // the console/child-process honesty surface (OrTMM's CRT imports)
  {"GetStdHandle", (void*)&trt_GetStdHandle},
  {"SetStdHandle", (void*)&trt_SetStdHandle},
  {"GetConsoleMode", (void*)&trt_GetConsoleMode},
  {"ReadConsoleW", (void*)&trt_ReadConsoleW},
  {"WriteConsoleW", (void*)&trt_WriteConsoleW},
  {"SetConsoleCtrlHandler", (void*)&trt_SetConsoleCtrlHandler},
  {"CreateProcessW", (void*)&trt_CreateProcessW},
  // ---- STAGE 7: EXE boot / MSVC CRT surface ----
  {"ExitProcess", (void*)&trt_ExitProcess},
  {"TerminateProcess", (void*)&trt_TerminateProcess},
  {"GetStartupInfoW", (void*)&trt_GetStartupInfoW},
  {"GetStartupInfoA", (void*)&trt_GetStartupInfoA},
  {"GetCommandLineA", (void*)&trt_GetCommandLineA},
  {"GetCommandLineW", (void*)&trt_GetCommandLineW},
  {"HeapSize", (void*)&trt_HeapSize},
  {"HeapReAlloc", (void*)&trt_HeapReAlloc},
  {"HeapQueryInformation", (void*)&trt_HeapQueryInformation},
  {"FlsAlloc", (void*)&trt_FlsAlloc},
  {"FlsGetValue", (void*)&trt_FlsGetValue},
  {"FlsGetValue2", (void*)&trt_FlsGetValue2},
  {"FlsSetValue", (void*)&trt_FlsSetValue},
  {"FlsFree", (void*)&trt_FlsFree},
  {"GetCPInfo", (void*)&trt_GetCPInfo},
  {"GetConsoleOutputCP", (void*)&trt_GetConsoleOutputCP},
  {"GetStringTypeW", (void*)&trt_GetStringTypeW},
  {"LCMapStringW", (void*)&trt_LCMapStringW},
  {"LCMapStringEx", (void*)&trt_LCMapStringEx},
  {"CompareStringW", (void*)&trt_CompareStringW},
  {"CompareStringEx", (void*)&trt_CompareStringEx},
  {"RtlCaptureContext", (void*)&trt_RtlCaptureContext},
  {"RtlLookupFunctionEntry", (void*)&trt_RtlLookupFunctionEntry},
  {"RtlVirtualUnwind", (void*)&trt_RtlVirtualUnwind},
  {"RtlPcToFileHeader", (void*)&trt_RtlPcToFileHeader},
  {"RtlUnwind", (void*)&trt_RtlUnwind},
  {"RtlUnwindEx", (void*)&trt_RtlUnwindEx},
  {"RaiseException", (void*)&trt_RaiseException},
  {"SetUnhandledExceptionFilter", (void*)&trt_SetUnhandledExceptionFilter},
  {"UnhandledExceptionFilter", (void*)&trt_UnhandledExceptionFilter},
  {"IsDebuggerPresent", (void*)&trt_IsDebuggerPresent},
  {"EncodePointer", (void*)&trt_EncodePointer},
  {"DecodePointer", (void*)&trt_DecodePointer},
  {"InitializeSListHead", (void*)&trt_InitializeSListHead},
  {"InitializeCriticalSectionAndSpinCount",
   (void*)&trt_InitializeCriticalSectionAndSpinCount},
  {"InitializeCriticalSectionEx", (void*)&trt_InitializeCriticalSectionEx},
  {"AcquireSRWLockExclusive", (void*)&trt_AcquireSRWLockExclusive},
  {"ReleaseSRWLockExclusive", (void*)&trt_ReleaseSRWLockExclusive},
  {"TryAcquireSRWLockExclusive", (void*)&trt_TryAcquireSRWLockExclusive},
  {"SleepConditionVariableSRW", (void*)&trt_SleepConditionVariableSRW},
  {"WaitOnAddress", (void*)&trt_WaitOnAddress},
  {"WakeByAddressSingle", (void*)&trt_WakeByAddressSingle},
  {"WakeByAddressAll", (void*)&trt_WakeByAddressAll},
  {"WakeAllConditionVariable", (void*)&trt_WakeAllConditionVariable},
  {"WakeConditionVariable", (void*)&trt_WakeConditionVariable},
  {"FindFirstFileExW", (void*)&trt_FindFirstFileExW},
  {"FindFirstFileW", (void*)&trt_FindFirstFileW},
  {"FindNextFileW", (void*)&trt_FindNextFileW},
  {"FindClose", (void*)&trt_FindClose},
  {"GetFileAttributesExW", (void*)&trt_GetFileAttributesExW},
  {"SetFilePointerEx", (void*)&trt_SetFilePointerEx},
  {"FlushFileBuffers", (void*)&trt_FlushFileBuffers},
  {"GetFileType", (void*)&trt_GetFileType},
  {"GetCurrentProcessId", (void*)&trt_GetCurrentProcessId},
  {"GetProcessId", (void*)&trt_GetProcessId},
  {"GetModuleHandleExW", (void*)&trt_GetModuleHandleExW},
  {"GetModuleHandleA", (void*)&trt_GetModuleHandleA},
  {"GetSystemTimeAsFileTime", (void*)&trt_GetSystemTimeAsFileTime},
  {"GetSystemTime", (void*)&trt_GetSystemTime},
};

int trt_bridge_register_kernel32(void) {
  trt_module_register("kernel32.dll", TRT_ROUTE_BUILTIN, k_kernel32_fns,
                       (int)(sizeof(k_kernel32_fns) / sizeof(k_kernel32_fns[0])));
  // kernelbase forwards: PE games import a mix; route kernelbase to the same
  // builtin surface (Wine does the same via spec forwarders)
  trt_module_register("kernelbase.dll", TRT_ROUTE_BUILTIN, k_kernel32_fns,
                       (int)(sizeof(k_kernel32_fns) / sizeof(k_kernel32_fns[0])));
  // the OrTMM-audit gap batch (file/locale/time/process surface)
  {
    extern void trt_gap_register(void);
    trt_gap_register();
  }
  trt_module_register("api-ms-win-core-rtlsupport-l1-1-0.dll", TRT_ROUTE_STUB,
                       nullptr, 0);
  return 0;
}

} // extern "C"
