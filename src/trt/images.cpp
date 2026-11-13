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
// Load-only mode (corpus dry-run): map + bind + seal guest DLLs but do NOT
// run their TLS callbacks or DllMain — the corpus test proves the load
// path, it does not execute game code. Default 1 = full boot semantics.
int g_run_image_code = 1;

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
  if (!handle) return nullptr;
  // STAGE 7: HMODULE == image base (guest code dereferences handles as a
  // PE header pointer); fall back to the legacy synthetic tag
  const u64 v = (u64)(uintptr_t)handle;
  for (int i = 0; i < g_image_count; ++i)
    if ((u64)(uintptr_t)g_images[i].img->base == v) return g_images[i].img;
  if ((v & 0xFFF00000u) != TRT_HANDLE_TAG) return nullptr;
  const int idx = (int)(v & 0xFFFFu);
  if (idx >= g_image_count) return nullptr;
  return g_images[idx].img;
}

trt_image* trt_images_first(void) {
  return g_image_count > 0 ? g_images[0].img : nullptr;
}

void trt_images_register_main(trt_image* img, const char* guest_name) {
  if (!img || g_image_count >= TRT_MAX_GUEST_IMAGES) return;
  const char* base = base_name(guest_name ? guest_name : "main.exe");
  guest_image& g = g_images[g_image_count];
  memset(&g, 0, sizeof(g));
  snprintf(g.name, sizeof(g.name), "%s", base);
  snprintf(g.path, sizeof(g.path), "<mapped>");
  g.img = img;
  // STAGE 7: HMODULE == image base on Windows (guest code dereferences it:
  // UnityCrashHandler64's CRT does cmp $0x5a4d,(%rax) on the handle right
  // after GetModuleHandleW — a synthetic tag faults there)
  g.handle = (void*)(uintptr_t)img->base;
  ++g_image_count;
}

// enumeration for the psapi/dbghelp bridges (the REAL guest module list)
int trt_images_count(void) { return g_image_count; }

trt_image* trt_images_nth(int idx) {
  return (idx >= 0 && idx < g_image_count) ? g_images[idx].img : nullptr;
}

const char* trt_images_nth_name(int idx) {
  return (idx >= 0 && idx < g_image_count) ? g_images[idx].name : nullptr;
}

const char* trt_images_nth_path(int idx) {
  return (idx >= 0 && idx < g_image_count) ? g_images[idx].path : nullptr;
}

void* trt_images_handle(trt_image* img) {
  for (int i = 0; i < g_image_count; ++i)
    if (g_images[i].img == img) return g_images[i].handle;
  return nullptr;
}

const char* trt_images_name(void* handle) {
  if (handle) {
    // STAGE 7: HMODULE == image base; fall back to the legacy tag
    const u64 v = (u64)(uintptr_t)handle;
    for (int i = 0; i < g_image_count; ++i)
      if ((u64)(uintptr_t)g_images[i].img->base == v) return g_images[i].name;
    if ((v & 0xFFF00000u) != TRT_HANDLE_TAG) return nullptr;
    const int idx = (int)(v & 0xFFFFu);
    if (idx >= g_image_count) return nullptr;
    return g_images[idx].name;
  }
  return nullptr;
}

// ---- synthetic system-module handles (STAGE 7, OrTMM) ---------------------
// LoadLibraryW("kernel32.dll") / GetModuleHandleW("api-ms-win-core-...") on
// modules the runtime implements but that exist as no host file. The handle
// is a TRT_HANDLE_TAG-tagged value registered in a small side table so
// GetProcAddress can route back to the module table and psapi can list it.
#define TRT_MAX_SYNTH_MODULES 32
struct synth_module {
  char name[64];
  void* handle;
};
synth_module g_synth[TRT_MAX_SYNTH_MODULES];
int g_synth_count = 0;

void* trt_images_synthetic_handle(const char* module_name) {
  if (!module_name || !module_name[0]) return nullptr;
  // stable: same name -> same handle for the process lifetime
  for (int i = 0; i < g_synth_count; ++i)
    if (ieq(g_synth[i].name, module_name)) return g_synth[i].handle;
  if (g_synth_count >= TRT_MAX_SYNTH_MODULES) return nullptr;
  synth_module& s = g_synth[g_synth_count];
  memset(&s, 0, sizeof(s));
  snprintf(s.name, sizeof(s.name), "%s", module_name);
  s.handle = (void*)(uintptr_t)(TRT_HANDLE_TAG |
                               (u64)(0x1000 + g_synth_count));
  ++g_synth_count;
  return s.handle;
}

