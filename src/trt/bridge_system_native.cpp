#if defined(_WIN32)  // UWP-only: Mono System.Native P/Invoke bridge
// tiramisu/src/trt/bridge_system_native.cpp
// Host-side implementation of Mono corelib's "System.Native" P/Invoke
// surface for the UWP embedding (Celeste under embedded Mono + FNA).
//
// WHY: the packaged framework is Linux Mono 4.5. Its etc/mono/config maps
// System.Native to libmono-native.so only on non-Windows ("os=!windows"),
// so on the console Mono looks for System.Native.dll, which does not exist,
// and the first file-IO call dies with DllNotFoundException (seen in .127:
// Interop+Sys..cctor -> LChflagsCanSetHiddenFlag). The host exports the
// whole surface from tiramisu.exe instead (host-bridges.def) and the UWP
// assembly-load hook maps "System.Native" -> "__Internal" (mono_host.cpp).
// The Linux gate is untouched: the Linux hook never installs that dllmap,
// so offline Mono keeps using the real libmono-native.so.
//
// CONTRACT SOURCES (all measured, not guessed — see
// ~/tiramisu-gow-skye/docs/progress/sysnative-bridge-report.md):
//   * Signatures: monodis --implmap on mscorlib/System/System.Core
//     (46 unique native entry points across the three assemblies).
//   * Struct layouts: reflection probe (Marshal.OffsetOf/SizeOf) on the
//     packaged assemblies (byte-identical to /usr/lib/mono/4.5/).
//   * errno -> Interop.Error mapping: measured live from the real
//     libmono-native.so (sysnative_errno_table.inc, generated).
//   * ReadDirR return contract (0 = entry, -1 = end of dir, +errno =
//     error) + NameLength == -1 sentinel: verified against the real
//     libmono-native.so AND the managed callers' IL.
//   * FileStream reads go through MonoIO (w32file.c icalls), NOT
//     Interop/Sys::Read — the Sys.Read icall is only used by
//     FileSystemWatcher (inotify), so no icall registration is needed.
//   * The managed Interop/Error enum is Mono's own numbering
//     (0x10000 + alphabetical index), NOT Linux errno values.
//
// ERRNO DISCIPLINE (read this before touching the file):
//   * The managed layer reads the failure errno via
//     Marshal.GetLastWin32Error() after a lasterr pinvoke, then feeds it
//     to ConvertErrorPlatformToPal, which is indexed by LINUX errno
//     numbers. MSVC's errno.h DIFFERS for several values (ENOSYS 40 vs
//     Linux 38, ENOTEMPTY 41 vs 39, ENAMETOOLONG 38 vs 36, ELOOP 42 vs
//     40), so this file NEVER uses the CRT errno macros — it uses the
//     L_* constants below (Linux numbers) everywhere.
//   * Every failure sets BOTH errno and SetLastError to the Linux value:
//     Mono's Windows pinvoke wrapper captures GetLastError; setting errno
//     too covers the CRT-errno path. Belt and suspenders.
//   * OpenDir failure returns NULL (managed checks for IntPtr.Zero);
//     Open/INotifyInit failure returns -1 (SafeFileHandle invalid = -1).
//
// LOGGING: the console log is the only diagnostic channel, and Stat2 is
// a hot path (every File.Exists), so Stat2/LStat2/FStat2/ReadDirR stay
// silent; stubs log once each; rare real failures (Open, MkDir, Rename,
// Unlink, OpenDir) log with the path + errno.
//
// UWP API NOTES: CreateFileW and GetComputerNameExW are DESKTOP-partition
// in the SDK headers (undeclared under WINAPI_FAMILY_APP) — this file
// uses CreateFile2 and GetComputerNameW instead (both APP-partition).
// Every other Win32 call here was verified APP-partition by compiling
// this TU under the exact UWP crossbuild flags (see the report).
#include <windows.h>
#include <bcrypt.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include "thunk.h"

