// tiramisu/src/trt/images.cpp
// Guest image registry, on-demand DLL loading, and the Win64 call helper.
#include "images.h"
#include "pe_exports.h"
#include "thunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

#define TRT_MAX_GUEST_IMAGES 64
#define TRT_HANDLE_TAG 0x4D000000u

struct guest_image {
  char name[64];      // "engine.dll"
  char path[512];     // host path it came from
  trt_image* img;
  void* handle;
};

guest_image g_images[TRT_MAX_GUEST_IMAGES];
int g_image_count = 0;

const trt_backend* g_be = nullptr;
trt_resolver g_rs;
char g_search_dir[512] = "";
int g_load_depth = 0;

char lower_of(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

bool ieq(const char* a, const char* b) {
  while (*a && *b) {
    if (lower_of(*a) != lower_of(*b)) return false;
    ++a; ++b;
  }
  return *a == *b;
}

const char* base_name(const char* path) {
  const char* slash = strrchr(path, '\\');
  if (!slash) slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

u8* read_whole_file(const char* path, size_t* size) {
  FILE* f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0) { fclose(f); return nullptr; }
  u8* buf = (u8*)malloc((size_t)n);
  const size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  *size = got;
  return buf;
}

} // namespace

extern "C" {

// ---------------------------------------------------------------------------
// runtime context
// ---------------------------------------------------------------------------

void trt_runtime_init(const trt_backend* be, const char* search_dir) {
  g_be = be;
  if (search_dir) snprintf(g_search_dir, sizeof(g_search_dir), "%s", search_dir);
  g_rs.user = nullptr;
  g_rs.resolve = &trt_resolve_full;
}

const trt_backend* trt_runtime_backend(void) { return g_be; }

const char* trt_runtime_search_dir(void) { return g_search_dir; }

trt_resolver* trt_runtime_resolver(void) { return &g_rs; }

// ---------------------------------------------------------------------------
// registry
// ---------------------------------------------------------------------------

trt_image* trt_images_find(const char* name) {
  if (!name) return nullptr;
  const char* base = base_name(name);
  for (int i = 0; i < g_image_count; ++i)
    if (ieq(g_images[i].name, base)) return g_images[i].img;
  return nullptr;
}

trt_image* trt_images_by_handle(void* handle) {
  const u32 v = (u32)(uintptr_t)handle;
  if ((v & 0xFFF00000u) != TRT_HANDLE_TAG) return nullptr;
  const int idx = (int)(v & 0xFFFFu);
  if (idx >= g_image_count) return nullptr;
  return g_images[idx].img;
}

trt_image* trt_images_first(void) {
  return g_image_count > 0 ? g_images[0].img : nullptr;
}

void* trt_images_handle(trt_image* img) {
  for (int i = 0; i < g_image_count; ++i)
    if (g_images[i].img == img) return g_images[i].handle;
  return nullptr;
}

const char* trt_images_name(void* handle) {
  const u32 v = (u32)(uintptr_t)handle;
  if ((v & 0xFFF00000u) != TRT_HANDLE_TAG) return nullptr;
  const int idx = (int)(v & 0xFFFFu);
  if (idx >= g_image_count) return nullptr;
  return g_images[idx].name;
}

// ---------------------------------------------------------------------------
// resolver: guest images first, then the module table
// ---------------------------------------------------------------------------

// Try to load a DLL the game ships with us (from the game's directory).
static trt_image* try_load_sibling(const char* module) {
  if (!g_be || !g_search_dir[0] || g_load_depth > 8) return nullptr;
  const char* base = base_name(module);
  char host[1024];
  snprintf(host, sizeof(host), "%s/%s", g_search_dir, base);
  FILE* probe = fopen(host, "rb");
  if (!probe) return nullptr;
  fclose(probe);
  char line[320];
  snprintf(line, sizeof(line), "trt: loading guest DLL %s", base);
  trt_core_log(line);
  return trt_load_pe(host, base, 1);
}

void* trt_resolve_full(void* user, const char* module, const char* symbol,
                       u16 ordinal) {
  (void)user;
  if (!module) return nullptr;

  // 1. a guest image we already loaded (the game's own DLLs win over the
  //    system's, exactly like the Windows loader's search order)
  trt_image* img = trt_images_find(module);
  if (!img) {
    // 2. a DLL the game ships that we have not loaded yet
    img = try_load_sibling(module);
  }
  if (img) {
    void* fn = trt_pe_find_export(g_be, &g_rs, img, symbol, ordinal);
    if (fn) return fn;
    // the image does not export it: fall through to the module table
  }

  // 3. system modules (builtin bridges, api-set contracts, console DLLs)
  extern void* trt_resolve_module(void* user, const char* module,
                                  const char* symbol, u16 ordinal);
  return trt_resolve_module(nullptr, module, symbol, ordinal);
}

// ---------------------------------------------------------------------------
// loading
// ---------------------------------------------------------------------------

trt_image* trt_load_pe(const char* host_path, const char* guest_name,
                       int run_entry) {
  if (!g_be || !host_path) return nullptr;
  size_t size = 0;
  u8* file = read_whole_file(host_path, &size);
  if (!file) {
    char line[320];
    snprintf(line, sizeof(line), "trt: cannot read %s", host_path);
    trt_core_log(line);
    return nullptr;
  }

  trt_image* img = nullptr;
  ++g_load_depth;
  const int rc = trt_map_pe(g_be, &g_rs, file, size, &img);
  --g_load_depth;
  free(file);
  if (rc != TRT_OK || !img) {
    char line[320];
    snprintf(line, sizeof(line), "trt: guest image %s failed to map (%s)",
             guest_name ? guest_name : host_path, trt_errstr(rc));
    trt_core_log(line);
    if (img) trt_unmap(g_be, img);
    return nullptr;
  }

  // register BEFORE the entry runs: the DLL may load another DLL that
  // imports back into this one
  const char* base = base_name(guest_name ? guest_name : host_path);
  if (g_image_count < TRT_MAX_GUEST_IMAGES) {
    guest_image& g = g_images[g_image_count];
    memset(&g, 0, sizeof(g));
    snprintf(g.name, sizeof(g.name), "%s", base);
    snprintf(g.path, sizeof(g.path), "%s", host_path);
    g.img = img;
    g.handle = (void*)(uintptr_t)(TRT_HANDLE_TAG | (u32)g_image_count);
    ++g_image_count;
  }

  if (trt_seal_image(g_be, img) != TRT_OK) {
    char line[320];
    snprintf(line, sizeof(line), "trt: seal failed for %s", base);
    trt_core_log(line);
    return nullptr;
  }

  // TLS callbacks, then the entry point (DllMain for a DLL)
  for (int i = 0; i < img->tls_callback_count; ++i) {
    void* cb = img->base + img->tls_callback_rvas[i];
    trt_call_guest(cb, (u64)(uintptr_t)img->base, 1, 0, 0);
  }
  if (run_entry && img->entry_rva) {
    void* handle = trt_images_handle(img);
    char line[320];
    snprintf(line, sizeof(line),
             "trt: %s entry call handle=%p reason=1 (DLL_PROCESS_ATTACH)",
             base, handle);
    trt_core_log(line);
    const u64 rc_entry = trt_call_guest(img->base + img->entry_rva,
                                        (u64)(uintptr_t)handle, 1, 0, 0);
    snprintf(line, sizeof(line), "trt: %s entry returned 0x%llx", base,
             (unsigned long long)rc_entry);
    trt_core_log(line);
  }
  return img;
}

// ---------------------------------------------------------------------------
// calling guest code from host code
// ---------------------------------------------------------------------------

u64 trt_call_guest(void* fn, u64 a, u64 b, u64 c, u64 d) {
  if (!fn) return 0;
#if defined(_WIN32)
  // the host IS Win64: the guest ABI is the host ABI
  using guest_fn = u64 (*)(u64, u64, u64, u64);
  return ((guest_fn)fn)(a, b, c, d);
#else
  // Linux test harness: the host is SysV, the guest is Win64. Marshal
  // rcx/rdx/r8/r9 and keep the stack 16-byte aligned for the callee.
  //
  // Two traps this avoids (both bit us, both were silent):
  //  - the args are read through MEMORY constraints, because with a
  //    register constraint the compiler may allocate the base pointer to
  //    rcx and the first "mov 0(%[buf]), %rcx" would destroy the pointer
  //    before the next argument is read;
  //  - the args are read BEFORE %rsp is adjusted. The compiler computes
  //    the memory addresses relative to the current %rsp, so touching %rsp
  //    first makes every address stale (the guest then gets garbage).
  u64 args[4] = {a, b, c, d};
  u64 ret = 0;
  asm volatile(
      "movq %[a0], %%rcx\n"
      "movq %[a1], %%rdx\n"
      "movq %[a2], %%r8\n"
      "movq %[a3], %%r9\n"
      "subq $40, %%rsp\n"
      "call *%[fn]\n"
      "addq $40, %%rsp\n"
      : "=a"(ret)
      : [a0] "m"(args[0]), [a1] "m"(args[1]), [a2] "m"(args[2]),
        [a3] "m"(args[3]), [fn] "r"(fn)
      : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory", "cc");
  return ret;
#endif
}

} // extern "C"
