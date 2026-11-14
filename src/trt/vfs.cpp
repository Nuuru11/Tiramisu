// tiramisu/src/trt/vfs.cpp
// Virtual filesystem: the guest sees a Windows world; Tiramisu translates.
//   C:\ -> <writable>/drive_c      (game installs, saves, registry file)
//   Z:\ -> package install dir     (read-only: the game's own files)
// Rules are data, so per-game overrides ship as JSON later, not code.
#include "trt.h"

#include <stdio.h>
#include <string.h>

namespace {

char lower_of(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

struct vfs_mount {
  char prefix[8];   // lower-case ASCII drive prefix, e.g. "c:"
  char target[512]; // host path (UTF-8)
};
vfs_mount g_mounts[8];
int g_mount_count = 0;

// guest current-working-directory drive (Win32 relative paths resolve
// against it). PyInstaller extracts its archive via RELATIVE paths
// (".\\_MEI42422\\NoThumbnail.png"): with no cwd rule every relative
// path failed with PATH_NOT_FOUND and extraction aborted. Defaults to
// the Z: drive (the exe's dir); if Z: is not mounted, C:.
char g_cwd_drive[8] = "z:";

void utf16_to_utf8(const u16* in, char* out, size_t out_cap) {
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 4 <= out_cap; ++i) {
    u32 c = in[i];
    if (c >= 0xD800u && c < 0xDC00u && in[i + 1] >= 0xDC00u && in[i + 1] < 0xE000u) {
      c = 0x10000u + ((c - 0xD800u) << 10) + (in[i + 1] - 0xDC00u);
      ++i;
    }
    if (c < 0x80) out[o++] = (char)c;
    else if (c < 0x800) {
      out[o++] = (char)(0xC0 | (c >> 6));
      out[o++] = (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out[o++] = (char)(0xE0 | (c >> 12));
      out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
      out[o++] = (char)(0x80 | (c & 0x3F));
    } else {
      out[o++] = (char)(0xF0 | (c >> 18));
      out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
      out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
      out[o++] = (char)(0x80 | (c & 0x3F));
    }
  }
  out[o] = 0;
}

} // namespace

extern "C" {

// UTF-16 -> UTF-8 (shared with the loader bridge: LoadLibraryW paths)
int trt_utf16_to_utf8(const u16* in, char* out, size_t out_cap) {
  if (!in || !out || out_cap == 0) return -1;
  utf16_to_utf8(in, out, out_cap);
  return 0;
}

void trt_vfs_mount(const char* drive_prefix, const char* host_path) {
  if (g_mount_count >= 8) return;
  vfs_mount& m = g_mounts[g_mount_count++];
  memset(&m, 0, sizeof(m));
  strncpy(m.prefix, drive_prefix, sizeof(m.prefix) - 1);
  strncpy(m.target, host_path, sizeof(m.target) - 1);
  // the guest cwd drive: Z: when mounted (exe's dir), else the first mount
  if (strcmp(m.prefix, "z:") == 0 || g_mount_count == 1)
    strncpy(g_cwd_drive, m.prefix, sizeof(g_cwd_drive) - 1);
}

// translate "C:\foo\bar" -> "<host>/foo/bar", and relative paths like
// ".\_MEI42422\file" or "sub\\file" -> "<cwd-drive>/foo/bar".
// Returns 0 on hit, -1 no mount.
int trt_vfs_resolve_utf16(const u16* guest_path, char* out, size_t out_cap) {
  // Fail-visible: real DllMain code (baselib.dll, UnityPlayer.dll) calls
  // path APIs with NULL and expects the error return, never a fault. The
  // internal utf16_to_utf8 is unchecked on purpose; the public boundary
  // guards so a NULL guest path becomes a named error, not a SIGSEGV.
  if (!guest_path || !out || out_cap == 0) return -1;
  char utf8[1024];
  utf16_to_utf8(guest_path, utf8, sizeof(utf8));
  // strip a leading ".\" / "./" (Win32 "current directory" prefix)
  const char* p = utf8;
  if (p[0] == '.' && (p[1] == '\\' || p[1] == '/')) p += 2;
  if (lower_of(p[0]) >= 'a' && lower_of(p[0]) <= 'z' && p[1] == ':') {
    char drv[3] = {(char)lower_of(p[0]), ':', 0};
    const char* rest = p + 2;
    if (*rest == '\\') ++rest;
    for (int i = 0; i < g_mount_count; ++i) {
      if (strcmp(g_mounts[i].prefix, drv) == 0) {
        snprintf(out, out_cap, "%s%s%s", g_mounts[i].target,
                 *rest ? "/" : "", rest);
        // normalise backslashes the guest may have mixed in
        for (char* q = out; *q; ++q)
          if (*q == '\\') *q = '/';
        return 0;
      }
    }
    return -1;  // named drive, no mount: fail-visible (not silently cwd)
  }
  // RELATIVE guest path (no drive): resolve against the guest cwd drive.
  // PyInstaller extracts via ".\\_MEIxxxx\\entry" — without this rule the
  // write failed PATH_NOT_FOUND and extraction aborted (named blocker:
  // "Failed to extract NoThumbnail.png: failed to open target file!").
  for (int i = 0; i < g_mount_count; ++i) {
    if (strcmp(g_mounts[i].prefix, g_cwd_drive) == 0) {
      snprintf(out, out_cap, "%s%s%s", g_mounts[i].target,
               *p ? "/" : "", p);
      for (char* q = out; *q; ++q)
        if (*q == '\\') *q = '/';
      return 0;
    }
  }
  return -1;
}

} // extern "C"