int trt_images_is_synthetic(void* handle) {
  if (!handle) return 0;
  const u64 v = (u64)(uintptr_t)handle;
  if ((v & 0xFFF00000u) != TRT_HANDLE_TAG) return 0;
  const int idx = (int)(v & 0xFFFFu) - 0x1000;
  return idx >= 0 && idx < g_synth_count;
}

const char* trt_images_synth_name(void* handle) {
  if (!trt_images_is_synthetic(handle)) return nullptr;
  const int idx = (int)(((u64)(uintptr_t)handle) & 0xFFFFu) - 0x1000;
  return g_synth[idx].name;
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
  // STAGE 7 guard: re-entrant trt_load_pe (the exe's static CRT init or a
  // DllMain loads a DLL) would run tls callbacks again for the SAME image
  // object that already ran them — each pass burns a TEB TLS index and the
  // exe's index slot would end up pointing at the FIRST block while later
  // callbacks write later blocks. The load is the same file: skip the
  // callback/entry re-run, return the already-mapped image.
  for (int i = 0; i < g_image_count; ++i)
    if (strcmp(g_images[i].path, host_path) == 0 ||
        strcmp(g_images[i].name, base_name(guest_name ? guest_name : host_path)) == 0) {
      char line[320];
      snprintf(line, sizeof(line), "trt: load_pe %s -> already mapped (slot %d), re-entry guard",
               g_images[i].name, i);
      trt_core_log(line);
      return g_images[i].img;
    }
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
    // STAGE 7: HMODULE == image base (see trt_images_register_main)
    g.handle = (void*)(uintptr_t)img->base;
    ++g_image_count;
  }

  if (trt_seal_image(g_be, img) != TRT_OK) {
    char line[320];
    snprintf(line, sizeof(line), "trt: seal failed for %s", base);
    trt_core_log(line);
    return nullptr;
  }

  // TLS callbacks, then the entry point (DllMain for a DLL) — skipped in
  // load-only mode (corpus dry-run proves binding without executing code)
  if (g_run_image_code)
    for (int i = 0; i < img->tls_callback_count; ++i) {
      void* cb = img->base + img->tls_callback_rvas[i];
      trt_call_guest_img(cb, img, (u64)(uintptr_t)img->base, 1, 0, 0);
    }
  if (run_entry && g_run_image_code && img->entry_rva) {
    void* handle = trt_images_handle(img);
    char line[320];
    snprintf(line, sizeof(line),
             "trt: %s entry call handle=%p reason=1 (DLL_PROCESS_ATTACH)",
             base, handle);
    trt_core_log(line);
    const u64 rc_entry = trt_call_guest_img(img->base + img->entry_rva, img,
                                        (u64)(uintptr_t)handle, 1, 0, 0);
    snprintf(line, sizeof(line), "trt: %s entry returned 0x%llx", base,
             (unsigned long long)rc_entry);
    trt_core_log(line);
  }
  return img;
}

// Load-only mode toggle: 0 = map/bind/seal without running TLS callbacks or
// DllMain (corpus dry-run), 1 (default) = full boot semantics.
void trt_set_image_code_run(int enable) { g_run_image_code = enable ? 1 : 0; }

void trt_images_reset(void) { g_image_count = 0; }

// ---------------------------------------------------------------------------
// calling guest code from host code
// ---------------------------------------------------------------------------

u64 trt_call_guest(void* fn, u64 a, u64 b, u64 c, u64 d) {
  return trt_call_guest_img(fn, nullptr, a, b, c, d);
}

