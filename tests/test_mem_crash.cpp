// tiramisu/tests/test_mem_crash.cpp
// Wave 5 Agent B — Sprint 2 item 35 (MapViewOfFile live-view ceiling + LRU
// eviction, TRT_MAX_MAPS, use-after-evict visibility) + Sprint 3 item 46
// remainder (SetUnhandledExceptionFilter storage + fatal-path wiring).
//
// Runs ARGLESS: every surface is exercised host-side through the bridge
// entry points (the same functions the guest IAT resolves to). The fatal
// path is driven through trt_raise_exception_sysv with a synthetic Win64
// CONTEXT, and _exit(42) is intercepted the documented way — the TEST-HARNESS
// ONLY trt_boot_raise_trap longjmp (see trt.h; the console host never
// installs it, so console semantics are unchanged).
//
// The two TRT_MAX_MAPS clamp probes re-exec this binary in a fresh process
// (the ceiling is parsed once per process, so a fresh env needs a fresh exec).
#include "../src/trt/trt.h"

#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>
#include <vector>

extern "C" {
int trt_bridge_register_kernel32(void);
void trt_vfs_mount(const char* drive, const char* host_dir);

u64 TRT_STDCALL trt_CreateFileMappingW(u64 hfile, void* attrs, u32 protect,
                                       u32 max_high, u32 max_low,
                                       const u16* name);
void* TRT_STDCALL trt_MapViewOfFile(u64 h, u32 access, u32 off_high,
                                    u32 off_low, u64 bytes);
void* TRT_STDCALL trt_MapViewOfFileEx(u64 h, u32 access, u32 off_high,
                                      u32 off_low, u64 bytes, void* base_addr);
int TRT_STDCALL trt_UnmapViewOfFile(void* view);
int TRT_STDCALL trt_FlushViewOfFile(void* view, u64 bytes);
int trt_mem_close_handle(void* h);
void trt_mem_stats(u32* active_mappings, u32* active_views,
                   u64* total_view_bytes);
u32 TRT_STDCALL trt_GetLastError(void);
void* TRT_STDCALL trt_CreateFileW(const u16* path, u32 access, u32 share,
                                  void* security, u32 disposition, u32 flags,
                                  void* template_handle);
int TRT_STDCALL trt_CloseHandle(void* h);

// item 46 surfaces
void* TRT_STDCALL trt_SetUnhandledExceptionFilter(void* filter);
void* TRT_STDCALL trt_UnhandledExceptionFilter(void* info);
// the terminal unhandled-exception path (bridge_kernel32.cpp): called by the
// naked trt_RaiseException thunk with the captured Win64 CONTEXT
void trt_raise_exception_sysv(u32 code, u32 flags, u32 count, const u64* args,
                              const u8* caller_ctx);
extern void (*trt_boot_raise_trap)(u32 code);  // defined in bridge_kernel32.cpp
}

// ---------------------------------------------------------------------------
// harness
// ---------------------------------------------------------------------------

namespace {

std::vector<std::string> g_log;
void log_capture(const char* line) { g_log.emplace_back(line); }
void log_quiet(const char*) {}

// backend extent registry: alloc records, release munmaps the real length
struct extent { void* base; size_t len; };
extent g_extents[4096 + 8];
int g_extent_count = 0;

int log_count(const char* substr) {
  int n = 0;
  for (const std::string& l : g_log) {
    if (l.find(substr) != std::string::npos) ++n;
  }
  return n;
}
bool log_contains(const char* substr) { return log_count(substr) > 0; }

int failures = 0;
#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (cond) printf("  PASS %s\n", msg);                            \
    else { printf("  FAIL %s\n", msg); ++failures; }                 \
  } while (0)

