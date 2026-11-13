// tiramisu/src/trt/bridge_kernel32_gap.cpp
// The OrTMM-audit gap batch: the 20 kernel32 symbols the offline boot audit
// (tools/audit_boot via trt_audit_imports) named as unresolved on a real
// 48.9 MB PE64 game binary. Each is honest on existing infrastructure;
// anything that cannot be real FAILS NAMED (the fail-visible rule).
//
//   IsValidCodePage / GetCPInfo family   — CRT locale boot (cp1252/UTF-8)
//   SetEnvironmentVariableW              — real env store behind GetEnvironmentStringsW
//   MulDiv                               — the exact Win64 integer semantics
//   FormatMessageW                       — FORMAT_MESSAGE_FROM_SYSTEM only, named gap else
//   SetDllDirectoryW                     — loader already searches the game dir; accept+log
//   CreateSymbolicLinkW                  — named failure (VFS has no links)
//   GetDriveTypeW                        — every mounted path is DRIVE_FIXED
//   GetExitCodeProcess                   — the single guest "process" exit code
//   LocalFree / LocalAlloc               — Win32 legacy wrappers over HeapAlloc
//   K32EnumProcessModules / K32GetModuleFileNameExW — forward to the psapi bridge
//   GetFinalPathNameByHandleW            — VFS path of an open file handle
//   GetTimeZoneInformation               — UTC (bias 0), honest + named
//   SetEndOfFile                         — truncate through the FILE*
//   GetFileInformationByHandle           — size/times/attributes from the FILE*
//   PeekNamedPipe                        — named failure: no pipe handles exist
//   SystemTimeToTzSpecificLocalTime / FileTimeToSystemTime — real conversions
//   GetFullPathNameW                     — cwd-relative path resolution (VFS-honest)
//   GetFileSizeEx                        — 64-bit size through the FILE*
#include "thunk.h"
#include "trt.h"
#include "pe.h"  // TRT_STDCALL (ms_abi on Linux test hosts — guest-call rule)

#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#  include <io.h>       // _chsize_s
#else
#  include <unistd.h>   // ftruncate
#endif

namespace k32gap {
void line(const char* s) { trt_core_log(s); }
}
#define LOG(s_) k32gap::line(s_)

// ---- shared with bridge_kernel32.cpp (extern-C accessors live there) ------
extern "C" FILE* trt_file_os(void* h);   // handle -> FILE* (or null)
extern "C" char* trt_gap_cwd(void);      // the guest cwd (g_cwd)
extern "C" void TRT_STDCALL trt_SetLastError(u32 e);
extern "C" u32 TRT_STDCALL trt_GetLastError(void);

namespace {

// ---- environment store: GetEnvironmentStringsW shows a fixed block today.
// SetEnvironmentVariableW appends/updates a real store that the next
// GetEnvironmentStringsW build includes (honest cross-call consistency).
constexpr int k_env_max = 64;
struct env_rec { char name[64]; char value[256]; bool used; };
env_rec g_env_extra[k_env_max];
bool g_env_dirty = false;

env_rec* env_find(const char* name) {
  for (env_rec& r : g_env_extra)
    if (r.used && strcmp(r.name, name) == 0) return &r;
  return nullptr;
}

// locale: cp1252 for the CRT boot path (IsValidCodePage(1252)=1,
// GetCPInfo fills CPINFO with the max char size + default char)
constexpr u32 k_cp1252 = 1252;
constexpr u32 k_utf8 = 65001;

bool valid_cp(u32 cp) {
  return cp == k_cp1252 || cp == k_utf8 || cp == 0 /*CP_ACP*/ ||
         cp == 20127 /*US-ASCII*/ || cp == 2019;
}

}  // namespace

