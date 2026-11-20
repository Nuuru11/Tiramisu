// tiramisu/third_party/mono-bridges/mono_dllmap_shim.cpp
// Host-side services the bridges expect, for the Linux harness .so
// files. The bridges (bridge_fmod.cpp, bridge_fmod_studio.cpp,
// bridge_steam.cpp) were written against the UWP host, which provides
// logging, a VFS, a voice mixer and pad state. This file provides
// Linux-harness equivalents so each .so is self-contained:
//
//   trt_core_log           -> stderr
//   trt_module_register    -> no-op (module registration is for the
//                             native PE loader; the .so never calls the
//                             trt_bridge_register_* functions)
//   trt_audio_*            -> voice-hook machinery mirroring
//                             bridge_audio.cpp: submit calls the hook
//                             installed by trt_audio_set_voice_hook
//                             (the harness test installs a counting
//                             hook; with no hook, PCM is dropped and
//                             submit returns 0)
//   trt_vfs_*              -> mini VFS: host absolute paths pass
//                             through; Windows drive paths
//                             (C:\users\...) map under
//                             $TIRAMISU_VFS_ROOT (default
//                             $HOME/.tiramisu-vfs); relative paths are
//                             CWD-relative. ".." components are rejected
//                             in drive-mapped paths.
//   trt_pad_*              -> no pads on the harness (connected=false,
//                             vibration is a no-op)
//
// Linux-only (POSIX mkdir/stat). NOT part of the UWP host build.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

#include "../../src/trt/bridge_audio.h"
#include "../../src/trt/input.h"
#include "../../src/trt/thunk.h"
#include "../../src/trt/trt.h"