void* mmap_alloc(size_t bytes, u64 preferred) {
  (void)preferred;
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) return nullptr;
  // record the extent so release can munmap the REAL length (munmap with
  // length 0 fails and the pages would silently stay mapped)
  if (g_extent_count < 4096 + 8) {
    g_extents[g_extent_count].base = p;
    g_extents[g_extent_count].len = bytes;
    ++g_extent_count;
  }
  return p;
}
int mmap_protect(void* base, size_t bytes, unsigned prot) {
  int p = 0;
  if (prot & TRT_PROT_R) p |= PROT_READ;
  if (prot & TRT_PROT_W) p |= PROT_WRITE;
  if (prot & TRT_PROT_X) p |= PROT_EXEC;
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  const uintptr_t b = (uintptr_t)base & ~(uintptr_t)(page - 1);
  const size_t len =
      ((uintptr_t)base + bytes - b + page - 1) & ~(uintptr_t)(page - 1);
  return mprotect((void*)b, len, p);
}
void mmap_free(void* base, size_t) {
  for (int i = 0; i < g_extent_count; ++i) {
    if (g_extents[i].base == base) {
      munmap(base, g_extents[i].len);
      g_extents[i].base = nullptr;
      return;
    }
  }
}

void make_u16(const char* s, u16* out, size_t cap) {
  size_t i = 0;
  for (; s[i] && i + 1 < cap; ++i) out[i] = (u16)s[i];
  out[i] = 0;
}

void boot_runtime(bool capture_log) {
  const trt_backend be{mmap_alloc, mmap_protect, mmap_free,
                       capture_log ? log_capture : log_quiet};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
}

// Win64 CONTEXT pieces the suite needs (offsets per the bridge's own
// k_ctx_size = 0x4D0 layout; RIP 0xF8, RSP 0x98, ContextFlags 0x30, MxCsr
// 0x34/0x118 — same offsets TRT_CAPTURE_WIN64_REGS_ASM writes).
constexpr size_t k_ctx_size = 0x4D0;
constexpr size_t k_ctx_flags = 0x30;
constexpr size_t k_ctx_rip = 0xF8;
constexpr size_t k_ctx_rsp = 0x98;

// Win64 EXCEPTION_RECORD / EXCEPTION_POINTERS mirrors (bridge_kernel32.cpp
// static_asserts pin the real ones: 152 bytes / 16 bytes).
struct ex_record {
  u32 code;
  u32 flags;
  u64 record;
  u64 address;
  u32 num_params;
  u32 pad;
  u64 info[15];
};
struct ex_pointers {
  u64 exception_record;
  u64 context_record;
};

// the filter the suite registers (host code, Win64 ABI — trt_call_guest
// marshals rcx/rdx/r8/r9 exactly as it does for guest handlers)
volatile u32 g_filter_saw_code = 0;
volatile u64 g_filter_saw_addr = 0;
int g_filter_calls = 0;
i32 TRT_STDCALL test_filter(void* ep_v) {
  const ex_pointers* ep = (const ex_pointers*)ep_v;
  if (ep && ep->exception_record) {
    const ex_record* rec = (const ex_record*)(uintptr_t)ep->exception_record;
    g_filter_saw_code = rec->code;
    g_filter_saw_addr = rec->address;
  }
  ++g_filter_calls;
  return 1;  // EXCEPTION_EXECUTE_HANDLER
}

jmp_buf g_back;
volatile int g_trapped = 0;
volatile u32 g_trap_code = 0;
void raise_trap(u32 code) {
  g_trapped = 1;
  g_trap_code = code;
  longjmp(g_back, 1);
}

void build_synthetic_ctx(u8* ctx, u64 rip_marker) {
  memset(ctx, 0, k_ctx_size);
  const u32 flags = 0x10000Fu;
  const u32 mxcsr = 0x1F80u;
  memcpy(ctx + k_ctx_flags, &flags, 4);
  memcpy(ctx + 0x34, &mxcsr, 4);
  memcpy(ctx + 0x118, &mxcsr, 4);
  memcpy(ctx + k_ctx_rip, &rip_marker, 8);
  const u64 rsp = 0x7FFF0000ull;
  memcpy(ctx + k_ctx_rsp, &rsp, 8);
}

// ---- the two clamp probes run as FRESH PROCESSES (parse-once ceiling) ----