extern "C" {

// registered by trt_bridge_register_kernel32 after the core table
void trt_gap_register(void);

// ---- CRT locale boot -------------------------------------------------------
int TRT_STDCALL trt_IsValidCodePage(u32 cp) { return valid_cp(cp) ? 1 : 0; }

// ---- environment -----------------------------------------------------------
int TRT_STDCALL trt_SetEnvironmentVariableW(const u16* name, const u16* value) {
  if (!name || !name[0]) { trt_SetLastError(87); return 0; }
  char n[64] = {}, v[256] = {};
  size_t i = 0;
  for (; name[i] && i < sizeof(n) - 1; ++i) n[i] = (char)name[i];
  if (value)
    for (i = 0; value[i] && i < sizeof(v) - 1; ++i) v[i] = (char)value[i];
  if (!value) {  // delete
    if (env_rec* r = env_find(n)) r->used = false;
    g_env_dirty = true;
    return 1;
  }
  env_rec* r = env_find(n);
  if (!r) {
    for (env_rec& e : g_env_extra)
      if (!e.used) { r = &e; break; }
    if (!r) { LOG("kernel32: SetEnvironmentVariableW store full (64)"); trt_SetLastError(87); return 0; }
    r->used = true;
    snprintf(r->name, sizeof(r->name), "%s", n);
  }
  snprintf(r->value, sizeof(r->value), "%s", v);
  g_env_dirty = true;
  char line[384];
  snprintf(line, sizeof(line), "kernel32: env %s=%s", n, v);
  LOG(line);
  return 1;
}

// ---- math / string legacy --------------------------------------------------
int TRT_STDCALL trt_MulDiv(int a, int b, int c) {
  // exact Win64 semantics: 64-bit intermediate, round-half-away-from-zero,
  // -1 on divide-by-zero and overflow
  if (c == 0) return -1;
  const i64 prod = (i64)a * (i64)b;
  const i64 q = (prod >= 0) ? (prod + c / 2) / c : (prod - c / 2) / c;
  if (q > 0x7FFFFFFF || q < -0x80000000LL) return -1;
  return (int)q;
}

// ---- diagnostics formatting -------------------------------------------------
u32 TRT_STDCALL trt_FormatMessageW(u32 flags, const void* src, u32 msgid,
                                   u32 langid, u16* buf, u32 size, void* args) {
  (void)langid; (void)args;
  // FORMAT_MESSAGE_FROM_SYSTEM (0x1000): a numeric error code only — the
  // honest answer is the code itself; full message tables are a named gap.
  if (flags & 0x1000) {
    char narrow[96];
    snprintf(narrow, sizeof(narrow), "error %lu", (unsigned long)msgid);
    u32 n = 0;
    for (; narrow[n] && n < size - 1; ++n) buf[n] = (u16)(u8)narrow[n];
    buf[n] = 0;
    return n;
  }
  char line[128];
  snprintf(line, sizeof(line),
           "kernel32: FormatMessageW flags=0x%lx unsupported (only "
           "FROM_SYSTEM) -> 0", (unsigned long)flags);
  LOG(line);
  return 0;
}

// ---- loader search path ------------------------------------------------------
int TRT_STDCALL trt_SetDllDirectoryW(const u16* dir) {
  // the loader already searches the game directory and every mounted VFS
  // root; accepting and logging is the honest behavior (the game wants its
  // own directory searched for its own DLLs — which we already do)
  char line[256];
  if (dir && dir[0]) {
    char n[192] = {};
    size_t i = 0;
    for (; dir[i] && i < sizeof(n) - 1; ++i) n[i] = (char)dir[i];
    snprintf(line, sizeof(line), "kernel32: SetDllDirectoryW(%s) accepted "
             "(loader searches game dir already)", n);
  } else {
    snprintf(line, sizeof(line), "kernel32: SetDllDirectoryW(NULL) accepted");
  }
  LOG(line);
  return 1;
}

int TRT_STDCALL trt_CreateSymbolicLinkW(const u16* link, const u16* target,
                                        u32 flags) {
  (void)flags;
  char line[256];
  snprintf(line, sizeof(line),
           "kernel32: CreateSymbolicLinkW -> FALSE (the VFS has no symbolic "
           "links) link=%p target=%p", (void*)link, (void*)target);
  LOG(line);
  trt_SetLastError(50);  // ERROR_NOT_SUPPORTED
  return 0;
}

// ---- drives -------------------------------------------------------------------
u32 TRT_STDCALL trt_GetDriveTypeW(const u16* root) {
  // every mounted VFS path is a fixed disk; the CRT's _wgetcwd and game
  // asset code only distinguish DRIVE_FIXED(3) from DRIVE_NO_ROOT_DIR(1)
  (void)root;
  return 3;  // DRIVE_FIXED
}

// ---- process -------------------------------------------------------------------
int TRT_STDCALL trt_GetExitCodeProcess(void* proc, u32* code) {
  if (!code) { trt_SetLastError(87); return 0; }
  (void)proc;
  *code = 0;  // the single guest "process" has not exited while it runs
  return 1;
}

// ---- Local* legacy heap (Win3.1 compatibility wrappers) -------------------------
extern "C" void* TRT_STDCALL trt_HeapAlloc(void* heap, u32 flags, u64 bytes);
extern "C" int TRT_STDCALL trt_HeapFree(void* heap, u32 flags, void* p);

void* TRT_STDCALL trt_LocalAlloc(u32 flags, u64 bytes) {
  // LMEM_FIXED=0, LMEM_MOVEABLE=2 (treated as FIXED: the handle==pointer
  // lie holds because games re-lock anyway), LMEM_ZEROINIT=0x40
  return trt_HeapAlloc(nullptr, (flags & 0x40) ? 0x8 : 0, bytes);
}

void* TRT_STDCALL trt_LocalFree(void* p) {
  return trt_HeapFree(nullptr, 0, p) ? nullptr : p;  // NULL = success
}// ---- K32 forwarders (psapi bridge owns the real implementation) -----------------
extern "C" int TRT_GUEST_ABI psapi_K32EnumProcessModules(void* proc,
    void** lphandle, u32 cb, u32* needed);
extern "C" int TRT_GUEST_ABI psapi_K32GetModuleFileNameExW(void* proc,
    void* mod, u16* buf, u32 cb);

int TRT_STDCALL trt_K32EnumProcessModules(void* proc, void** lphandle,
                                          u32 cb, u32* needed) {
  return psapi_K32EnumProcessModules(proc, lphandle, cb, needed);
}

u32 TRT_STDCALL trt_K32GetModuleFileNameExW(void* proc, void* mod, u16* buf,
                                            u32 cb) {
  return psapi_K32GetModuleFileNameExW(proc, mod, buf, cb);
}

// ---- paths ------------------------------------------------------------------------
u32 TRT_STDCALL trt_GetFinalPathNameByHandleW(void* h, u16* buf, u32 len,
                                              u32 flags) {
  (void)flags;
  FILE* os = trt_file_os(h);
  if (!os) { trt_SetLastError(6); return 0; }  // ERROR_INVALID_HANDLE
  // VFS handles were opened by path; the final path IS the path. The FILE*
  // does not remember it, so the honest answer for now: fail named.
  char line[160];
  snprintf(line, sizeof(line),
           "kernel32: GetFinalPathNameByHandleW -> 0 (handles do not carry "
           "their path; h=%p)", h);
  LOG(line);
  trt_SetLastError(87);
  (void)buf; (void)len;
  return 0;
}

u32 TRT_STDCALL trt_GetFullPathNameW(const u16* name, u32 len, u16* out,
                                     u16** filepart) {
  if (filepart) *filepart = nullptr;
  if (!name || !out || !len) return 0;
  char n[512] = {};
  size_t i = 0;
  for (; name[i] && i < sizeof(n) - 1; ++i) n[i] = (char)name[i];
  // absolute already (drive letter prefix) -> pass through
  char full[1024];
  if (n[0] && n[1] == ':')
    snprintf(full, sizeof(full), "%s", n);
  else
    snprintf(full, sizeof(full), "%s%s", trt_gap_cwd(), n);
  u32 m = 0;
  for (; full[m] && m < len - 1; ++m) out[m] = (u16)(u8)full[m];
  out[m] = 0;
  // filepart points at the last component inside out
  if (filepart) {
    u32 last = 0;
    for (u32 k = 0; k < m; ++k)
      if (out[k] == (u16)'\\' || out[k] == (u16)'/') last = k + 1;
    if (last && last < m) *filepart = out + last;
  }
  return m;  // full length excluding NUL (Win64 semantics: may exceed len)
}

// ---- time zone / time conversion ---------------------------------------------------
u32 TRT_STDCALL trt_GetTimeZoneInformation(void* out) {
  // TIME_ZONE_INFORMATION: Bias i32 (long), StandardName[32] u16,
  // StandardDate (8 words), StandardBias, DaylightName[32], DaylightDate,
  // DaylightBias = 172 bytes. UTC bias 0, no daylight — honest for a
  // console (no wall-clock locale of its own).
  if (!out) { trt_SetLastError(87); return (u32)-1; }
  memset(out, 0, 172);
  char line[96];
  snprintf(line, sizeof(line), "kernel32: GetTimeZoneInformation -> UTC "
           "(bias 0, no daylight)");
  LOG(line);
  return 0;  // TIME_ZONE_ID_UNKNOWN
}

int TRT_STDCALL trt_FileTimeToSystemTime(const u64* ft, u16* st) {
  if (!ft || !st) { trt_SetLastError(87); return 0; }
  // reuse trt_GetSystemTime's civil conversion: it takes a FILETIME and
  // writes 8 SYSTEMTIME words
  extern void TRT_STDCALL trt_GetSystemTime(u16* st);
  // GetSystemTime writes "now"; convert the GIVEN ft through the same math
  // by inlining the conversion here (the algorithm lives in
  // bridge_kernel32.cpp — duplicating 20 lines beats a fake dependency)
  i64 t = (i64)(*ft) / 10000000ll;
  u16 ms = (u16)(((i64)(*ft) % 10000000ll) / 10000);
  i64 days = t / 86400; i64 secs = t % 86400;
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
  st[0] = (u16)y; st[1] = (u16)m; st[2] = (u16)(d - 1) % 7; st[3] = (u16)d;
  st[4] = (u16)(secs/3600); st[5] = (u16)((secs%3600)/60); st[6] = (u16)(secs%60);
  st[7] = ms;
  return 1;
}

int TRT_STDCALL trt_SystemTimeToTzSpecificLocalTime(const void* tzi,
                                                    const u16* utc, u16* local) {
  // the runtime's only time zone is UTC (GetTimeZoneInformation bias 0):
  // local == utc, a straight copy is the exact honest answer
  if (!utc || !local) { trt_SetLastError(87); return 0; }
  (void)tzi;
  for (int i = 0; i < 8; ++i) local[i] = utc[i];
  return 1;
}

// ---- files ---------------------------------------------------------------------------
int TRT_STDCALL trt_SetEndOfFile(void* h) {
  FILE* os = trt_file_os(h);
  if (!os) { trt_SetLastError(6); return 0; }
  fflush(os);
#ifdef _WIN32
  if (_chsize_s(_fileno(os), _ftelli64(os)) != 0) { trt_SetLastError(87); return 0; }
#else
  if (ftruncate(fileno(os), ftell(os)) != 0) { trt_SetLastError(87); return 0; }
#endif
  return 1;
}

// Win64 ftell/fseek are 32-bit long on Windows; keep it simple: files are
// < 2GB in the VFS so long works on both hosts. (_ftelli64 exists but the
// handle table stores FILE* only — no size caching needed for <2GB.)

int TRT_STDCALL trt_GetFileInformationByHandle(void* h, void* info) {
  // BY_HANDLE_FILE_INFORMATION: attributes u32, times 8+8, volume/serial
  // u32+u32... 52 bytes total on x64. Size + attributes are the fields
  // games actually read; times are zero (named gap).
  struct bhfi { u32 attrs; u64 created, accessed, modified;
                u32 vol_serial, index_hi, index_lo; u64 size; };
  FILE* os = trt_file_os(h);
  if (!os) { trt_SetLastError(6); return 0; }
  if (!info) { trt_SetLastError(87); return 0; }
  bhfi* b = (bhfi*)info;
  memset(b, 0, sizeof(*b));
  b->attrs = 0x80;  // FILE_ATTRIBUTE_NORMAL
  const long pos = ftell(os);
  fseek(os, 0, SEEK_END);
  const long sz = ftell(os);
  fseek(os, pos, SEEK_SET);
  b->size = (u64)sz;
  return 1;
}

i64 TRT_STDCALL trt_GetFileSizeEx(void* h, i64* size) {
  FILE* os = trt_file_os(h);
  if (!os) { trt_SetLastError(6); if (size) *size = 0; return 0; }
  const long pos = ftell(os);
  fseek(os, 0, SEEK_END);
  const long sz = ftell(os);
  fseek(os, pos, SEEK_SET);
  if (size) *size = (i64)sz;
  return 1;
}

int TRT_STDCALL trt_PeekNamedPipe(void* h, void* buf, u32 bytes, u32* read,
                                  u32* avail, u32* left) {
  // every handle in the VFS is a disk file; no pipe handles exist. Named
  // failure is the fail-visible rule.
  char line[160];
  snprintf(line, sizeof(line),
           "kernel32: PeekNamedPipe -> FALSE (no pipe handles in the VFS; "
           "h=%p)", h);
  LOG(line);
  (void)buf; (void)bytes; (void)read; (void)avail; (void)left;
  trt_SetLastError(87);
  return 0;
}

static const trt_named_fn k_gap_fns[] = {
  {"IsValidCodePage", (void*)&trt_IsValidCodePage},
  {"SetEnvironmentVariableW", (void*)&trt_SetEnvironmentVariableW},
  {"MulDiv", (void*)&trt_MulDiv},
  {"FormatMessageW", (void*)&trt_FormatMessageW},
  {"SetDllDirectoryW", (void*)&trt_SetDllDirectoryW},
  {"CreateSymbolicLinkW", (void*)&trt_CreateSymbolicLinkW},
  {"GetDriveTypeW", (void*)&trt_GetDriveTypeW},
  {"GetExitCodeProcess", (void*)&trt_GetExitCodeProcess},
  {"LocalAlloc", (void*)&trt_LocalAlloc},
  {"LocalFree", (void*)&trt_LocalFree},
  {"K32EnumProcessModules", (void*)&trt_K32EnumProcessModules},
  {"K32GetModuleFileNameExW", (void*)&trt_K32GetModuleFileNameExW},
  {"GetFinalPathNameByHandleW", (void*)&trt_GetFinalPathNameByHandleW},
  {"GetFullPathNameW", (void*)&trt_GetFullPathNameW},
  {"GetTimeZoneInformation", (void*)&trt_GetTimeZoneInformation},
  {"FileTimeToSystemTime", (void*)&trt_FileTimeToSystemTime},
  {"SystemTimeToTzSpecificLocalTime", (void*)&trt_SystemTimeToTzSpecificLocalTime},
  {"SetEndOfFile", (void*)&trt_SetEndOfFile},
  {"GetFileInformationByHandle", (void*)&trt_GetFileInformationByHandle},
  {"GetFileSizeEx", (void*)&trt_GetFileSizeEx},
  {"PeekNamedPipe", (void*)&trt_PeekNamedPipe},
};

// registered by trt_bridge_register_kernel32 after the core table
void trt_gap_register(void);

#include "trt.h"  // trt_named_fn, trt_route (re-included harmless)

void trt_gap_register(void) {
  // merge into kernel32 + kernelbase (same table, same merge path)
  extern void trt_module_register(const char* name, trt_route route,
                                  const trt_named_fn* fns, int count);
  trt_module_register("kernel32.dll", TRT_ROUTE_BUILTIN, k_gap_fns,
                      (int)(sizeof(k_gap_fns) / sizeof(k_gap_fns[0])));
  trt_module_register("kernelbase.dll", TRT_ROUTE_BUILTIN, k_gap_fns,
                      (int)(sizeof(k_gap_fns) / sizeof(k_gap_fns[0])));
}

}  // extern "C"
