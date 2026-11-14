// tiramisu/src/trt/bridge_sys.cpp
// The system-information and file APIs every engine touches at startup.
//
// Games query the OS version, the CPU/memory layout, the temp path, the
// ANSI/Unicode code pages and the file system before they will start. A
// missing answer here is often a silent "game quits at launch".
#include "trt.h"
#include "thunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  include <direct.h>
#  define mkdir_portable(p) _mkdir(p)
#  define rmdir_portable(p) _rmdir(p)
#else
#  include <sys/stat.h>
#  include <unistd.h>
#  define mkdir_portable(p) mkdir((p), 0755)
#  define rmdir_portable(p) rmdir(p)
#endif

#include <errno.h>

namespace {

size_t wcslen_(const u16* s) {
  size_t n = 0;
  while (s && s[n]) ++n;
  return n;
}

void utf16_to_utf8(const u16* in, char* out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; in && in[i] && o + 1 < cap; ++i) {
    if (in[i] < 0x80) out[o++] = (char)in[i];
    else if (in[i] < 0x800) {
      if (o + 2 >= cap) break;
      out[o++] = (char)(0xC0 | (in[i] >> 6));
      out[o++] = (char)(0x80 | (in[i] & 0x3F));
    } else {
      if (o + 3 >= cap) break;
      out[o++] = (char)(0xE0 | (in[i] >> 12));
      out[o++] = (char)(0x80 | ((in[i] >> 6) & 0x3F));
      out[o++] = (char)(0x80 | (in[i] & 0x3F));
    }
  }
  out[o] = 0;
}

} // namespace