int probe_clamp_low() {
  // TRT_MAX_MAPS=2 (inherited env) must clamp UP to 8: 9 maps all succeed,
  // the 9th evicts, and the named line says ceiling 8.
  boot_runtime(true);
  u64 h = trt_CreateFileMappingW((u64)-1, nullptr, 0x04, 0, 0x10000, nullptr);
  if (h == 0) { printf("  FAIL probe-low: mapping\n"); return 1; }
  void* views[9] = {};
  int bad = 0;
  for (int i = 0; i < 9; ++i) {
    views[i] = trt_MapViewOfFile(h, 0x04 /*READ*/, 0, (u32)(i * 0x1000),
                                 0x1000);
    if (!views[i]) { printf("  FAIL probe-low: map %d returned NULL\n", i); ++bad; }
  }
  u32 maps = 0, live = 0;
  u64 bytes = 0;
  trt_mem_stats(&maps, &live, &bytes);
  if (live != 8) { printf("  FAIL probe-low: live=%u want 8\n", live); ++bad; }
  if (!log_contains("map ceiling 8 hit")) {
    printf("  FAIL probe-low: no 'map ceiling 8 hit' line\n");
    ++bad;
  }
  printf("  probe-clamp-low: %s\n", bad ? "FAIL" : "ok (TRT_MAX_MAPS=2 -> ceiling 8, 9th map evicts)");
  return bad ? 1 : 0;
}

int probe_clamp_high() {
  // TRT_MAX_MAPS=8192 must clamp DOWN to 4096: 4097 maps all succeed, the
  // 4097th evicts, and the named line says ceiling 4096.
  boot_runtime(true);
  u64 h = trt_CreateFileMappingW((u64)-1, nullptr, 0x04, 0, 0x1000, nullptr);
  if (h == 0) { printf("  FAIL probe-high: mapping\n"); return 1; }
  int bad = 0;
  void* last = nullptr;
  for (int i = 0; i < 4097; ++i) {
    last = trt_MapViewOfFile(h, 0x04, 0, 0, 0x1000);
    if (!last) { printf("  FAIL probe-high: map %d returned NULL\n", i); ++bad; break; }
  }
  u32 maps = 0, live = 0;
  u64 bytes = 0;
  trt_mem_stats(&maps, &live, &bytes);
  // with ceiling C, after N >= C successful maps the registry holds exactly C
  // (each map past the ceiling evicts one, then inserts one)
  if (live != 4096) { printf("  FAIL probe-high: live=%u want 4096\n", live); ++bad; }
  if (!log_contains("map ceiling 4096 hit")) {
    printf("  FAIL probe-high: no 'map ceiling 4096 hit' line\n");
    ++bad;
  }
  if (last) trt_UnmapViewOfFile(last);
  printf("  probe-clamp-high: %s\n", bad ? "FAIL" : "ok (TRT_MAX_MAPS=8192 -> ceiling 4096, 4097th map evicts)");
  return bad ? 1 : 0;
}