namespace {

// ---- Linux errno numbers (NOT the MSVC values — see header comment) ----
enum LinuxErrno : int32_t {
  L_EPERM = 1,
  L_ENOENT = 2,
  L_ESRCH = 3,
  L_EINTR = 4,
  L_EIO = 5,
  L_ENXIO = 6,
  L_E2BIG = 7,
  L_ENOEXEC = 8,
  L_EBADF = 9,
  L_ECHILD = 10,
  L_EAGAIN = 11,
  L_ENOMEM = 12,
  L_EACCES = 13,
  L_EFAULT = 14,
  L_EBUSY = 16,
  L_EEXIST = 17,
  L_EXDEV = 18,
  L_ENODEV = 19,
  L_ENOTDIR = 20,
  L_EISDIR = 21,
  L_EINVAL = 22,
  L_ENFILE = 23,
  L_EMFILE = 24,
  L_ENOTTY = 25,
  L_EFBIG = 27,
  L_ENOSPC = 28,
  L_ESPIPE = 29,
  L_EROFS = 30,
  L_EMLINK = 31,
  L_EPIPE = 32,
  L_ENAMETOOLONG = 36,
  L_ENOSYS = 38,
  L_ENOTEMPTY = 39,
  L_ELOOP = 40,
};

// Managed Interop/Error values needed by functions that return Error
// directly (Poll). Full enum: 0x10000 + alphabetical index; ENOSYS is
// 65591 (verified by reflection probe).
constexpr int32_t kPalENOSYS = 65591;

// Unix mode bits (managed Interop/Sys/FileTypes, verified by probe).
constexpr int32_t kS_IFDIR = 0x4000;
constexpr int32_t kS_IFREG = 0x8000;
constexpr int32_t kS_IFLNK = 0xA000;

// dirent d_type values (managed Interop/Sys/NodeType, verified by probe).
constexpr int32_t kDT_DIR = 4;
constexpr int32_t kDT_REG = 8;
constexpr int32_t kDT_LNK = 10;

// Mono's OpenFlags (System.dll Interop/Sys/OpenFlags — Mono PAL values,
// NOT Linux: O_CREAT is 32 here, 0x40 on Linux).
constexpr int32_t kO_WRONLY = 1;
constexpr int32_t kO_RDWR = 2;
constexpr int32_t kO_ACCMODE = 3;
constexpr int32_t kO_CREAT = 32;
constexpr int32_t kO_EXCL = 64;
constexpr int32_t kO_TRUNC = 128;

// FileStatusFlags (verified by probe).
constexpr int32_t kFileStatusHasBirthTime = 1;
// UserFlags.UF_HIDDEN (verified by probe).
constexpr uint32_t kUserFlagHidden = 0x8000;

// ---- Managed struct layouts (verified by reflection probe) ------------
// FileStatus: size 112. Flags@0, Mode@4, Uid@8, Gid@12, Size@16,
// ATime@24, ATimeNsec@32, MTime@40, MTimeNsec@48, CTime@56, CTimeNsec@64,
// BirthTime@72, BirthTimeNsec@80, Dev@88, Ino@96, UserFlags@104.
struct FileStatus {
  int32_t Flags;
  int32_t Mode;
  uint32_t Uid;
  uint32_t Gid;
  int64_t Size;
  int64_t ATime;
  int64_t ATimeNsec;
  int64_t MTime;
  int64_t MTimeNsec;
  int64_t CTime;
  int64_t CTimeNsec;
  int64_t BirthTime;
  int64_t BirthTimeNsec;
  int64_t Dev;
  int64_t Ino;
  uint32_t UserFlags;
};
static_assert(sizeof(FileStatus) == 112, "FileStatus layout drifted");
static_assert(offsetof(FileStatus, Size) == 16, "FileStatus.Size offset");
static_assert(offsetof(FileStatus, MTime) == 40, "FileStatus.MTime offset");
static_assert(offsetof(FileStatus, UserFlags) == 104,
              "FileStatus.UserFlags offset");

// DirectoryEntry: size 16. Name@0 (uint8*), NameLength@8, InodeType@12.
struct DirectoryEntry {
  uint8_t* Name;
  int32_t NameLength;
  int32_t InodeType;
};
static_assert(sizeof(DirectoryEntry) == 16, "DirectoryEntry layout drifted");

// UTimBuf: size 16. AcTime@0, ModTime@8 (Unix seconds).
struct UTimBuf {
  int64_t AcTime;
  int64_t ModTime;
};
static_assert(sizeof(UTimBuf) == 16, "UTimBuf layout drifted");

// TimeValPair: size 32. ASec@0, AUSec@8, MSec@16, MUSec@24.
struct TimeValPair {
  int64_t ASec;
  int64_t AUSec;
  int64_t MSec;
  int64_t MUSec;
};
static_assert(sizeof(TimeValPair) == 32, "TimeValPair layout drifted");

// PollEvent: size 8. FileDescriptor@0 (int32), Events@4 (int16),
// TriggeredEvents@6 (int16). Only the size matters here (Poll is a stub).
struct PollEvent {
  int32_t FileDescriptor;
  int16_t Events;
  int16_t TriggeredEvents;
};
static_assert(sizeof(PollEvent) == 8, "PollEvent layout drifted");

// errno -> PAL table, generated from the real libmono-native.so.
#include "sysnative_errno_table.inc"

// ---- small helpers ------------------------------------------------------
int set_fail(int linux_errno) {
  errno = linux_errno;
  SetLastError((DWORD)linux_errno);
  return -1;
}
intptr_t set_fail_ptr(int linux_errno) {
  errno = linux_errno;
  SetLastError((DWORD)linux_errno);
  return (intptr_t)-1;
}

int win32_to_errno(DWORD e) {
  switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_NO_MORE_FILES:
      return L_ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
      return L_EACCES;
    case ERROR_INVALID_HANDLE:
      return L_EBADF;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
      return L_ENOMEM;
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_DATA:
    case ERROR_INVALID_NAME:
      return L_EINVAL;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
      return L_EEXIST;
    case ERROR_DIRECTORY:
      return L_ENOTDIR;
    case ERROR_DIR_NOT_EMPTY:
      return L_ENOTEMPTY;
    case ERROR_DISK_FULL:
      return L_ENOSPC;
    case ERROR_FILENAME_EXCED_RANGE:
      return L_ENAMETOOLONG;
    case ERROR_NOT_SAME_DEVICE:
      return L_EXDEV;
    case ERROR_WRITE_PROTECT:
      return L_EROFS;
    case ERROR_FILE_TOO_LARGE:
      return L_EFBIG;
    case ERROR_BROKEN_PIPE:
      return L_EPIPE;
    default:
      return L_EIO;
  }
}

// Log a message at most once per process (string-literal identity).
// Used for stubs: the first call names itself, repeats stay silent.
void log_once(const char* msg) {
  static const char* logged[48];
  static int count = 0;
  for (int i = 0; i < count; ++i) {
    if (logged[i] == msg) return;
  }
  if (count < 48) logged[count++] = msg;
  trt_core_log(msg);
}

void log_fail(const char* fn, const char* path, int linux_errno) {
  char line[600];
  std::snprintf(line, sizeof(line), "sysnative: %s(%s) failed errno=%d",
                fn, path ? path : "?", linux_errno);
  trt_core_log(line);
}