extern "C" {

// ---- logging ----
void trt_core_log(const char* line) {
  fprintf(stderr, "%s\n", line ? line : "(null)");
}

// ---- module registration: no-op in the .so ----
void trt_module_register(const char* name, trt_route route,
                         const trt_named_fn* fns, int fn_count) {
  (void)name;
  (void)route;
  (void)fns;
  (void)fn_count;
}

// ---- audio voice hook (mirrors bridge_audio.cpp) ----
static trt_audio_voice_submit_fn g_voice_hook = nullptr;
static void* g_voice_user = nullptr;
static std::mutex g_voice_mu;

void trt_audio_set_voice_hook(trt_audio_voice_submit_fn fn, void* user) {
  std::lock_guard<std::mutex> lk(g_voice_mu);
  g_voice_hook = fn;
  g_voice_user = user;
}

int trt_audio_submit_voice(u64 voice_id, const void* pcm, u32 bytes, u32 rate,
                           u32 channels, u32 bytes_per_sample, u32 is_float,
                           float volume) {
  if (!bytes) return 0;
  trt_audio_voice_submit_fn hook;
  void* user;
  {
    std::lock_guard<std::mutex> lk(g_voice_mu);
    hook = g_voice_hook;
    user = g_voice_user;
  }
  if (hook) {
    hook(voice_id, pcm, bytes, rate, channels, bytes_per_sample, is_float,
         volume, user);
    return 1;
  }
  return 0;
}

// Single-voice hook pair (declared in bridge_audio.h; the bridges in
// this .so do not use it, but the surface is complete).
typedef int (*single_submit_fn)(const void* pcm, u32 bytes, u32 rate,
                                u32 channels, u32 bytes_per_sample,
                                void* user);
static single_submit_fn g_single_hook = nullptr;
static void* g_single_user = nullptr;

void trt_audio_set_hooks(int (*submit)(const void* pcm, u32 bytes, u32 rate,
                                       u32 channels, u32 bytes_per_sample,
                                       void* user),
                         void* user) {
  g_single_hook = submit;
  g_single_user = user;
}

void trt_audio_get_hooks(void (**submit)(const void* pcm, u32 bytes, u32 rate,
                                         u32 channels, u32 bytes_per_sample,
                                         void* user),
                         void** user) {
  // NOTE: bridge_audio.h declares get_hooks with a void-returning fn
  // pointer but set_hooks with an int-returning one. Cast to match the
  // declared out-param type; the return value is ignored by callers.
  if (submit)
    *submit = (void (*)(const void*, u32, u32, u32, u32, void*))g_single_hook;
  if (user) *user = g_single_user;
}

// ---- mini VFS ----
static std::string vfs_root() {
  const char* env = getenv("TIRAMISU_VFS_ROOT");
  if (env && env[0]) return env;
  const char* home = getenv("HOME");
  if (home && home[0]) return std::string(home) + "/.tiramisu-vfs";
  return "/tmp/.tiramisu-vfs";
}

// UTF-16 (BMP only — the bridges build paths from ASCII) -> UTF-8.
static std::string utf16_to_utf8(const u16* w) {
  std::string out;
  if (!w) return out;
  for (size_t i = 0; w[i]; ++i) {
    u16 c = w[i];
    if (c < 0x80) {
      out.push_back((char)c);
    } else if (c < 0x800) {
      out.push_back((char)(0xC0 | (c >> 6)));
      out.push_back((char)(0x80 | (c & 0x3F)));
    } else {
      out.push_back((char)(0xE0 | (c >> 12)));
      out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
      out.push_back((char)(0x80 | (c & 0x3F)));
    }
  }
  return out;
}

int trt_vfs_resolve_utf16_rw(const u16* guest_path, int for_write, char* out,
                             size_t out_cap) {
  if (!guest_path || !out || out_cap == 0) return -1;
  std::string p = utf16_to_utf8(guest_path);
  for (char& c : p)
    if (c == '\\') c = '/';
  std::string host;
  if (!p.empty() && p[0] == '/') {
    host = p;  // host absolute path: pass through
  } else if (p.size() >= 3 && p[1] == ':' && p[2] == '/') {
    // Drive path: map under the VFS root, rejecting ".." escapes.
    std::string rel = p.substr(3);
    if (rel.find("..") != std::string::npos) return -1;
    host = vfs_root() + "/" + rel;
  } else if (p.size() >= 2 && p[1] == ':') {
    return -1;  // drive-relative path: unsupported
  } else {
    host = p;  // relative: CWD-relative
  }
  if (host.size() + 1 > out_cap) return -1;
  memcpy(out, host.c_str(), host.size() + 1);
  if (!for_write) {
    struct stat st;
    if (stat(host.c_str(), &st) != 0) return -1;
  }
  return 0;
}

int trt_vfs_ensure_parents(const char* host_path) {
  if (!host_path) return -1;
  std::string p(host_path);
  size_t slash = p.rfind('/');
  if (slash == std::string::npos) return 0;
  std::string dir = p.substr(0, slash);
  if (dir.empty()) return 0;
  // mkdir -p
  std::string cur;
  size_t i = 0;
  if (dir[0] == '/') {
    cur = "/";
    i = 1;
  }
  while (i <= dir.size()) {
    size_t next = dir.find('/', i);
    if (next == std::string::npos) next = dir.size();
    if (next > i) {
      if (!cur.empty() && cur.back() != '/') cur += '/';
      cur += dir.substr(i, next - i);
      mkdir(cur.c_str(), 0755);  // EEXIST is fine
    }
    i = next + 1;
  }
  return 0;
}

int trt_vfs_stat(const char* host_path, int* is_dir, u64* size_out) {
  if (!host_path) return -1;
  struct stat st;
  if (stat(host_path, &st) != 0) return -1;
  if (is_dir) *is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
  if (size_out) *size_out = (u64)st.st_size;
  return 0;
}

// ---- pads: none on the harness ----
bool trt_pad_connected(u32 index) {
  (void)index;
  return false;
}
void trt_pad_set_vibration(u32 index, u16 left_motor, u16 right_motor) {
  (void)index;
  (void)left_motor;
  (void)right_motor;
}

}  // extern "C"