int run_probe(int argc, char** argv) {
  if (argc >= 2 && strcmp(argv[1], "--probe-clamp-low") == 0) {
    return probe_clamp_low();
  }
  if (argc >= 2 && strcmp(argv[1], "--probe-clamp-high") == 0) {
    return probe_clamp_high();
  }
  return -1;  // not a probe
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  const int probe_rc = run_probe(argc, argv);
  if (probe_rc >= 0) return probe_rc;

  printf("tiramisu wave5b mem-ceiling + crash-filter suite\n");

  // The ceiling is parsed once at the first map call: fix it before boot.
  setenv("TRT_MAX_MAPS", "16", 1);

  char tmpdir[] = "/tmp/tiramisu_w5b_XXXXXX";
  if (!mkdtemp(tmpdir)) { printf("cannot mkdtemp\n"); return 2; }
  boot_runtime(true);
  trt_vfs_mount("c:", tmpdir);

  u32 maps = 0, live = 0;
  u64 view_bytes = 0;

  // =========================================================================
  // Block A — ceiling 16 + LRU eviction + touch-refresh + registry hygiene
  // =========================================================================
  printf("block A: map ceiling + LRU eviction\n");
  {
    u64 h = trt_CreateFileMappingW((u64)-1, nullptr, 0x04, 0, 0x10000, nullptr);
    CHECK(h != 0, "anon mapping core created");

    // 16 CLEAN views (FILE_MAP_READ -> flush_on_unmap == false)
    void* v[16] = {};
    bool all_mapped = true;
    for (int i = 0; i < 16; ++i) {
      v[i] = trt_MapViewOfFile(h, 0x04 /*READ*/, 0, (u32)(i * 0x1000), 0x1000);
      if (!v[i]) all_mapped = false;
    }
    CHECK(all_mapped, "16 views map under the ceiling (no eviction yet)");
    trt_mem_stats(&maps, &live, &view_bytes);
    CHECK(live == 16 && view_bytes == 16ull * 0x1000,
          "registry holds exactly 16 live views / 64 KiB");

    // 17th map: ceiling hit -> LRU (v[0], never touched since map) evicted.
    // The new view is 0x8000 bytes so its allocation CANNOT fit into v[0]'s
    // freed 0x1000 hole — the evicted-range record survives for the checks
    // below (with a same-size map the allocator would reuse the address and
    // legitimately retire the record).
    char pat[128];
    void* v17 = trt_MapViewOfFile(h, 0x04, 0, 0, 0x8000);
    CHECK(v17 != nullptr, "17th map SUCCEEDS via eviction (pressure valve)");
    snprintf(pat, sizeof(pat), "evicting LRU view ptr=%p ", v[0]);
    CHECK(log_contains(pat), "named line: ceiling 16 hit, evicted LRU view v[0]");
    CHECK(log_contains("map ceiling 16 hit"), "named line carries the ceiling value");
    trt_mem_stats(&maps, &live, &view_bytes);
    CHECK(live == 16, "registry stays at the ceiling after eviction");

    // touching an EVICTED view fails visibly, named exactly once — checked
    // BEFORE any same-size allocation can recycle the address
    CHECK(trt_UnmapViewOfFile(v[0]) == 0, "unmap of an evicted view returns 0");
    CHECK(log_contains("use-after-evict detected"),
          "named line: use-after-evict detected");
    trt_UnmapViewOfFile(v[0]);
    CHECK(log_count("use-after-evict detected") == 1,
          "use-after-evict logged ONCE per evicted range");

    // Touch v[1] (a flush is a registry touch) -> v[2] becomes the LRU
    CHECK(trt_FlushViewOfFile(v[1], 0) == 1, "flush touches a live view");
    void* v18 = trt_MapViewOfFile(h, 0x04, 0, 0, 0x1000);
    CHECK(v18 != nullptr, "18th map succeeds");
    snprintf(pat, sizeof(pat), "evicting LRU view ptr=%p ", v[2]);
    CHECK(log_contains(pat),
          "LRU order honors the touch: v[2] evicted, refreshed v[1] kept");

    // survivors are still live and functional
    CHECK(trt_FlushViewOfFile(v[3], 0) == 1, "survivor view still registered");
    CHECK(trt_UnmapViewOfFile(v[3]) == 1, "survivor unmaps cleanly");

    // unmap everything that is live -> registry empty
    int unmapped = 0;
    for (int i = 0; i < 16; ++i) {
      if (i == 0 || i == 2 || i == 3) continue;  // evicted or already unmapped
      unmapped += (trt_UnmapViewOfFile(v[i]) == 1) ? 1 : 0;
    }
    unmapped += (trt_UnmapViewOfFile(v17) == 1) ? 1 : 0;
    unmapped += (trt_UnmapViewOfFile(v18) == 1) ? 1 : 0;
    CHECK(unmapped == 15, "all 15 remaining live views unmap");
    trt_mem_stats(&maps, &live, &view_bytes);
    CHECK(live == 0 && view_bytes == 0, "registry EMPTY after unmap-all");

    trt_CloseHandle((void*)(uintptr_t)h);
  }

  // =========================================================================
  // Block B — dirty views: flush-then-evict PRESERVES data; clean preferred
  // =========================================================================
  printf("block B: dirty views flush before eviction (no data loss)\n");
  {
    u64 h = trt_CreateFileMappingW((u64)-1, nullptr, 0x04, 0, 0x10000, nullptr);
    CHECK(h != 0, "anon mapping core (dirty block)");

    // 16 DIRTY views (FILE_MAP_WRITE -> flush_on_unmap == true), each with
    // its own unflushed marker
    void* d[16] = {};
    bool all_mapped = true;
    for (int i = 0; i < 16; ++i) {
      d[i] = trt_MapViewOfFile(h, 0x02 /*WRITE*/, 0, (u32)(i * 0x1000), 0x1000);
      if (!d[i]) { all_mapped = false; continue; }
      char mark[32];
      snprintf(mark, sizeof(mark), "DIRTY_EVICT_%d", i);
      strcpy((char*)d[i], mark);
    }
    CHECK(all_mapped, "16 dirty WRITE views mapped, markers written, NOT flushed");

    // 17th map: NO clean view exists -> the LRU dirty view (d[0]) is flushed
    // (data persists into the mapping backing) and only then evicted
    char pat[128];
    void* extra = trt_MapViewOfFile(h, 0x04, 0, 0, 0x1000);
    CHECK(extra != nullptr, "17th map succeeds via flush-then-evict");
    snprintf(pat, sizeof(pat), "evicting LRU dirty view ptr=%p ", d[0]);
    CHECK(log_contains(pat), "named line: LRU DIRTY view evicted");
    CHECK(log_contains("flushed first"), "named line says flushed first");

    // the evicted view's writes must have landed in the mapping backing
    void* back = trt_MapViewOfFile(h, 0x02 /*WRITE*/, 0, 0, 0x1000);
    CHECK(back != nullptr, "remap of the evicted offset succeeds");
    CHECK(memcmp(back, "DIRTY_EVICT_0", 13) == 0,
          "flush-then-evict PRESERVED the dirty bytes (remap reads them)");

    // the 18th map (back) finds a CLEAN candidate (extra) and must prefer
    // evicting it over flushing an older dirty view: extra (newest) goes
    // while older dirty views survive — that IS clean-preference
    void* back2 = trt_MapViewOfFile(h, 0x04, 0, 0x1000, 0x1000);
    CHECK(back2 != nullptr, "19th map succeeds");
    snprintf(pat, sizeof(pat), "evicting LRU view ptr=%p ", extra);
    CHECK(log_contains(pat),
          "clean-preferred: the LRU CLEAN view (extra) evicted, dirty kept");

    // the 19th map has NO clean candidate left (extra was consumed by the
    // 18th map's clean-preference) -> it flush-then-evicts the next LRU
    // dirty view (d[1]), preserving its bytes like d[0] before it
    snprintf(pat, sizeof(pat), "evicting LRU dirty view ptr=%p ", d[1]);
    CHECK(log_contains(pat),
          "19th map flush-then-evicts the next LRU dirty view (d[1])");

    // d[0] (17th), extra (18th, clean-preferred) and d[1] (19th) were
    // evicted; d[2..15] + back + back2 are live (16). The backend may have
    // REUSED evicted addresses for later maps, so dead pointers can alias
    // live views: count SUCCESSES over every pointer we hold — each live
    // view unmaps exactly once (first pointer to hit its address), every
    // other pointer is dead, so the count is 16 under ANY reuse pattern.
    int unmapped = 0;
    for (int i = 0; i < 16; ++i) {
      unmapped += (trt_UnmapViewOfFile(d[i]) == 1) ? 1 : 0;
    }
    unmapped += (trt_UnmapViewOfFile(extra) == 1) ? 1 : 0;
    unmapped += (trt_UnmapViewOfFile(back) == 1) ? 1 : 0;
    unmapped += (trt_UnmapViewOfFile(back2) == 1) ? 1 : 0;
    CHECK(unmapped == 16,
          "exactly the 16 live views unmap (evicted pointers are dead)");
    trt_mem_stats(&maps, &live, &view_bytes);
    CHECK(live == 0, "registry empty after dirty block");
    trt_CloseHandle((void*)(uintptr_t)h);
  }

  // =========================================================================
  // Block C — unevictable views: the map FAILS HONESTLY (never silent)
  // =========================================================================
  printf("block C: unevictable (flush-failing) views fail the map visibly\n");
  {
    // Mechanism probe: a GENERIC_READ-only VFS handle opens its OS stream
    // "rb" (bridge_kernel32.cpp:1505), so pwrite through it fails -> a dirty
    // view on that mapping cannot be flushed -> cannot be evicted.
    char probe_host[256];
    snprintf(probe_host, sizeof(probe_host), "%s/probe_ro.bin", tmpdir);
    FILE* f = fopen(probe_host, "wb");
    fwrite("PROBE-RO-FILE", 1, 13, f);
    fclose(f);
    u16 wpath[128];
    char narrow[128];
    snprintf(narrow, sizeof(narrow), "C:\\probe_ro.bin");
    make_u16(narrow, wpath, 128);
    void* fh = trt_CreateFileW(wpath, 0x80000000u /*GENERIC_READ only*/, 0,
                               nullptr, 3 /*OPEN_EXISTING*/, 0, nullptr);
    CHECK(fh != (void*)(uintptr_t)-1 && fh != nullptr,
          "read-only VFS handle opens");
    u64 h4 = trt_CreateFileMappingW((u64)(uintptr_t)fh, nullptr, 0x04, 0,
                                    0x2000, nullptr);
    CHECK(h4 != 0, "file mapping over the read-only handle");
    void* pv = trt_MapViewOfFile(h4, 0x02 /*WRITE -> dirty*/, 0, 0, 0x1000);
    CHECK(pv != nullptr, "dirty view over read-only file maps");
    strcpy((char*)pv, "WILL NOT PERSIST");
    CHECK(trt_FlushViewOfFile(pv, 16) == 0,
          "MEASURED: flush through a read-only handle FAILS (unevictable)");
    CHECK(trt_UnmapViewOfFile(pv) == 1, "probe view unmaps (flush loss named)");
    trt_CloseHandle((void*)(uintptr_t)h4);
    trt_CloseHandle(fh);

    // 16 dirty views whose flush always fails -> nothing is evictable
    char ro_host[256];
    snprintf(ro_host, sizeof(ro_host), "%s/ro_pak.bin", tmpdir);
    f = fopen(ro_host, "wb");
    for (int i = 0; i < 16 * 0x1000; ++i) fputc('R', f);
    fclose(f);
    snprintf(narrow, sizeof(narrow), "C:\\ro_pak.bin");
    make_u16(narrow, wpath, 128);
    void* fh2 = trt_CreateFileW(wpath, 0x80000000u, 0, nullptr, 3, 0, nullptr);
    CHECK(fh2 != (void*)(uintptr_t)-1 && fh2 != nullptr,
          "64 KiB read-only archive handle opens");
    u64 h5 = trt_CreateFileMappingW((u64)(uintptr_t)fh2, nullptr, 0x04, 0,
                                    0x10000, nullptr);
    CHECK(h5 != 0, "mapping over the read-only archive");
    void* r[16] = {};
    bool all_mapped = true;
    for (int i = 0; i < 16; ++i) {
      r[i] = trt_MapViewOfFile(h5, 0x02 /*WRITE -> dirty*/, 0,
                               (u32)(i * 0x1000), 0x1000);
      if (!r[i]) { all_mapped = false; continue; }
      ((volatile char*)r[i])[0] = 'W';  // make it dirty in fact, not just kind
    }
    CHECK(all_mapped, "16 unevictable dirty views fill the ceiling");

    void* dead = trt_MapViewOfFile(h5, 0x04, 0, 0, 0x1000);
    CHECK(dead == nullptr, "17th map FAILS HONESTLY (NULL, no evictable view)");
    CHECK(trt_GetLastError() == 8 /*ERROR_NOT_ENOUGH_MEMORY*/,
          "last error is ERROR_NOT_ENOUGH_MEMORY (the file's mapping error)");
    CHECK(log_contains("no evictable view"),
          "named line: no evictable view -> map fails");
    CHECK(log_contains("unevictable, skipping"),
          "each flush failure is named before the honest failure");
    trt_mem_stats(&maps, &live, &view_bytes);
    CHECK(live == 16, "registry unchanged by the failed map");

    int unmapped = 0;
    for (int i = 0; i < 16; ++i) unmapped += (trt_UnmapViewOfFile(r[i]) == 1);
    CHECK(unmapped == 16, "unevictable views still unmap (flush result named)");
    trt_mem_stats(&maps, &live, &view_bytes);
    CHECK(live == 0, "registry empty after unevictable block");
    trt_CloseHandle((void*)(uintptr_t)h5);
    trt_CloseHandle(fh2);
  }

  // =========================================================================
  // Block D — use-after-evict through the stale pointer faults VISIBLY
  // =========================================================================
  printf("block D: use-after-evict raw touch faults (fork-isolated)\n");
  {
    u64 h = trt_CreateFileMappingW((u64)-1, nullptr, 0x04, 0, 0x10000, nullptr);
    void* victim = trt_MapViewOfFile(h, 0x04, 0, 0, 0x1000);
    CHECK(victim != nullptr, "victim view mapped");
    // 16 fillers stay LIVE: victim + 15 fillers = 16, the 16th filler is the
    // 17th map -> ceiling hit -> the LRU view (victim, oldest stamp) evicts.
    void* filler[16] = {};
    int filled = 0;
    for (int i = 0; i < 16; ++i) {
      filler[i] = trt_MapViewOfFile(h, 0x04, 0, (u32)(((i + 1) % 16) * 0x1000),
                                    0x1000);
      if (filler[i]) ++filled;
    }
    CHECK(filled == 16, "16 fillers mapped live");
    char pat[128];
    snprintf(pat, sizeof(pat), "evicting LRU view ptr=%p ", victim);
    CHECK(log_contains(pat), "victim evicted by ceiling pressure");
    for (int i = 0; i < 16; ++i) trt_UnmapViewOfFile(filler[i]);
    trt_mem_stats(&maps, &live, &view_bytes);
    CHECK(live == 0, "fillers unmapped, victim evicted");

    const pid_t pid = fork();
    if (pid == 0) {
      // child: touch the dead view. Backend-backed views are munmapped on
      // eviction, so this must SIGSEGV — fail visibly, not silently.
      volatile u8 sink = ((volatile u8*)victim)[0];
      (void)sink;
      _exit(0);  // reached only if the touch did NOT fault
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV,
          "raw touch of an evicted view SIGSEGVs (pages returned to runtime)");
    trt_CloseHandle((void*)(uintptr_t)h);
  }

  // =========================================================================
  // Block E — item 46: SetUnhandledExceptionFilter + fatal-path wiring
  // =========================================================================
  printf("block E: unhandled-exception filter + fatal path\n");
  {
    trt_boot_raise_trap = &raise_trap;

    // no filter registered: the fatal path still terminates, named no-filter
    u8 ctx[k_ctx_size];
    build_synthetic_ctx(ctx, 0x0000DEAD0000BEEFull);
    g_trapped = 0;
    if (setjmp(g_back) == 0) {
      trt_raise_exception_sysv(0xE0004607u, 0, 0, nullptr, ctx);
    }
    CHECK(g_trapped == 1 && g_trap_code == 0xE0004607u,
          "no-filter fatal path still reaches termination (trap = _exit stand-in)");
    CHECK(log_contains("-> filter no-filter"),
          "named line: unhandled exception -> filter no-filter");

    // register the filter: stored, previous returned, named line
    void* prev = trt_SetUnhandledExceptionFilter((void*)&test_filter);
    CHECK(prev == nullptr, "first registration reports no previous filter");
    CHECK(log_contains("SetUnhandledExceptionFilter(filter="),
          "named line: filter registration logged");

    // the fatal path invokes the filter with the REAL record before dying
    g_trapped = 0;
    g_filter_saw_code = 0;
    g_filter_saw_addr = 0;
    if (setjmp(g_back) == 0) {
      trt_raise_exception_sysv(0xE0004606u, 0, 0, nullptr, ctx);
    }
    CHECK(g_trapped == 1 && g_trap_code == 0xE0004606u,
          "fatal path still terminates after the filter had its chance");
    CHECK(g_filter_calls == 1, "filter invoked exactly once by the fatal path");
    CHECK(g_filter_saw_code == 0xE0004606u,
          "filter saw the correct exception CODE");
    CHECK(g_filter_saw_addr == 0x0000DEAD0000BEEFull,
          "filter saw the correct exception ADDRESS (from the CONTEXT)");
    CHECK(log_contains("[trt] eh: unhandled exception 0xe0004606"),
          "named line: fatal path names the exception");
    CHECK(log_contains("-> filter handled"),
          "named line: verdict handled (EXCEPTION_EXECUTE_HANDLER)");

    // swap/clear: previous filter is returned, nullptr clears
    void* prev2 = trt_SetUnhandledExceptionFilter(nullptr);
    CHECK(prev2 == (void*)&test_filter, "clearing returns the previous filter");

    // UnhandledExceptionFilter export: consults the stored filter
    void* nof = trt_UnhandledExceptionFilter(nullptr);
    CHECK((uintptr_t)nof == 0,
          "UnhandledExceptionFilter with no filter -> EXCEPTION_CONTINUE_SEARCH");
    trt_SetUnhandledExceptionFilter((void*)&test_filter);
    ex_record rec{};
    rec.code = 0xE0001234u;
    rec.address = 0x0000FEED0000FACEull;
    ex_pointers ep{(u64)(uintptr_t)&rec, (u64)(uintptr_t)ctx};
    g_filter_saw_code = 0;
    g_filter_saw_addr = 0;
    void* verd = trt_UnhandledExceptionFilter(&ep);
    CHECK((uintptr_t)verd == 1, "export returns the filter's verdict (1)");
    CHECK(g_filter_saw_code == 0xE0001234u && g_filter_saw_addr == 0x0000FEED0000FACEull,
          "export passes the guest's EXCEPTION_POINTERS through verbatim");
    CHECK(log_contains("UnhandledExceptionFilter -> filter handled"),
          "named line: export path logs the verdict");
    trt_SetUnhandledExceptionFilter(nullptr);

    trt_boot_raise_trap = nullptr;  // restore: NULL on every host build
  }

  // =========================================================================
  // Block F — TRT_MAX_MAPS clamps (fresh processes: parse-once ceiling)
  // =========================================================================
  printf("block F: TRT_MAX_MAPS honored + clamped (re-exec probes)\n");
  {
    char self[512];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    CHECK(n > 0, "readlink /proc/self/exe for the clamp probes");
    if (n > 0) {
      self[n] = 0;
      struct probe_spec { const char* env; const char* arg; int want_rc; };
      const probe_spec probes[] = {
          {"2", "--probe-clamp-low", 0},
          {"8192", "--probe-clamp-high", 0},
      };
      for (const probe_spec& p : probes) {
        const pid_t pid = fork();
        if (pid == 0) {
          setenv("TRT_MAX_MAPS", p.env, 1);
          execl(self, self, p.arg, (char*)nullptr);
          _exit(127);  // exec failed
        }
        int st = 0;
        waitpid(pid, &st, 0);
        char msg[96];
        snprintf(msg, sizeof(msg), "probe TRT_MAX_MAPS=%s %s", p.env,
                 WIFEXITED(st) && WEXITSTATUS(st) == p.want_rc ? "passed"
                                                               : "FAILED");
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == p.want_rc, msg);
      }
    }
  }

  printf("failures=%d\n", failures);
  if (failures == 0) {
    printf("RESULT: ALL PASS\n");
    return 0;
  }
  printf("RESULT: FAILURES\n");
  return 1;
}