// FILETIME (100ns since 1601) <-> Unix seconds + nanoseconds.
constexpr int64_t kUnixEpochInFiletime = 116444736000000000LL;
void filetime_to_unix(FILETIME ft, int64_t* sec, int64_t* nsec) {
  uint64_t v =
      ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
  if (v < (uint64_t)kUnixEpochInFiletime) {
    *sec = 0;
    *nsec = 0;
    return;
  }
  uint64_t u = v - (uint64_t)kUnixEpochInFiletime;
  *sec = (int64_t)(u / 10000000ULL);
  *nsec = (int64_t)((u % 10000000ULL) * 100ULL);
}
FILETIME unix_to_filetime(int64_t sec, int64_t nsec) {
  uint64_t v = (uint64_t)kUnixEpochInFiletime +
               (uint64_t)(sec > 0 ? sec : 0) * 10000000ULL +
               (uint64_t)(nsec > 0 ? nsec : 0) / 100ULL;
  FILETIME ft;
  ft.dwLowDateTime = (DWORD)(v & 0xFFFFFFFFULL);
  ft.dwHighDateTime = (DWORD)(v >> 32);
  return ft;
}

// Managed string (UTF-8 from Mono's marshaler; CP_ACP fallback) -> wide.
// Long absolute paths get the \\?\ prefix so deep game paths don't hit
// MAX_PATH. Forward slashes are normalized (\\?\ forbids them).
// Returns false on overflow (caller maps to ENAMETOOLONG).
bool wpath(const char* p, wchar_t* out, size_t cap) {
  if (!p || !out || cap < 8) return false;
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, -1, out,
                              (int)cap);
  if (n <= 0) {
    n = MultiByteToWideChar(CP_ACP, 0, p, -1, out, (int)cap);
    if (n <= 0) {
      out[0] = 0;
      return false;
    }
  }
  out[cap - 1] = 0;
  size_t len = 0;
  while (out[len] != 0) {
    if (out[len] == L'/') out[len] = L'\\';
    ++len;
  }
  if (len >= 240 && len + 4 < cap && len >= 3 && out[1] == L':' &&
      out[2] == L'\\') {
    std::memmove(out + 4, out, (len + 1) * sizeof(wchar_t));
    out[0] = L'\\';
    out[1] = L'\\';
    out[2] = L'?';
    out[3] = L'\\';
  }
  return true;
}

int32_t mode_for_attributes(DWORD attrs, bool is_link) {
  if (is_link) return kS_IFLNK | 0777;
  const bool read_only = (attrs & FILE_ATTRIBUTE_READONLY) != 0;
  if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0)
    return kS_IFDIR | (read_only ? 0555 : 0755);
  return kS_IFREG | (read_only ? 0444 : 0644);
}

void fill_times(FileStatus* out, FILETIME creation, FILETIME last_access,
                FILETIME last_write) {
  filetime_to_unix(last_access, &out->ATime, &out->ATimeNsec);
  filetime_to_unix(last_write, &out->MTime, &out->MTimeNsec);
  // Windows has no ctime (inode-change time); last-write is the closest.
  filetime_to_unix(last_write, &out->CTime, &out->CTimeNsec);
  filetime_to_unix(creation, &out->BirthTime, &out->BirthTimeNsec);
  out->Flags = kFileStatusHasBirthTime;
}

// Best-effort Dev/Ino from a handle (volume serial + file index).
// Leaves them 0 when the handle can't be opened (not fatal: nothing in
// Celeste's path reads Dev/Ino).
void fill_dev_ino(HANDLE h, FileStatus* out) {
  if (h == nullptr || h == INVALID_HANDLE_VALUE) return;
  BY_HANDLE_FILE_INFORMATION info{};
  if (GetFileInformationByHandle(h, &info)) {
    out->Dev = (int64_t)info.dwVolumeSerialNumber;
    out->Ino = (int64_t)(((uint64_t)info.nFileIndexHigh << 32) |
                         (uint64_t)info.nFileIndexLow);
  }
}

// CreateFileW is DESKTOP-partition in the SDK headers (not declared under
// WINAPI_FAMILY_APP); CreateFile2 is the APP-partition equivalent.
HANDLE open_file2(const wchar_t* path, DWORD desired, DWORD share,
                  DWORD disp, DWORD flags) {
  CREATEFILE2_EXTENDED_PARAMETERS p{};
  p.dwSize = sizeof(p);
  p.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
  p.dwFileFlags = flags;
  return CreateFile2(path, desired, share, disp, &p);
}