u64 trt_call_guest_img(void* fn, trt_image* img, u64 a, u64 b, u64 c, u64 d) {
  if (!fn) return 0;
#if defined(_WIN32)
  // the host IS Win64: the guest ABI is the host ABI
  using guest_fn = u64 (*)(u64, u64, u64, u64);
  (void)img;
  return ((guest_fn)fn)(a, b, c, d);
#else
  // Linux test harness: the host is SysV, the guest is Win64. Marshal
  // rcx/rdx/r8/r9 and keep the stack correct for the Win64 callee:
  //   - Win64 requires rsp % 16 == 0 AT THE CALL INSTRUCTION, so the callee
  //     enters with rsp % 16 == 8. The old "subq $40" left rsp % 16 == 8 at
  //     the call (entry rsp % 16 == 0) — the guest booted "fine" until real
  //     MSVC code (dstorage.dll DllMain -> QueryPerformanceCounter) ran a
  //     movaps prologue and faulted. STAGE 6 run-depth found this; the
  //     load-only corpus never could (it never executed the entry).
  //   - 32 bytes of shadow space, exactly what Win64 guarantees.
  //   - the args are read through MEMORY constraints, because with a
  //    register constraint the compiler may allocate the base pointer to
  //    rcx and the first "mov 0(%[buf]), %rcx" would destroy the pointer
  //    before the next argument is read;
  //  - the args are read BEFORE %rsp is adjusted. The compiler computes
  //    the memory addresses relative to the current %rsp, so touching %rsp
  //    first makes every address stale (the guest then gets garbage).
  u64 args[4] = {a, b, c, d};
  u64 ret = 0;
  // STAGE 7: if the image has a guest TEB (TLS directory), point the guest
  // gs base at it for the duration of the call and restore the host gs
  // base after (the host is libc/glibc: its gs base is the thread pointer
  // and MUST come back). r11 is caller-saved in Win64, so it also carries
  // the saved host gs base across the guest call.
  //
  // STAGE 7 (second): the harness rsp is NOT guaranteed 16-aligned at this
  // asm. Win64 requires rsp % 16 == 0 AT the call instruction; a skewed
  // caller makes the ENTIRE guest stack skewed and the first movaps in any
  // bridge function faults (measured live under gdb: UnityCrashHandler64
  // CRT startup -> QueryPerformanceCounter -> movaps %xmm15,0xa0(%rsp)
  // SIGSEGV). Align rsp, save the original on the guest stack, restore
  // with popq %rsp (pop into rsp: the +8 is overridden by the popped value).
  const u64 teb =
      (img && img->guest_teb) ? (u64)(uintptr_t)img->guest_teb
      // STAGE 7: even images without their own TLS directory get the shared
      // process TEB when it exists — UnityPlayer reads gs:[0x10] for stack
      // checks inside UnityMain2 even when the exe has no TLS dir.
      // STAGE 7 (second): acquire it LAZILY — a child that maps ONE image
      // with no TLS dir (dstorage.dll in the corpus DLL phase) previously
      // got teb=0 and ran on the HOST gs base; the MSVC stack probe
      // `mov r11,gs:0x10` faulted at address 0x10 (measured live).
          : (u64)(uintptr_t)trt_guest_teb_acquire(nullptr);
  asm volatile(
      "movq %[a0], %%rcx\n"
      "movq %[a1], %%rdx\n"
      "movq %[a2], %%r8\n"
      "movq %[a3], %%r9\n"
      "movq %%rsp, %%r10\n"
      "andq $-16, %%rsp\n"
      "subq $8, %%rsp\n"
      "pushq %%r10\n"             // original rsp, recovered with popq %%rsp
      "subq $32, %%rsp\n"
      "testq %[teb], %[teb]\n"
      "jnz 7f\n"
      "call *%[fn]\n"
      "jmp 8f\n"
      "7:\n"
      "rdgsbase %%r11\n"          // save the host thread pointer
      "wrgsbase %[teb]\n"         // guest gs -> the TEB
      "call *%[fn]\n"
      "wrgsbase %%r11\n"          // host gs back (any guest clobber dies here)
      "8:\n"
      "addq $32, %%rsp\n"
      "popq %%rsp\n"              // exact original rsp back
      : "=a"(ret)
      : [a0] "m"(args[0]), [a1] "m"(args[1]), [a2] "m"(args[2]),
        [a3] "m"(args[3]), [fn] "r"(fn), [teb] "r"(teb)
      : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory", "cc");
  return ret;
#endif
}

} // extern "C"