extern "C" {
int trt_vfs_resolve_utf16(const u16* guest_path, char* out, size_t out_cap);

// ---- system information ----

// GetVersionExW(OSVERSIONINFOW*)
u32 TRT_STDCALL trt_GetVersionExW(void* info) {
  if (!info) return 0;
  // LPOSTROSVERSIONINFOW: dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion,
  // dwBuildNumber, dwPlatformId, szCSDVersion[128]
  u32* p = (u32*)info;
  p[0] = 276;   // sizeof(OSVERSIONINFOW)
  p[1] = 10;    // Windows 10
  p[2] = 0;
  p[3] = 19045; // build
  p[4] = 2;     // VER_PLATFORM_WIN32_NT
  return 1;
}

u32 TRT_STDCALL trt_GetVersion(void) {
  // Win10 = 0x0A000006 (major 10, minor 0)
  return 0x0A000006u;
}

// GetSystemInfo(SYSTEM_INFO*)
void TRT_STDCALL trt_GetSystemInfo(void* si) {
  if (!si) return;
  // SYSTEM_INFO: u16 alignment, u16 pad, u32 pageSize, u32 minApp, u32 maxApp,
  // u32 dwMask, u32 processors, u32 processorType, u32 allocationGranularity,
  // u16 processorLevel, u16 processorRevision
  u16* w = (u16*)si;
  w[0] = 8;        // alignment
  w[1] = 0;
  u32* d = (u32*)si;
  d[1] = 4096;     // page size
  d[2] = 0x1000;   // min app address
  d[3] = 0x7FFEFFFF;
  d[4] = 0x000003FF;  // processor mask
  d[5] = 8;        // processors
  d[6] = 258;      // PROCESSOR_TYPE_INTEL
  d[7] = 0x10000;  // allocation granularity
  w[16] = 6;       // processor level
  w[17] = 0;       // revision
}

// GetNativeSystemInfo(SYSTEM_INFO*) - same numbers, no WOW64 downgrade
void TRT_STDCALL trt_GetNativeSystemInfo(void* si) {
  trt_GetSystemInfo(si);
}

// GetSystemTimes(idle, kernel, user)
int TRT_STDCALL trt_GetSystemTimes(u64* idle, u64* kernel, u64* user) {
  const u64 now = (u64)clock() / (CLOCKS_PER_SEC / 1000);
  if (idle) *idle = now / 2;
  if (kernel) *kernel = now;
  if (user) *user = now / 2;
  return 1;
}

u32 TRT_STDCALL trt_GetEnvironmentVariableW(const u16* name, u16* buf,
                                               u32 size) {
  if (!name) return 0;
  char n[256];
  utf16_to_utf8(name, n, sizeof(n));
  const char* v = getenv(n);
  if (!v) return 0;
  const u32 need = (u32)strlen(v) + 1;
  if (buf && size >= need) {
    for (u32 i = 0; i < need; ++i) buf[i] = (u16)(u8)v[i];
    return need;
  }
  return need;
}

u32 TRT_STDCALL trt_ExpandEnvironmentStringsW(const u16* in, u16* out,
                                                u32 size) {
  if (!in) return 0;
  char src[1024], dst[2048];
  utf16_to_utf8(in, src, sizeof(src));
  const char* base = getenv(src);
  snprintf(dst, sizeof(dst), "%s", base ? base : src);
  const u32 need = (u32)strlen(dst) + 1;
  if (out && size >= need) {
    for (u32 i = 0; i < need; ++i) out[i] = (u16)(u8)dst[i];
  }
  return need;
}

u32 TRT_STDCALL trt_GetACP(void) { return 1252; }
u32 TRT_STDCALL trt_GetOEMCP(void) { return 437; }
u32 TRT_STDCALL trt_GetUserDefaultLCID(void) { return 0x0409; }
u32 TRT_STDCALL trt_GetUserDefaultLangID(void) { return 0x0409; }
u32 TRT_STDCALL trt_GetSystemDefaultLCID(void) { return 0x0409; }
u32 TRT_STDCALL trt_GetSystemDefaultLangID(void) { return 0x0409; }

int TRT_STDCALL trt_GetComputerNameW(u16* buf, u32* size) {
  const char* host = getenv("COMPUTERNAME");
  if (!host) host = "TIRAMISU";
  const u32 need = (u32)strlen(host) + 1;
  if (buf && size && *size >= need) {
    for (u32 i = 0; i < need; ++i) buf[i] = (u16)(u8)host[i];
    *size = need;
    return 1;
  }
  if (size) *size = need;
  return 0;
}

// ---- wide/ANSI conversion ----

// MultiByteToWideChar(codepage, flags, src, srclen, dst, dstlen)
int TRT_STDCALL trt_MultiByteToWideChar(u32 codepage, u32 flags,
                                          const char* src, int srclen,
                                          u16* dst, int dstlen) {
  (void)codepage; (void)flags;
  if (!src) return 0;
  const int len = srclen < 0 ? (int)strlen(src) : srclen;
  if (!dst || dstlen <= 0) return len + 1;
  int n = 0;
  for (int i = 0; i < len; ++i) {
    const u8 c = (u8)src[i];
    u16 w = c;
    if (c >= 0x80) {  // latin-1 -> utf-16 for the common range
      w = (u16)(0x00C0 | (c >> 6));
      if (n + 1 >= dstlen) break;
      dst[n++] = w;
      w = (u16)(0x0080 | (c & 0x3F));
    }
    if (n + 1 > dstlen) break;
    dst[n++] = w;
  }
  if (n < dstlen) dst[n] = 0;
  return n;
}

// WideCharToMultiByte(codepage, flags, src, srclen, dst, dstlen, def, used)
int TRT_STDCALL trt_WideCharToMultiByte(u32 codepage, u32 flags,
                                          const u16* src, int srclen, char* dst,
                                          int dstlen, const char* def,
                                          void* used) {
  (void)codepage; (void)flags; (void)def; (void)used;
  if (!src) return 0;
  const int len = srclen < 0 ? (int)wcslen_(src) : srclen;
  if (!dst || dstlen <= 0) return len;
  int n = 0;
  for (int i = 0; i < len && n < dstlen; ++i) {
    const u16 c = src[i];
    if (c < 0x80) {
      dst[n++] = (char)c;
    } else if (c < 0x800) {
      if (n + 1 >= dstlen) break;
      dst[n++] = (char)(0xC0 | (c >> 6));
      dst[n++] = (char)(0x80 | (c & 0x3F));
    } else {
      if (n + 2 >= dstlen) break;
      dst[n++] = (char)(0xE0 | (c >> 12));
      dst[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
      dst[n++] = (char)(0x80 | (c & 0x3F));
    }
  }
  return n;
}

// ---- files and directories ----

u32 TRT_STDCALL trt_GetTempPathW(u32 size, u16* buf) {
  // GUEST-style path under a mounted drive. Returning the HOST path ("/tmp/")
  // used to poison every derived path: PyInstaller built "/tmp\_MEIxxxx"
  // which the resolver treated as a RELATIVE guest path (host-absolute paths
  // are not guest paths), so the temp dir never materialised and the
  // candidate-name probe looped for 2.5M opens (watchdog exit 43). The
  // environment block already advertises TEMP=C:\Temp — match it.
  const char* t = "C:\\Temp\\";
  const u32 need = (u32)strlen(t) + 1;
  if (buf && size >= need) {
    for (u32 i = 0; i < need; ++i) buf[i] = (u16)(u8)t[i];
    return need;
  }
  return need;
}

u32 TRT_STDCALL trt_GetTempFileNameW(const u16* dir, const u16* prefix,
                                       u32 unique, u16* buf) {
  char d[256], p[64];
  utf16_to_utf8(dir, d, sizeof(d));
  utf16_to_utf8(prefix, p, sizeof(p));
  char out[512];
  snprintf(out, sizeof(out), "%s%s%u.tmp", d, p, unique);
  const u32 need = (u32)strlen(out) + 1;
  if (buf) {
    for (u32 i = 0; i < need; ++i) buf[i] = (u16)(u8)out[i];
  }
  return need;
}

u32 TRT_STDCALL trt_GetWindowsDirectoryW(u16* buf, u32 size) {
  const char* w = "C:\\Windows";
  const u32 need = (u32)strlen(w) + 1;
  if (buf && size >= need)
    for (u32 i = 0; i < need; ++i) buf[i] = (u16)(u8)w[i];
  return need;
}

u32 TRT_STDCALL trt_GetFileAttributesW(const u16* path) {
  char p[512];
  if (trt_vfs_resolve_utf16(path, p, sizeof(p)) != 0)
    utf16_to_utf8(path, p, sizeof(p));
  FILE* f = fopen(p, "rb");
  if (!f) return 0xFFFFFFFFu;  // INVALID_FILE_ATTRIBUTES
  fclose(f);
  return 0x80;  // FILE_ATTRIBUTE_NORMAL
}

int TRT_STDCALL trt_SetFileAttributesW(const u16* path, u32 attrs) {
  (void)path; (void)attrs;
  return 1;
}

int TRT_STDCALL trt_DeleteFileW(const u16* path) {
  char p[512];
  if (trt_vfs_resolve_utf16(path, p, sizeof(p)) != 0)
    utf16_to_utf8(path, p, sizeof(p));
  return remove(p) == 0 ? 1 : 0;
}

int TRT_STDCALL trt_CreateDirectoryW(const u16* path, void* sa) {
  (void)sa;
  char p[512];
  // resolve via the VFS so relative guest paths (".\_MEIxxxx") land on the
  // guest cwd drive instead of mkdir'ing a literal ".\_MEIxxxx" entry in
  // the host cwd (measured: PyInstaller's extraction dir went to the wrong
  // place and CreateFileW could not find it).
  if (trt_vfs_resolve_utf16(path, p, sizeof(p)) != 0) {
    utf16_to_utf8(path, p, sizeof(p));  // fail-visible: try the raw name
  }
  // create missing parents (Win32 C:\Temp exists on a real install; on the
  // console the drive_c mount starts empty, so "C:\Temp\_MEIxxxx" needs the
  // intermediate level). Each level is a real mkdir, not a fake success.
  for (char* q = p + 1; *q; ++q) {
    if (*q == '/') {
      *q = 0;
      mkdir_portable(p);
      *q = '/';
    }
  }
  // Win32 semantics: nonzero = created. POSIX mkdir returns 0 on success —
  // and EEXIST for a directory that ALREADY exists is also a Win32 success
  // (ERROR_ALREADY_EXISTS, GetLastError names it; the call itself answers 1).
  if (mkdir_portable(p) == 0) return 1;
  return errno == EEXIST ? 1 : 0;
}

int TRT_STDCALL trt_RemoveDirectoryW(const u16* path) {
  char p[512];
  if (trt_vfs_resolve_utf16(path, p, sizeof(p)) != 0)
    utf16_to_utf8(path, p, sizeof(p));
  return rmdir_portable(p);
}

int TRT_STDCALL trt_CopyFileW(const u16* src, const u16* dst, bool fail) {
  (void)fail;
  char s[512], d[512];
  if (trt_vfs_resolve_utf16(src, s, sizeof(s)) != 0)
    utf16_to_utf8(src, s, sizeof(s));
  if (trt_vfs_resolve_utf16(dst, d, sizeof(d)) != 0)
    utf16_to_utf8(dst, d, sizeof(d));
  FILE* in = fopen(s, "rb");
  if (!in) return 0;
  FILE* out = fopen(d, "wb");
  if (!out) { fclose(in); return 0; }
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
  fclose(in);
  fclose(out);
  return 1;
}

int trt_bridge_register_system(void) {
  static const trt_named_fn k_fns[] = {
      {"GetVersionExW", (void*)&trt_GetVersionExW},
      {"GetVersion", (void*)&trt_GetVersion},
      {"GetSystemInfo", (void*)&trt_GetSystemInfo},
      {"GetNativeSystemInfo", (void*)&trt_GetNativeSystemInfo},
      {"GetSystemTimes", (void*)&trt_GetSystemTimes},
      {"GetEnvironmentVariableW", (void*)&trt_GetEnvironmentVariableW},
      {"ExpandEnvironmentStringsW", (void*)&trt_ExpandEnvironmentStringsW},
      {"GetACP", (void*)&trt_GetACP},
      {"GetOEMCP", (void*)&trt_GetOEMCP},
      {"GetUserDefaultLCID", (void*)&trt_GetUserDefaultLCID},
      {"GetUserDefaultLangID", (void*)&trt_GetUserDefaultLangID},
      {"GetSystemDefaultLCID", (void*)&trt_GetSystemDefaultLCID},
      {"GetSystemDefaultLangID", (void*)&trt_GetSystemDefaultLangID},
      {"GetComputerNameW", (void*)&trt_GetComputerNameW},
      {"MultiByteToWideChar", (void*)&trt_MultiByteToWideChar},
      {"WideCharToMultiByte", (void*)&trt_WideCharToMultiByte},
      {"GetTempPathW", (void*)&trt_GetTempPathW},
      // Win8+ sibling the UCRT itself imports (kernel32!GetTempPath2W);
      // blanket-0 bind made UCRT's temp-name loop spin -> boot backstop.
      {"GetTempPath2W", (void*)&trt_GetTempPathW},
      {"GetTempFileNameW", (void*)&trt_GetTempFileNameW},
      {"GetWindowsDirectoryW", (void*)&trt_GetWindowsDirectoryW},
      {"GetFileAttributesW", (void*)&trt_GetFileAttributesW},
      {"SetFileAttributesW", (void*)&trt_SetFileAttributesW},
      {"DeleteFileW", (void*)&trt_DeleteFileW},
      {"CreateDirectoryW", (void*)&trt_CreateDirectoryW},
      {"RemoveDirectoryW", (void*)&trt_RemoveDirectoryW},
      {"CopyFileW", (void*)&trt_CopyFileW},
  };
  trt_module_register("kernel32.dll", TRT_ROUTE_BUILTIN, k_fns,
                       (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

} // extern "C"