int stat_impl(const char* path, FileStatus* out, bool follow_links) {
  static bool logged_active = false;
  if (!logged_active) {
    logged_active = true;
    trt_core_log("sysnative: bridge active (Stat2)");
  }
  if (!path || !out) return set_fail(L_EINVAL);
  wchar_t wide[4096];
  if (!wpath(path, wide, 4096)) return set_fail(L_ENAMETOOLONG);
  WIN32_FILE_ATTRIBUTE_DATA attr{};
  if (!GetFileAttributesExW(wide, GetFileExInfoStandard, &attr)) {
    return set_fail(win32_to_errno(GetLastError()));
  }
  std::memset(out, 0, sizeof(*out));
  const DWORD a = attr.dwFileAttributes;
  const bool is_link =
      !follow_links && (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
  out->Mode = mode_for_attributes(a, is_link);
  out->Uid = 0;
  out->Gid = 0;
  out->Size = is_link
                  ? 0
                  : (int64_t)(((uint64_t)attr.nFileSizeHigh << 32) |
                              (uint64_t)attr.nFileSizeLow);
  fill_times(out, attr.ftCreationTime, attr.ftLastAccessTime,
             attr.ftLastWriteTime);
  if ((a & FILE_ATTRIBUTE_HIDDEN) != 0) out->UserFlags = kUserFlagHidden;
  // Dev/Ino need a handle; 0-access open, best-effort only.
  HANDLE h = open_file2(wide, 0,
                        FILE_SHARE_READ | FILE_SHARE_WRITE |
                            FILE_SHARE_DELETE,
                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
  if (h != INVALID_HANDLE_VALUE) {
    fill_dev_ino(h, out);
    CloseHandle(h);
  }
  return 0;
}

// ---- directory iteration state ------------------------------------------
struct DirState {
  HANDLE find = INVALID_HANDLE_VALUE;
  WIN32_FIND_DATAW data{};
  bool pending = false;  // data holds an unconsumed entry
};

}  // namespace

extern "C" {

// ---- error conversion (measured table; see sysnative_errno_table.inc) ---
int32_t SystemNative_ConvertErrorPlatformToPal(int32_t platform_errno) {
  if (platform_errno < 0 || platform_errno > 135) return 131071;
  return kErrnoToPal[platform_errno];
}
int32_t SystemNative_ConvertErrorPalToPlatform(int32_t pal_error) {
  // Reverse lookup; the table is injective (verified at generation).
  for (int i = 0; i < 136; ++i) {
    if (kErrnoToPal[i] == pal_error) return i;
  }
  return -1;
}

// ---- strerror ------------------------------------------------------------
uint8_t* SystemNative_StrErrorR(int32_t err, uint8_t* buf, int32_t cap) {
  static const struct {
    int32_t e;
    const char* msg;
  } kMessages[] = {
      {L_EPERM, "Operation not permitted"},
      {L_ENOENT, "No such file or directory"},
      {L_ESRCH, "No such process"},
      {L_EINTR, "Interrupted system call"},
      {L_EIO, "Input/output error"},
      {L_ENXIO, "No such device or address"},
      {L_E2BIG, "Argument list too long"},
      {L_ENOEXEC, "Exec format error"},
      {L_EBADF, "Bad file descriptor"},
      {L_EAGAIN, "Resource temporarily unavailable"},
      {L_ENOMEM, "Cannot allocate memory"},
      {L_EACCES, "Permission denied"},
      {L_EFAULT, "Bad address"},
      {L_EBUSY, "Device or resource busy"},
      {L_EEXIST, "File exists"},
      {L_EXDEV, "Invalid cross-device link"},
      {L_ENODEV, "No such device"},
      {L_ENOTDIR, "Not a directory"},
      {L_EISDIR, "Is a directory"},
      {L_EINVAL, "Invalid argument"},
      {L_ENFILE, "Too many open files in system"},
      {L_EMFILE, "Too many open files"},
      {L_ENOTTY, "Inappropriate ioctl for device"},
      {L_EFBIG, "File too large"},
      {L_ENOSPC, "No space left on device"},
      {L_ESPIPE, "Illegal seek"},
      {L_EROFS, "Read-only file system"},
      {L_EMLINK, "Too many links"},
      {L_EPIPE, "Broken pipe"},
      {L_ENAMETOOLONG, "File name too long"},
      {L_ENOSYS, "Function not implemented"},
      {L_ENOTEMPTY, "Directory not empty"},
      {L_ELOOP, "Too many levels of symbolic links"},
  };
  if (!buf || cap <= 0) return buf;
  const char* msg = nullptr;
  for (size_t i = 0; i < sizeof(kMessages) / sizeof(kMessages[0]); ++i) {
    if (kMessages[i].e == err) {
      msg = kMessages[i].msg;
      break;
    }
  }
  if (msg) {
    std::snprintf((char*)buf, (size_t)cap, "%s", msg);
  } else {
    std::snprintf((char*)buf, (size_t)cap, "Unknown error %d", (int)err);
  }
  return buf;
}

// ---- random bytes (BCrypt; bcrypt.lib is already linked for Mono) --------
void SystemNative_GetNonCryptographicallySecureRandomBytes(uint8_t* buf,
                                                           int32_t len) {
  if (!buf || len <= 0) return;
  NTSTATUS st = BCryptGenRandom(nullptr, buf, (ULONG)len,
                                BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (BCRYPT_SUCCESS(st)) return;
  // Fallback only (BCrypt essentially never fails): xorshift64 seeded
  // from the tick count + thread id. Better than zeros; not crypto.
  uint64_t s = (uint64_t)GetTickCount64() ^
               ((uint64_t)(uintptr_t)buf << 32) ^
               (uint64_t)GetCurrentThreadId() ^ 0x9E3779B97F4A7C15ULL;
  if (s == 0) s = 0x2545F4914F6CDD1DULL;
  for (int32_t i = 0; i < len; ++i) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    buf[i] = (uint8_t)(s >> 56);
  }
}

// ---- stat family (the File.Exists / FileInfo core) -----------------------
int32_t SystemNative_Stat2(const char* path, FileStatus* out) {
  return stat_impl(path, out, /*follow_links=*/true);
}
int32_t SystemNative_LStat2(const char* path, FileStatus* out) {
  return stat_impl(path, out, /*follow_links=*/false);
}
int32_t SystemNative_FStat2(intptr_t fd, FileStatus* out) {
  // fd is a MonoIO SafeFileHandle value: a Win32 HANDLE on this platform.
  if (!out) return set_fail(L_EINVAL);
  HANDLE h = (HANDLE)fd;
  if (h == nullptr || h == INVALID_HANDLE_VALUE) return set_fail(L_EBADF);
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(h, &info))
    return set_fail(win32_to_errno(GetLastError()));
  std::memset(out, 0, sizeof(*out));
  const DWORD a = info.dwFileAttributes;
  out->Mode = mode_for_attributes(a, false);
  out->Uid = 0;
  out->Gid = 0;
  out->Size = (int64_t)(((uint64_t)info.nFileSizeHigh << 32) |
                        (uint64_t)info.nFileSizeLow);
  fill_times(out, info.ftCreationTime, info.ftLastAccessTime,
             info.ftLastWriteTime);
  if ((a & FILE_ATTRIBUTE_HIDDEN) != 0) out->UserFlags = kUserFlagHidden;
  fill_dev_ino(h, out);
  return 0;
}

// ---- directory iteration -------------------------------------------------
// Managed contract (verified vs the real lib + managed IL):
//   OpenDir: DIR* or NULL + errno. ReadDirR: 0 = entry, -1 = end of dir
//   (Name NULL), +errno = error. CloseDir: 0 or -1 + errno.
intptr_t SystemNative_OpenDir(const char* path) {
  if (!path) {
    set_fail(L_EINVAL);
    return 0;
  }
  wchar_t wide[4096];
  if (!wpath(path, wide, 4096)) {
    set_fail(L_ENAMETOOLONG);
    return 0;
  }
  DWORD a = GetFileAttributesW(wide);
  if (a == INVALID_FILE_ATTRIBUTES) {
    const int e = win32_to_errno(GetLastError());
    log_fail("OpenDir", path, e);
    set_fail(e);
    return 0;
  }
  if ((a & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    set_fail(L_ENOTDIR);
    return 0;
  }
  wchar_t search[4100];
  size_t len = std::wcslen(wide);
  if (len + 3 >= 4100) {
    set_fail(L_ENAMETOOLONG);
    return 0;
  }
  std::memcpy(search, wide, (len + 1) * sizeof(wchar_t));
  if (len > 0 && search[len - 1] != L'\\') search[len++] = L'\\';
  search[len++] = L'*';
  search[len] = 0;
  DirState* st = (DirState*)std::malloc(sizeof(DirState));
  if (!st) {
    set_fail(L_ENOMEM);
    return 0;
  }
  st->find = INVALID_HANDLE_VALUE;
  st->pending = false;
  std::memset(&st->data, 0, sizeof(st->data));
  HANDLE h = FindFirstFileW(search, &st->data);
  if (h == INVALID_HANDLE_VALUE) {
    const int e = win32_to_errno(GetLastError());
    std::free(st);
    log_fail("OpenDir", path, e);
    set_fail(e);
    return 0;
  }
  st->find = h;
  st->pending = true;
  return (intptr_t)st;
}
int32_t SystemNative_GetReadDirRBufferSize() {
  // Managed code rents this many bytes and passes them as the name
  // scratch buffer. The real Linux value is 287; 4096 covers long
  // Windows names converted to UTF-8.
  return 4096;
}
int32_t SystemNative_ReadDirR(intptr_t dir, uint8_t* buffer,
                              int32_t buffer_size, DirectoryEntry* out) {
  if (!out) return L_EINVAL;
  out->Name = nullptr;
  out->NameLength = 0;
  out->InodeType = 0;
  DirState* st = (DirState*)dir;
  if (!st || st->find == INVALID_HANDLE_VALUE || !buffer ||
      buffer_size <= 0) {
    return L_EBADF;
  }
  WIN32_FIND_DATAW* d = nullptr;
  if (st->pending) {
    d = &st->data;
    st->pending = false;
  } else {
    if (!FindNextFileW(st->find, &st->data)) {
      DWORD e = GetLastError();
      if (e == ERROR_NO_MORE_FILES) return -1;  // end of directory
      return win32_to_errno(e);
    }
    d = &st->data;
  }
  // UTF-8 name into the caller buffer. NameLength is the REAL byte
  // length (the managed GetName also accepts -1 = "scan for NUL within
  // 256 bytes", but a real length is correct for long names too).
  int n = WideCharToMultiByte(CP_UTF8, 0, d->cFileName, -1, (char*)buffer,
                              buffer_size, nullptr, nullptr);
  if (n <= 0) return L_ENAMETOOLONG;
  out->Name = buffer;
  out->NameLength = n - 1;  // exclude NUL
  const DWORD a = d->dwFileAttributes;
  if ((a & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    out->InodeType = kDT_DIR;
  } else if ((a & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    out->InodeType = kDT_LNK;
  } else {
    out->InodeType = kDT_REG;
  }
  return 0;
}
int32_t SystemNative_CloseDir(intptr_t dir) {
  DirState* st = (DirState*)dir;
  if (!st || st->find == INVALID_HANDLE_VALUE) return set_fail(L_EBADF);
  FindClose(st->find);
  std::free(st);
  return 0;
}

// ---- open / close / read / write (handles are Win32 HANDLEs) -------------
intptr_t SystemNative_Open(const char* path, int32_t flags, int32_t mode) {
  (void)mode;  // Unix creation mode has no Windows equivalent.
  if (!path) return set_fail_ptr(L_EINVAL);
  wchar_t wide[4096];
  if (!wpath(path, wide, 4096)) return set_fail_ptr(L_ENAMETOOLONG);
  DWORD desired = 0;
  switch (flags & kO_ACCMODE) {
    case 0:  // O_RDONLY
      desired = GENERIC_READ;
      break;
    case kO_WRONLY:
      desired = GENERIC_WRITE;
      break;
    case kO_RDWR:
      desired = GENERIC_READ | GENERIC_WRITE;
      break;
    default:
      return set_fail_ptr(L_EINVAL);
  }
  DWORD disp = OPEN_EXISTING;
  const bool creat = (flags & kO_CREAT) != 0;
  const bool excl = (flags & kO_EXCL) != 0;
  const bool trunc = (flags & kO_TRUNC) != 0;
  if (creat && excl) {
    disp = CREATE_NEW;
  } else if (creat && trunc) {
    disp = CREATE_ALWAYS;
  } else if (creat) {
    disp = OPEN_ALWAYS;
  } else if (trunc) {
    disp = TRUNCATE_EXISTING;
  }
  // O_CLOEXEC / O_SYNC: no-ops on Windows (no fork/exec; writes are
  // synchronous enough for this surface).
  HANDLE h = open_file2(wide, desired,
                        FILE_SHARE_READ | FILE_SHARE_WRITE |
                            FILE_SHARE_DELETE,
                        disp, 0);
  if (h == INVALID_HANDLE_VALUE) {
    const int e = win32_to_errno(GetLastError());
    log_fail("Open", path, e);
    return set_fail_ptr(e);
  }
  return (intptr_t)h;
}
int32_t SystemNative_Close(intptr_t fd) {
  HANDLE h = (HANDLE)fd;
  if (h == nullptr || h == INVALID_HANDLE_VALUE) return set_fail(L_EBADF);
  if (!CloseHandle(h)) return set_fail(win32_to_errno(GetLastError()));
  return 0;
}
// System.Core pinvoke (NOT the System.dll Sys.Read icall — that one is
// FileSystemWatcher-only and stays unregistered by design). SafePipeHandle
// values are Win32 pipe HANDLEs here.
int32_t SystemNative_Read(intptr_t handle, uint8_t* buffer, int32_t count) {
  HANDLE h = (HANDLE)handle;
  if (h == nullptr || h == INVALID_HANDLE_VALUE) return set_fail(L_EBADF);
  if (!buffer || count < 0) return set_fail(L_EINVAL);
  if (count == 0) return 0;
  DWORD got = 0;
  if (!ReadFile(h, buffer, (DWORD)count, &got, nullptr)) {
    DWORD e = GetLastError();
    if (e == ERROR_BROKEN_PIPE) return 0;  // EOF on a pipe
    return set_fail(win32_to_errno(e));
  }
  return (int32_t)got;
}
int32_t SystemNative_Write(intptr_t handle, const uint8_t* buffer,
                           int32_t count) {
  HANDLE h = (HANDLE)handle;
  if (h == nullptr || h == INVALID_HANDLE_VALUE) return set_fail(L_EBADF);
  if (!buffer || count < 0) return set_fail(L_EINVAL);
  int32_t written = 0;
  while (written < count) {
    DWORD n = 0;
    if (!WriteFile(h, buffer + written, (DWORD)(count - written), &n,
                   nullptr)) {
      if (written > 0) return written;
      return set_fail(win32_to_errno(GetLastError()));
    }
    if (n == 0) break;
    written += (int32_t)n;
  }
  return written;
}

// ---- file copy (File.Copy has NO managed fallback: CheckIo throws on
// failure, so this is implemented for real, not stubbed) -------------------
int32_t SystemNative_CopyFile(intptr_t source, intptr_t destination) {
  HANDLE src = (HANDLE)source;
  HANDLE dst = (HANDLE)destination;
  if (src == nullptr || src == INVALID_HANDLE_VALUE || dst == nullptr ||
      dst == INVALID_HANDLE_VALUE) {
    return set_fail(L_EBADF);
  }
  // Copy from the current position to EOF (sendfile semantics).
  uint8_t* buf = (uint8_t*)std::malloc(32768);
  if (!buf) return set_fail(L_ENOMEM);
  int32_t result = 0;
  for (;;) {
    DWORD got = 0;
    if (!ReadFile(src, buf, 32768, &got, nullptr)) {
      result = set_fail(win32_to_errno(GetLastError()));
      break;
    }
    if (got == 0) break;  // EOF
    DWORD off = 0;
    while (off < got) {
      DWORD n = 0;
      if (!WriteFile(dst, buf + off, got - off, &n, nullptr)) {
        result = set_fail(win32_to_errno(GetLastError()));
        break;
      }
      if (n == 0) {
        result = set_fail(L_EIO);
        break;
      }
      off += n;
    }
    if (result != 0) break;
  }
  std::free(buf);
  return result;
}

// ---- directory / file mutation -------------------------------------------
int32_t SystemNative_MkDir(const char* path, int32_t mode) {
  (void)mode;
  if (!path) return set_fail(L_EINVAL);
  wchar_t wide[4096];
  if (!wpath(path, wide, 4096)) return set_fail(L_ENAMETOOLONG);
  if (!CreateDirectoryW(wide, nullptr)) {
    const int e = win32_to_errno(GetLastError());
    log_fail("MkDir", path, e);
    return set_fail(e);
  }
  return 0;
}
int32_t SystemNative_RmDir(const char* path) {
  if (!path) return set_fail(L_EINVAL);
  wchar_t wide[4096];
  if (!wpath(path, wide, 4096)) return set_fail(L_ENAMETOOLONG);
  if (!RemoveDirectoryW(wide)) {
    const int e = win32_to_errno(GetLastError());
    log_fail("RmDir", path, e);
    return set_fail(e);
  }
  return 0;
}
int32_t SystemNative_Rename(const char* old_path, const char* new_path) {
  if (!old_path || !new_path) return set_fail(L_EINVAL);
  wchar_t wold[4096];
  wchar_t wnew[4096];
  if (!wpath(old_path, wold, 4096) || !wpath(new_path, wnew, 4096))
    return set_fail(L_ENAMETOOLONG);
  if (!MoveFileExW(wold, wnew, MOVEFILE_REPLACE_EXISTING)) {
    const int e = win32_to_errno(GetLastError());
    log_fail("Rename", old_path, e);
    return set_fail(e);
  }
  return 0;
}
int32_t SystemNative_Unlink(const char* path) {
  if (!path) return set_fail(L_EINVAL);
  wchar_t wide[4096];
  if (!wpath(path, wide, 4096)) return set_fail(L_ENAMETOOLONG);
  DWORD a = GetFileAttributesW(wide);
  if (a != INVALID_FILE_ATTRIBUTES &&
      (a & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return set_fail(L_EISDIR);  // POSIX unlink on a directory
  }
  if (!DeleteFileW(wide)) {
    const int e = win32_to_errno(GetLastError());
    log_fail("Unlink", path, e);
    return set_fail(e);
  }
  return 0;
}
int32_t SystemNative_ChMod(const char* path, int32_t mode) {
  // Best-effort: Unix permission bits have no Windows equivalent beyond
  // the read-only attribute. No write bits -> read-only; else clear it.
  if (!path) return set_fail(L_EINVAL);
  wchar_t wide[4096];
  if (!wpath(path, wide, 4096)) return set_fail(L_ENAMETOOLONG);
  DWORD a = GetFileAttributesW(wide);
  if (a == INVALID_FILE_ATTRIBUTES)
    return set_fail(win32_to_errno(GetLastError()));
  const bool want_ro = (mode & 0222) == 0;
  DWORD next = want_ro ? (a | FILE_ATTRIBUTE_READONLY)
                       : (a & ~FILE_ATTRIBUTE_READONLY);
  if (next != a && !SetFileAttributesW(wide, next))
    return set_fail(win32_to_errno(GetLastError()));
  return 0;
}
int32_t SystemNative_UTime(const char* path, const UTimBuf* times) {
  if (!path || !times) return set_fail(L_EINVAL);
  wchar_t wide[4096];
  if (!wpath(path, wide, 4096)) return set_fail(L_ENAMETOOLONG);
  HANDLE h = open_file2(wide, FILE_WRITE_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE |
                            FILE_SHARE_DELETE,
                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
  if (h == INVALID_HANDLE_VALUE)
    return set_fail(win32_to_errno(GetLastError()));
  FILETIME at = unix_to_filetime(times->AcTime, 0);
  FILETIME mt = unix_to_filetime(times->ModTime, 0);
  BOOL ok = SetFileTime(h, nullptr, &at, &mt);
  DWORD e = ok ? ERROR_SUCCESS : GetLastError();
  CloseHandle(h);
  if (!ok) return set_fail(win32_to_errno(e));
  return 0;
}
int32_t SystemNative_UTimes(const char* path, const TimeValPair* times) {
  if (!path || !times) return set_fail(L_EINVAL);
  wchar_t wide[4096];
  if (!wpath(path, wide, 4096)) return set_fail(L_ENAMETOOLONG);
  HANDLE h = open_file2(wide, FILE_WRITE_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE |
                            FILE_SHARE_DELETE,
                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
  if (h == INVALID_HANDLE_VALUE)
    return set_fail(win32_to_errno(GetLastError()));
  FILETIME at = unix_to_filetime(times->ASec, times->AUSec * 1000);
  FILETIME mt = unix_to_filetime(times->MSec, times->MUSec * 1000);
  BOOL ok = SetFileTime(h, nullptr, &at, &mt);
  DWORD e = ok ? ERROR_SUCCESS : GetLastError();
  CloseHandle(h);
  if (!ok) return set_fail(win32_to_errno(e));
  return 0;
}

// ---- hostname (real: Dns.GetHostName Unix path) ---------------------------
// NOTE: GetComputerNameExW is DESKTOP-partition; GetComputerNameW is APP.
int32_t SystemNative_GetHostName(uint8_t* buffer, int32_t count) {
  if (!buffer || count <= 0) return set_fail(L_EINVAL);
  wchar_t name[256];
  DWORD len = 256;
  if (!GetComputerNameW(name, &len))
    return set_fail(win32_to_errno(GetLastError()));
  int n = WideCharToMultiByte(CP_UTF8, 0, name, -1, (char*)buffer, count,
                              nullptr, nullptr);
  if (n <= 0) return set_fail(L_ENAMETOOLONG);
  return 0;
}

// ---- domain-socket sizes (Unix sockets don't exist on the console, but
// the values are real Linux constants, measured from libmono-native.so) ---
void SystemNative_GetDomainSocketSizes(int32_t* address_family_size,
                                       int32_t* path_offset,
                                       int32_t* address_size) {
  if (address_family_size) *address_family_size = 2;
  if (path_offset) *path_offset = 108;
  if (address_size) *address_size = 110;
}

// ---- ids ------------------------------------------------------------------
uint32_t SystemNative_GetEGid() { return 0; }
uint32_t SystemNative_GetEUid() { return 0; }
int32_t SystemNative_SetEUid(uint32_t euid) {
  (void)euid;
  log_once("sysnative: SetEUid not available on console (ENOSYS)");
  return set_fail(L_ENOSYS);
}

// ---- fail-visible stubs (no console equivalent; each logs once) ----------
int32_t SystemNative_ReadLink(const char* path, uint8_t* buffer,
                              int32_t buffer_size) {
  (void)path;
  (void)buffer;
  (void)buffer_size;
  // No symlinks on the console: EINVAL is what readlink returns for a
  // non-link, and the managed CopyDanglingSymlink check depends on it.
  log_once("sysnative: ReadLink not available on console (EINVAL)");
  return set_fail(L_EINVAL);
}
int32_t SystemNative_Symlink(const char* target, const char* link) {
  (void)target;
  (void)link;
  log_once("sysnative: Symlink not available on console (ENOSYS)");
  return set_fail(L_ENOSYS);
}
int32_t SystemNative_Link(const char* source, const char* link) {
  (void)source;
  (void)link;
  log_once("sysnative: Link not available on console (ENOSYS)");
  return set_fail(L_ENOSYS);
}
int32_t SystemNative_LChflags(const char* path, uint32_t flags) {
  (void)path;
  (void)flags;
  log_once("sysnative: LChflags not available on console (ENOSYS)");
  return set_fail(L_ENOSYS);
}
int32_t SystemNative_LChflagsCanSetHiddenFlag() {
  // The Interop+Sys..cctor calls this and MUST succeed (returning 0 =
  // false). It is the only call in the .cctor (verified in IL).
  return 0;
}
intptr_t SystemNative_INotifyInit() {
  log_once("sysnative: INotifyInit not available on console (ENOSYS)");
  return set_fail_ptr(L_ENOSYS);
}
int32_t SystemNative_INotifyAddWatch(intptr_t fd, const char* path,
                                     uint32_t mask) {
  (void)fd;
  (void)path;
  (void)mask;
  log_once("sysnative: INotifyAddWatch not available on console (ENOSYS)");
  return set_fail(L_ENOSYS);
}
int32_t SystemNative_INotifyRemoveWatch(intptr_t fd, int32_t wd) {
  (void)fd;
  (void)wd;
  log_once(
      "sysnative: INotifyRemoveWatch not available on console (ENOSYS)");
  return set_fail(L_ENOSYS);
}
int32_t SystemNative_FLock(intptr_t fd, int32_t operation) {
  (void)fd;
  (void)operation;
  // Called from SafeFileHandle.ReleaseHandle with LOCK_UN; the result
  // is discarded there, so ENOSYS is harmless.
  log_once("sysnative: FLock not available on console (ENOSYS)");
  return set_fail(L_ENOSYS);
}
int32_t SystemNative_Poll(PollEvent* events, uint32_t num_events,
                          int32_t timeout, uint32_t* triggered) {
  (void)events;
  (void)num_events;
  (void)timeout;
  (void)triggered;
  // Returns an Interop/Error directly (not -1 + errno).
  log_once("sysnative: Poll not available on console (ENOSYS)");
  return kPalENOSYS;
}
int32_t SystemNative_Pipe(int32_t* fds, int32_t flags) {
  (void)fds;
  (void)flags;
  // Cannot be implemented soundly: fds are int32 but Win32 HANDLEs are
  // 64-bit, so returning real handles would truncate them. Mono's
  // Windows pipe code never calls this (Unix-only path).
  log_once("sysnative: Pipe not available on console (ENOSYS)");
  return set_fail(L_ENOSYS);
}
int32_t SystemNative_GetPeerID(intptr_t socket, uint32_t* peer_id) {
  (void)socket;
  (void)peer_id;
  log_once("sysnative: GetPeerID not available on console (ENOSYS)");
  return set_fail(L_ENOSYS);
}
const uint8_t* SystemNative_GetPeerUserName(intptr_t socket) {
  (void)socket;
  // NULL marshals as a null string; the marshaler never frees NULL.
  // (Returning a static "" would be freed by the marshaler -> crash.)
  log_once("sysnative: GetPeerUserName not available on console (NULL)");
  return nullptr;
}
int32_t SystemNative_FcntlGetPipeSz(intptr_t fd) {
  (void)fd;
  log_once("sysnative: FcntlGetPipeSz not available on console (ENOSYS)");
  return set_fail(L_ENOSYS);
}
int32_t SystemNative_FcntlSetPipeSz(intptr_t fd, int32_t size) {
  (void)fd;
  (void)size;
  log_once("sysnative: FcntlSetPipeSz not available on console (ENOSYS)");
  return set_fail(L_ENOSYS);
}
int32_t SystemNative_FcntlCanGetSetPipeSz() {
  // Like LChflagsCanSetHiddenFlag: a capability probe, 0 = no.
  return 0;
}
int32_t SystemNative_FcntlSetCloseOnExec(intptr_t fd) {
  (void)fd;
  // No-op success: Win32 handles are non-inheritable by default, so the
  // close-on-exec property already holds; there is nothing to set.
  return 0;
}

// ---- PAL init (System.dll Sys..cctor calls this and must succeed) --------
// On desktop Mono this registers the Interop/Sys::Read icall (FileSystem-
// Watcher/inotify only — see the header comment). No-op here by design.
void mono_pal_init() {}

}  // extern "C"

void system_native_fail(const char* name) {
  char line[256];
  std::snprintf(line, sizeof(line), "System.Native/FNA3D: %s not available", name);
  trt_core_log(line);
}

typedef int32_t (* ImgReadFn)(void*, char*, int32_t);
typedef void (* ImgSkipFn)(void*, int32_t);
typedef int32_t (* ImgEofFn)(void*);

// FNA3D_Image_Load: collect the managed stream into memory and decode
// with Tiramisu's PNG/DDS/JPEG loader (bridge_img.cpp). The UWP host
// does not compile FNA3D_Image.c (it needs SDL surfaces).
#include "bridge_img.h"
#include <vector>

extern "C" {
uint8_t* FNA3D_Image_Load(ImgReadFn readFn, ImgSkipFn skipFn, ImgEofFn eofFn,
                          void* context, int32_t* w, int32_t* h, int32_t* len,
                          int32_t forceW, int32_t forceH, uint8_t zoom) {
  (void)forceW; (void)forceH; (void)zoom;
  std::vector<char> bytes;
  char buf[8192];
  for (;;) {
    int n = readFn(context, buf, (int32_t)sizeof(buf));
    if (n <= 0) break;
    bytes.insert(bytes.end(), buf, buf + n);
    if (n < (int)sizeof(buf)) break;
  }
  trt_img img{};
  if (trt_img_parse(bytes.data(), (u32)bytes.size(), &img) != 0) {
    system_native_fail("FNA3D_Image_Load");
    return nullptr;
  }
  *w = (int32_t)img.w;
  *h = (int32_t)img.h;
  *len = (int32_t)(img.w * img.h * 4);
  return img.rgba;
}
void FNA3D_Image_Free(uint8_t* mem) { free(mem); }
void FNA3D_Image_SaveJPG() { system_native_fail("FNA3D_Image_SaveJPG"); }
void FNA3D_Image_SavePNG() { system_native_fail("FNA3D_Image_SavePNG"); }
}  // extern "C"

#endif  // _WIN32
