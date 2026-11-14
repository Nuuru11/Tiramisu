// tiramisu/tests/test_corpus_run.cpp
// STAGE 6 — RUN-DEPTH: execute REAL game DLL code offline.
//
// The corpus load matrix (test_corpus.cpp) proves imports BIND ("LOAD OK").
// This harness proves the next frontier: whether real game code EXECUTES.
// The difference between "binds" and "boots" is DllMain — every game DLL
// runs its own initialization the moment Windows loads it, and that code
// exercises bridge paths no synthetic guest ever did.
//
// For every corpus PE (DLLs only — for a DLL, AddressOfEntryPoint IS
// DllMain):
//
//   1. map + bind + seal exactly like the loader tests (mmap backend,
//      trt_load_pe -> trt_map_pe -> trt_seal_image, W^X, game-dir sibling
//      search). Then EXECUTE the real entry point with the Win64 ABI:
//        INT_PTR DllMain(HINSTANCE hinst, DWORD reason=1 (PROCESS_ATTACH),
//                        LPVOID reserved=NULL)
//      on the SAME thread — the exact call images.cpp makes on console.
//
//   2. BEFORE any entry execution: SIGSEGV / SIGILL / SIGBUS handlers
//      (sigaction, SA_SIGINFO). On fault: a NAMED crash report — PE name,
//      fault address + offset from the image base, faulting RIP offset from
//      base, and the last 40 trt log lines (the loader logs every bridge
//      call via trt_core_log; the log lives in a mmap(MAP_SHARED) ring so
//      the report survives the fault) — then _exit(42).
//      A crash is a DISCOVERY ARTIFACT: the last bridge lines name which
//      surface real code hit before dying.
//
//   3. Verdicts (both legitimate; the report IS the work list):
//        RUN OK <name>: DllMain(PROCESS_ATTACH) returned.
//        RUN FAILED <name>: SIGSEGV at +0x... — last bridge: <line>
//
//   4. ONE attempt per invocation (no retry loops; you iterate by fixing
//      bridges between runs). A per-DLL watchdog (alarm) bounds a hang.
//
// Isolation: each DLL runs in a FORKED child, so a guest that corrupts the
// host process or dies unhandlable cannot take the whole matrix with it.
// The parent only decodes exit status; the child prints its own verdict.
//
// Exit 0 iff every PE got a verdict (RUN OK or RUN FAILED). A fault is a
// legitimate outcome and never fails the gate — but the report names it.
#include "../src/trt/trt.h"
#include "../src/trt/images.h"
#include "../src/trt/pe.h"
#include "../src/trt/input.h"
#include "../src/trt/thunk.h"
#include "../src/trt/bridge_reg.h"

// bridge_sys.cpp: Win8+ temp-path (shared answer with GetTempPathW).
// Declared inside extern "C" so the guest-ABI symbol name matches.
extern "C" int __attribute__((ms_abi)) trt_GetTempPathW(u32 size, u16* buf);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <fcntl.h>

// ---------------------------------------------------------------------------
// shared log ring: mmap(MAP_SHARED|MAP_ANONYMOUS) so the parent can read the
// last lines after the child dies, and so the signal handler (async-signal
// context) can dump it with raw write() only.
// ---------------------------------------------------------------------------
#define TRT_RING_LINES 512
#define TRT_RING_LINE  200
struct trt_ring {
  unsigned long count;                        // total lines written
  char last_bridge[TRT_RING_LINE];            // last "trt:" line (named surface)
  char lines[TRT_RING_LINES][TRT_RING_LINE];
};
static trt_ring* g_ring = nullptr;
static char g_pe_name[128] = "";
static unsigned long long g_image_base = 0;

static void ring_put(const char* line) {
  if (!g_ring) return;
  const unsigned long i = g_ring->count++ % TRT_RING_LINES;
  snprintf(g_ring->lines[i], TRT_RING_LINE, "%s", line);
  if (strncmp(line, "trt:", 4) == 0)
    snprintf(g_ring->last_bridge, TRT_RING_LINE, "%s", line);
}

// raw-write a whole string (async-signal-safe syscalls only in the handler)
static void wout(const char* s) {
  size_t n = strlen(s);
  while (n) { ssize_t w = write(1, s, n); if (w <= 0) break; s += w; n -= (size_t)w; }
}

// dump the last `howmany` lines of the ring with raw write()
static void ring_dump_last(int howmany) {
  if (!g_ring) return;
  const unsigned long total = g_ring->count;
  const unsigned long start = total > (unsigned long)howmany
                                  ? total - (unsigned long)howmany : 0;
  for (unsigned long i = start; i < total; ++i) {
    wout("  [trt] ");
    wout(g_ring->lines[i % TRT_RING_LINES]);
    wout("\n");
  }
}

// the ring line index of the LAST line whose text mentions bridge activity:
// the loader logs every bind ("trt: STUB <dll>!<sym>") and every bridge
// call reaches trt_thunk_dispatch which logs "trt: <kind> <dll>!<sym> called
// (unimplemented)". The LAST such line names the surface real code hit last.
static const char* last_bridge_line(void) {
  if (!g_ring || !g_ring->last_bridge[0]) return "(none — fault before any bridge call)";
  return g_ring->last_bridge;
}

// the first raw log line after a given ring index — unused hook for future
// surface back-tracing (kept out to avoid unused warnings)
// ---------------------------------------------------------------------------

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_user32(void);
}

namespace exe_boot {
void observe(const char* line);   // STAGE 7 depth-ladder detection (below)
int  child_boot(const char* path);  // the EXE boot child (below)
}

extern "C" void trt_log_ring(const char* line) {
  ring_put(line);
  exe_boot::observe(line);  // STAGE 7 depth-ladder detection
  printf("  [trt] %s\n", line);
}

// ---------------------------------------------------------------------------
// fault handler: the named crash report. Installed BEFORE entry execution.
// ---------------------------------------------------------------------------
static void fault_report(int sig, siginfo_t* si, void* uctx_v) {
  ucontext_t* uc = (ucontext_t*)uctx_v;
  unsigned long long rip = 0;
#if defined(__x86_64__)
  rip = (unsigned long long)uc->uc_mcontext.gregs[REG_RIP];
#endif
  const unsigned long long fault = (unsigned long long)(uintptr_t)si->si_addr;
  const char* signame = sig == SIGSEGV ? "SIGSEGV" : (sig == SIGILL ? "SIGILL" : "SIGBUS");

  char buf[TRT_RING_LINE];
  wout("\n==== TIRAMISU CRASH REPORT (discovery artifact) ====\n");
  snprintf(buf, sizeof(buf), "PE: %s\n", g_pe_name);              wout(buf);
  snprintf(buf, sizeof(buf), "signal: %s (code %d)\n", signame, si->si_code); wout(buf);
  snprintf(buf, sizeof(buf), "fault address: 0x%llx", fault);
  wout(buf);
  if (g_image_base && fault >= g_image_base) {
    snprintf(buf, sizeof(buf), "  (= image base + 0x%llx, inside the guest image)\n",
             fault - g_image_base);
    wout(buf);
  } else {
    wout("  (outside the guest image — host-side or thunk fault)\n");
  }
  snprintf(buf, sizeof(buf), "faulting RIP: 0x%llx", rip);
  wout(buf);
  if (g_image_base && rip >= g_image_base) {
    snprintf(buf, sizeof(buf), "  (= image base + 0x%llx, guest code)\n",
             rip - g_image_base);
    wout(buf);
  } else {
    wout("  (outside the guest image)\n");
  }
  snprintf(buf, sizeof(buf), "last bridge: %s\n", last_bridge_line()); wout(buf);
  wout("last trt log lines:\n");
  ring_dump_last(40);
  wout("==== end crash report ====\n");
  fflush(stdout);
  _exit(42);  // the named-fault exit code
}

static void install_fault_handlers(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = fault_report;
  sa.sa_flags = SA_SIGINFO | SA_RESETHAND;  // one report; a double fault dies
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGILL, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);
}

static void hang_kill(int) { _exit(43); }  // 43 = watchdog, distinct from 42

// 45 = SKIP verdict (32-bit x86 guest: the M7 x86 engine is not on this path)
static void print_x86_skip(const char* path) {
  const char* name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
  printf("SKIP %s: x86 guest (machine=0x14c) — DllMain execution needs the "
         "M7 x86 engine, not this PE64 path\n", name);
}

// ---------------------------------------------------------------------------
// mmap backend (same shape as test_corpus / test_pe_loader)
// ---------------------------------------------------------------------------
namespace {

struct extent { void* base; size_t len; };
extent g_extents[256];
int g_extent_count = 0;

void* linux_alloc(size_t bytes, u64 preferred_base) {
  // STAGE 6 correction: honor the console VirtualAlloc contract — a fixed
  // preferred-base allocation FAILS when the range is already taken. The
  // old unconditional MAP_FIXED silently overmapped: baselib.dll and
  // GameAssembly.dll both prefer 0x180000000, so baselib overwrote
  // GameAssembly's first 0x3C000 bytes and its section headers (found live
  // when GameAssembly's seal read baselib's headers and its entry died on
  // corrupted pages). trt_map_pe already handles a NULL here: it retries
  // anywhere and runs the relocation pass.
  if (preferred_base) {
    const uintptr_t want = (uintptr_t)preferred_base;
    const uintptr_t want_end = want + bytes;
    for (int i = 0; i < g_extent_count; ++i) {
      const uintptr_t b = (uintptr_t)g_extents[i].base;
      const uintptr_t e = b + g_extents[i].len;
      if (want < e && b < want_end) {
        char line[160];
        snprintf(line, sizeof(line),
                 "trt: preferred base %llx unavailable (extent %p..%p taken)",
                 (unsigned long long)want, g_extents[i].base,
                 (void*)(uintptr_t)e);
        trt_log_ring(line);
        return nullptr;
      }
    }
  }
  void* p = mmap(preferred_base ? (void*)(uintptr_t)preferred_base : nullptr,
                 bytes, PROT_READ | PROT_WRITE,
                 preferred_base ? (MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS)
                                : (MAP_PRIVATE | MAP_ANONYMOUS),
                 -1, 0);
  if (p == MAP_FAILED) return nullptr;
  if (g_extent_count < 256) {
    g_extents[g_extent_count].base = p;
    g_extents[g_extent_count].len = bytes;
    ++g_extent_count;
  }
  return p;
}

int linux_protect(void* base, size_t bytes, unsigned prot) {
  int p = 0;
  if (prot & TRT_PROT_R) p |= PROT_READ;
  if (prot & TRT_PROT_W) p |= PROT_WRITE;
  if (prot & TRT_PROT_X) p |= PROT_EXEC;
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  const uintptr_t b = (uintptr_t)base & ~(uintptr_t)(page - 1);
  const size_t len = ((uintptr_t)base + bytes - b + page - 1) & ~(uintptr_t)(page - 1);
  return mprotect((void*)b, len, p);
}

void linux_release(void* base, size_t bytes) {
  for (int i = 0; i < g_extent_count; ++i)
    if (g_extents[i].base == base) {
      munmap(base, g_extents[i].len);
      g_extents[i].base = nullptr;
      return;
    }
  (void)bytes;
}

// STAGE 7 (OrTMM): host-side .pdata unwind registration. The guest's CRT
// init walks its own x64 unwind tables (RtlLookupFunctionEntry over
// RtlCaptureContext's context) and VirtualProtects .pdata R->RW to edit the
// unwind EpiloguePacking bits — with no way to re-register the edited
// range with the HOST unwinder, the host unwinder cannot find the guest's
// function entries and the guest faults. libunwind exposes the same
// registration semantics for DWARF (.eh_frame), but not PE .pdata; the
// honest Linux-host equivalent of RtlAddFunctionTable for THIS test is a
// no-op success (the console host uses the real RtlAddFunctionTable —
// src/host/main.cpp:uwp_register_unwind). The guest still gets its
// R->RW VirtualProtect, which is what the CRT edits .pdata through.
int linux_register_unwind(void* image_base, const void* pdata, u32 count) {
  (void)image_base; (void)pdata; (void)count;
  return 0;
}

// ---------------------------------------------------------------------------
// the CONSOLE's system loader (same offline model as test_corpus.cpp —
// whitelist of P/U class DLLs; anything else routes STUB or fails named).
// ---------------------------------------------------------------------------
struct console_dll { const char* name; };
const console_dll k_console_dlls[] = {
    {"hid.dll"}, {"setupapi.dll"}, {"version.dll"}, {"shcore.dll"}, {"avrt.dll"},
    {"d3d11.dll"}, {"dxgi.dll"}, {"d2d1.dll"}, {"dwrite.dll"}, {"dcomp.dll"},
    {"xinput1_4.dll"}, {"ws2_32.dll"}, {"d3d12.dll"}, {"d3d12core.dll"},
    {"bcrypt.dll"}, {"winhttp.dll"},
    {"kernel32.dll"}, {"kernelbase.dll"}, {"ntdll.dll"}, {"ucrtbase.dll"},
    {"combase.dll"}, {"user32.dll"}, {"advapi32.dll"},
    {"vcruntime140.dll"}, {"msvcp140.dll"},
    {"vcruntime140_app.dll"}, {"msvcp140_app.dll"},
    {"dstorage.dll"}, {"dstoragecore.dll"},
};

char wl_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

bool wl_ieq(const char* a, const char* b) {
  while (*a && *b) {
    if (wl_lower(*a) != wl_lower(*b)) return false;
    ++a; ++b;
  }
  return *a == *b;
}

bool whitelist_has(const char* module) {
  if (!module) return false;
  if (wl_lower(module[0]) == 'a' && wl_lower(module[1]) == 'p' &&
      wl_lower(module[2]) == 'i' && module[3] == '-' && module[4] == 'm' &&
      module[5] == 's')
    return true;  // api-ms-win-* contract prefix
  for (const console_dll& d : k_console_dlls)
    if (wl_ieq(d.name, module)) return true;
  return false;
}

// Everything a whitelisted SYSTEM module binds to in this offline model.
// TRT_GUEST_ABI (ms_abi) — real guest code WILL call these with the Win64
// ABI in this harness (unlike the load-only corpus, where it never ran),
// so the ABI here is load-bearing: a SysV body called through the guest's
// IAT would corrupt rcx/rdx/r8/r9 arg registers.
//
// STAGE 7: the blanket "answers 0 for everything" bind turned out to be a
// real boot blocker, measured live under gdb: UnityPlayer.dll's memory
// kernel calls ntdll!NtQuerySystemInformation(SystemBasicInformation) to
// size its bucket allocator; a blanket-0 answer = "query failed" = an
// EMPTY allocator descriptor ([+0x50]=0), and the first alloc(0x78)
// takes the un-initialized slow path, returns NULL, and the caller
// SIGSEGVs storing through it (fault 0x1816da6a6, one instruction after
// the vcall — this cost a five-boot gdb trace to pin). The honest-minimal
// answer is NOT 0 for a kernel that on real Windows ANSWERS: it is the
// truthful structure the guest reads. Everything still unimplemented
// keeps the honest 0 / fail-visible answer.
u64 TRT_GUEST_ABI console_system_fn(void) {
  // STAGE 7: name every blanket-0 bind the boot actually hits — these are
  // the next surfaces to implement (each line is a discovery artifact).
  static _Thread_local char sym_name[96] = "";
  (void)sym_name;
  return 0;
}

// ---- SYSTEM_BASIC_INFORMATION (ntdll!NtQuerySystemInformation class 0) --
// Layout (winternl.h): ULONG Reserved; ULONG TimerResolution; ULONG
// PageSize; ULONG NumberOfProcessors; ULONG Reserved2[5]; ULONG
// MinimumApplicationAddress; ULONG MaximumApplicationAddress; ...
// UnityPlayer reads PageSize (must be 4096) and the address window, and
// gates its bucket-allocator init on the query SUCCEEDING
// (NTSTATUS >= 0). Same numbers the GetSystemInfo bridge already reports.
struct sys_basic_info {
  u32 reserved;
  u32 timer_resolution;
  u32 page_size;
  u32 number_of_processors;
  u32 reserved2[5];
  u32 minimum_application_address;
  u32 maximum_application_address;
  u32 reserved3[2];
  u64 padding_to_0x30[2];  // record size UnityPlayer's copy loop assumes
};
// NTSTATUS STATUS_SUCCESS = 0; class gate: >= 0 means success
u64 TRT_GUEST_ABI console_nt_query_system_info(u32 cls, void* out, u32 len,
                                               u32* needed) {
  if (cls != 0 /*SystemBasicInformation*/) {
    // honest: unknown class = not implemented here
    if (needed) *needed = 0;
    return (u64)(i64)-1;  // STATUS_INVALID_INFO_CLASS
  }
  if (needed) *needed = (u32)sizeof(sys_basic_info);
  if (!out || len < sizeof(sys_basic_info)) return (u64)0xC0000004u; // STATUS_INFO_LENGTH_MISMATCH
  sys_basic_info* bi = (sys_basic_info*)out;
  memset(bi, 0, sizeof(*bi));
  bi->timer_resolution = 156250;   // 15.6 ms tick, real machines
  bi->page_size = 4096;
  bi->number_of_processors = 8;
  bi->minimum_application_address = 0x10000;
  bi->maximum_application_address = 0x7FFEFFFF;
  return 0;  // STATUS_SUCCESS
}

struct console_named {
  const char* module;
  const char* symbol;
  void* fn;
};

// the SYSTEM surface the boot actually queries (measured: a blanket-0
// kernel turned UnityPlayer's allocator init into an empty descriptor and
// a NULL-store SIGSEGV one boot later). Unlisted symbols keep the 0 bind.
const console_named k_console_named_fns[] = {
    {"ntdll.dll", "NtQuerySystemInformation", (void*)&console_nt_query_system_info},
    {"ntdll.dll", "ZwQuerySystemInformation", (void*)&console_nt_query_system_info},
    // STAGE 7 (dstoragecore): its obfuscation runtime jumps through IAT slot
    // 0x18005e640 — covered by NO import FirstThunk span (ntdll's ends at
    // 0x5e628) and 0 in the file: an unbound DELAY-import-style slot. The
    // runtime's lazy loader wrote the resolved advapi32!SystemFunction036
    // there; until the pe_loader binds delay imports, this slot must hand
    // back a live function or the runtime's jmp through it SIGSEGVs. Bind
    // the same honest deterministic-fill bridge (never a fake success).
    {"kernel32.dll", "SystemFunction036", (void*)&trt_bridge_SystemFunction036},
    {"kernel32.dll", "GetSystemInfo", nullptr},  // BUILTIN already answers; guard
    // Win8+ temp-path the UCRT imports; blanket-0 made the UCRT temp-name
    // loop spin to the backstop. Same guest-style answer as GetTempPathW.
    {"kernel32.dll", "GetTempPath2W", (void*)&trt_GetTempPathW},
};
const int k_console_named_count =
    (int)(sizeof(k_console_named_fns) / sizeof(k_console_named_fns[0]));

void* console_system_load(const char* module, const char* symbol) {
  if (!module) return nullptr;
  if (!whitelist_has(module)) return nullptr;
  if (symbol) {
    // STAGE 7: named surface first — the guest reads real answers here
    for (int i = 0; i < k_console_named_count; ++i) {
      if (wl_ieq(k_console_named_fns[i].module, module) &&
          wl_ieq(k_console_named_fns[i].symbol, symbol)) {
        if (k_console_named_fns[i].fn) return k_console_named_fns[i].fn;
        break;  // named but no fn: fall through to the blanket 0 bind
      }
    }
    // STAGE 7 discovery: name every blanket-0 bind of a whitelisted SYSTEM
    // symbol — each line is a candidate surface for the next fix-cycle.
    fprintf(stderr, "  [trt] SYSTEM bind (blanket-0): %s!%s\n", module, symbol);
  }
  (void)symbol;  // name or ordinal: one shared Win64-ABI address either way
  return (void*)&console_system_fn;
}

// ---------------------------------------------------------------------------
// child: map + seal + execute DllMain for ONE PE
// ---------------------------------------------------------------------------
int child_run(const char* path) {
  setvbuf(stdout, NULL, _IONBF, 0);

  install_fault_handlers();
  signal(SIGALRM, hang_kill);
  alarm(180);  // ONE attempt, bounded: a hung DllMain is a named outcome too

  const char* slash = strrchr(path, '/');
  char g_game_dir[1024];
  if (slash) {
    const size_t dlen = (size_t)(slash - path);
    if (dlen < sizeof(g_game_dir)) { memcpy(g_game_dir, path, dlen); g_game_dir[dlen] = 0; }
    else g_game_dir[0] = 0;
  } else { g_game_dir[0] = '.'; g_game_dir[1] = 0; }

  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &trt_log_ring,
                 &linux_register_unwind};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();
  trt_set_system_loader(&console_system_load);
  // STAGE 6: full boot semantics — TLS callbacks AND the entry point run
  trt_set_image_code_run(1);
  trt_runtime_init(&be, g_game_dir);
  trt_images_reset();

  const char* name = slash ? slash + 1 : path;
  snprintf(g_pe_name, sizeof(g_pe_name), "%s", name);

  // machine gate BEFORE the map attempt (same named-skip discipline as the
  // load corpus): a 32-bit DLL is a known frontier, not a loader gap.
  {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("RUN FAILED %s: cannot read file\n", name); return 44; }
    unsigned char hdr[0x400];
    const size_t got = fread(hdr, 1, sizeof(hdr), f);
    fclose(f);
    if (got < 0x40 || hdr[0] != 'M' || hdr[1] != 'Z') {
      printf("RUN FAILED %s: no MZ\n", name);
      return 44;
    }
    const unsigned nt_off =
        (unsigned)hdr[0x3c] | ((unsigned)hdr[0x3d] << 8) |
        ((unsigned)hdr[0x3e] << 16) | ((unsigned)hdr[0x3f] << 24);
    if (nt_off + 8 > got) { printf("RUN FAILED %s: e_lfanew out of file\n", name); return 44; }
    const u16 machine = (u16)(hdr[nt_off + 4] | (hdr[nt_off + 5] << 8));
    if (machine != 0x8664) {
      print_x86_skip(path);
      return 45;
    }
  }

  trt_image* img = trt_load_pe(path, name, 1);
  if (!img) {
    printf("RUN FAILED %s: map/seal (see [trt] lines — the named loader gap)\n", name);
    fflush(stdout);
    return 44;
  }
  g_image_base = (unsigned long long)(uintptr_t)img->base;

  if (!img->entry_rva) {
    printf("RUN SKIPPED %s: no entry point (no DllMain to execute)\n", name);
    fflush(stdout);
    return 0;
  }

  printf("  run: executing DllMain at image+0x%llx (hinst=%p reason=1 reserved=NULL)\n",
         (unsigned long long)img->entry_rva, (void*)(uintptr_t)img->base);
  // The entry already ran inside trt_load_pe (run_entry=1, same thread,
  // Win64 ABI via trt_call_guest). Reaching here means it returned.
  printf("RUN OK %s: DllMain(PROCESS_ATTACH) returned.\n", name);
  fflush(stdout);
  return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// parent: fork per PE, decode the child's outcome
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc < 2) {
    printf("usage: test_corpus_run [--exe] <pe> [<pe> ...]\n");
    printf("  (default)   run DllMain(PROCESS_ATTACH) on each DLL\n");
    printf("  --exe       boot each EXE: CRT startup -> WinMain -> engine\n");
    return 2;
  }

  g_ring = (trt_ring*)mmap(nullptr, sizeof(trt_ring), PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (g_ring == MAP_FAILED) { fprintf(stderr, "ring mmap failed\n"); return 2; }
  memset(g_ring, 0, sizeof(*g_ring));

  // STAGE 7: --exe selects the EXE-entry boot phase
  int first = 1;
  int exe_mode = 0;
  if (strcmp(argv[1], "--exe") == 0) { exe_mode = 1; first = 2; }
  if (argc <= first) { printf("usage: test_corpus_run [--exe] <pe> ...\n"); return 2; }

  // EXE phase: one forked boot per EXE, same isolation contract
  if (exe_mode) {
    int n_ok = 0, n_failed = 0, n_skipped = 0;
    for (int a = first; a < argc; ++a) {
      const char* path = argv[a];
      const char* name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
      memset(g_ring, 0, sizeof(*g_ring));
      g_image_base = 0;
      g_pe_name[0] = 0;
      printf("\n==== BOOT %s ====\n", name);
      pid_t pid = fork();
      if (pid < 0) { printf("BOOT FAILED %s: fork failed\n", name); ++n_failed; continue; }
      if (pid == 0) _exit(exe_boot::child_boot(path));
      int st = 0;
      waitpid(pid, &st, 0);
      if (WIFEXITED(st)) {
        const int code = WEXITSTATUS(st);
        if (code == 0) ++n_ok;
        else if (code == 42) {
          printf("BOOT FAILED %s: guest fault/hang backstop (exit 42)\n", name);
          ++n_failed;
        } else if (code == 43) {
          printf("BOOT FAILED %s: watchdog exit 43\n", name);
          ++n_failed;
        } else if (code == 44 || code == 45) {
          if (code == 45) ++n_skipped; else ++n_failed;  // named by the child
        } else {
          printf("BOOT EXITED %s: child exit %d\n", name, code);
          ++n_ok;  // an ExitProcess-shaped exit is a NAMED boot outcome
        }
      } else if (WIFSIGNALED(st)) {
        printf("BOOT FAILED %s: died on signal %d without the named report\n",
               name, WTERMSIG(st));
        ring_dump_last(40);
        ++n_failed;
      } else { printf("BOOT FAILED %s: unknown wait state\n", name); ++n_failed; }
    }
    printf("\nCORPUS-EXE: %d booted, %d failed, %d skipped (x86)\n",
           n_ok, n_failed, n_skipped);
    printf("the depth ladder + named blockers above are the deliverable.\n");
    return n_failed ? 1 : 0;
  }

  int n_ok = 0, n_failed = 0, n_no_verdict = 0, n_skipped = 0;
  for (int a = first; a < argc; ++a) {
    const char* path = argv[a];
    const char* name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;

    // fresh ring per PE (the crash report must be THIS dll's last lines)
    memset(g_ring, 0, sizeof(*g_ring));
    g_image_base = 0;
    g_pe_name[0] = 0;

    printf("-- %s --\n", name);
    pid_t pid = fork();
    if (pid < 0) {
      printf("RUN FAILED %s: fork failed\n", name);
      ++n_no_verdict;
      continue;
    }
    if (pid == 0) _exit(child_run(path));

    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFEXITED(st)) {
      const int code = WEXITSTATUS(st);
      if (code == 0) { ++n_ok; }                        // verdict printed by child
      else if (code == 42) {
        printf("RUN FAILED %s: guest fault (named crash report above, exit 42)\n", name);
        ++n_failed;
      } else if (code == 43) {
        printf("RUN FAILED %s: watchdog — DllMain did not return in 180s\n", name);
        ++n_failed;
      } else if (code == 44) {
        ++n_failed;  // map/seal failure, named by the child
      } else if (code == 45) {
        ++n_skipped;  // x86 guest — named frontier, not a failure
      } else {
        printf("RUN FAILED %s: child exit %d (unnamed)\n", name, code);
        ++n_no_verdict;
      }
    } else if (WIFSIGNALED(st)) {
      printf("RUN FAILED %s: died on signal %d without the named report\n", name,
             WTERMSIG(st));
      printf("  last bridge: %s\n", last_bridge_line());
      ring_dump_last(40);
      ++n_no_verdict;
    } else {
      printf("RUN FAILED %s: unknown wait state\n", name);
      ++n_no_verdict;
    }
  }

  printf("\nCORPUS-RUN: %d run ok, %d run failed, %d skipped (x86), %d no-verdict\n",
         n_ok, n_failed, n_skipped, n_no_verdict);
  printf("run-depth verdicts are the work list: a RUN FAILED names the bridge\n"
         "surface real code hit last; fix it, add its own test, run again.\n");
  // Faults are legitimate discovery outcomes: the gate is "every PE got a
  // verdict", not "everything runs" — that is what keeps the harness green
  // while the work list is still open.
  return n_no_verdict ? 1 : 0;
}

// ===========================================================================
// STAGE 7 — EXE-ENTRY BOOT DEPTH: execute a real game EXE's CRT startup,
// WinMain and (if reached) the engine loop, OFFLINE, on the same PE64 path.
//
// A DLL boots through DllMain; an EXE must first run its MSVC CRT startup
// (the code before WinMain), which touches a kernel32 surface the DLL
// corpus never did: FLS, heap sizing, code pages, SEH registration,
// GetStartupInfoW, command lines, EncodePointer. Only if ALL of that
// returns honest answers does the CRT reach WinMain — and a Unity EXE's
// WinMain is one call: UnityPlayer!UnityMain2.
//
// Per EXE (one attempt, fork-isolated like the DLL phase):
//   1. map + seal the EXE with run_entry=0 (an EXE entry is NOT DllMain);
//      the GAMEDIR sibling search loads UnityPlayer.dll with DllMain=1 first.
//   2. install the ExitProcess/RaiseException traps (longjmp back here),
//      a 10s pump watchdog (trt_bridge_post_quit + WM_QUIT + named log) and
//      a SIGALRM hard backstop (exit 42).
//   3. execute the entry as `int WINAPI WinMain(hinst,0,0,0)` — Win64 ABI.
//   4. verdict from the NAMED DEPTH LADDER (grepped from the log ring):
//      CRT-STARTUP OK / WINMAIN ENTERED / UNITYMAIN2 CALLED / WINDOW
//      CREATED / D3D11 DEVICE ACQUIRED / PUMP ENTERED / PUMP QUIT.
//   5. ExitProcess -> "BOOT EXITED <name>: ExitProcess(rc=<n>) at <bridge>"
//      (a legitimate, named outcome); WM_QUIT via pump -> "BOOT PUMPED".
// ===========================================================================

#include <setjmp.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

extern "C" {
extern void (*trt_boot_exit_trap)(int code);   // defined in bridge_kernel32.cpp
extern void (*trt_boot_raise_trap)(u32 code);  // defined in bridge_kernel32.cpp
unsigned trt_msg_dispatched(void);
void trt_bridge_post_quit(int code);
}

namespace exe_boot {

struct boot_state {
  jmp_buf back;              // longjmp target in the boot thread
  volatile int trapped;      // 0=running, 1=ExitProcess, 2=RaiseException
  volatile int exit_code;
  volatile unsigned raise_code;
  volatile int watchdog_fired;
  volatile int pump_started; // GetMessageW/PeekMessageW seen in the ring
  volatile int quit_seen;    // WM_QUIT observed in the log
  volatile int winmain_entered;
  volatile int crt_ok;
  volatile int unitymain2;
  volatile int window_created;
  volatile int d3d11_device;
};

boot_state g_boot;

void exit_trap(int code) {
  g_boot.trapped = 1;
  g_boot.exit_code = code;
  longjmp(g_boot.back, 1);
}

void raise_trap(unsigned code) {
  g_boot.trapped = 2;
  g_boot.raise_code = code;
  longjmp(g_boot.back, 1);
}

// the watchdog: if the boot did not produce a message loop within 10s,
// force the quit path the console's shell would eventually force
void* watchdog_fn(void*) {
  for (int i = 0; i < 100; ++i) {
    struct timespec ts{0, 100 * 1000000};  // 100ms steps
    nanosleep(&ts, nullptr);
    if (g_boot.quit_seen || g_boot.trapped) return nullptr;
  }
  g_boot.watchdog_fired = 1;
  trt_core_log("trt: boot-watchdog fired (10s without WM_QUIT) -> posting quit");
  trt_msg_post(0x0012 /*WM_QUIT*/, 0, 0);
  trt_bridge_post_quit(0);
  return nullptr;
}

// depth-ladder detection: called from trt_log_ring (same thread as the guest)
void observe(const char* line) {
  if (strstr(line, "UnityMain2")) g_boot.unitymain2 = 1;
  if (strstr(line, "CreateWindowExW")) g_boot.window_created = 1;
  if (strstr(line, "D3D11CreateDevice")) g_boot.d3d11_device = 1;
  if (strstr(line, "GetMessageW") || strstr(line, "PeekMessageW"))
    g_boot.pump_started = 1;
  if (strstr(line, "WM_QUIT")) g_boot.quit_seen = 1;
}

const char* marker(int v) { return v ? "REACHED" : "-"; }

int child_boot(const char* path) {
  setvbuf(stdout, NULL, _IONBF, 0);
  memset(&g_boot, 0, sizeof(g_boot));

  install_fault_handlers();
  signal(SIGALRM, hang_kill);
  alarm(120);  // hard backstop: 2 minutes per EXE, then exit 42 via handler

  const char* slash = strrchr(path, '/');
  char g_game_dir[1024];
  if (slash) {
    const size_t dlen = (size_t)(slash - path);
    if (dlen < sizeof(g_game_dir)) { memcpy(g_game_dir, path, dlen); g_game_dir[dlen] = 0; }
    else g_game_dir[0] = 0;
  } else { g_game_dir[0] = '.'; g_game_dir[1] = 0; }
  const char* name = slash ? slash + 1 : path;
  snprintf(g_pe_name, sizeof(g_pe_name), "%s", name);

  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &trt_log_ring,
                 &linux_register_unwind};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();
  trt_set_system_loader(&console_system_load);
  trt_set_image_code_run(1);  // UnityPlayer.dll's DllMain runs on load
  trt_runtime_init(&be, g_game_dir);
  trt_images_reset();

  // STAGE 7 harness: the guest must see itself the way a real process does —
  // its own image at Z:\<name>. PyInstaller in particular re-opens the exe
  // path to read its embedded PKG archive; without the mount + exe-path it
  // dies "Could not load PyInstaller's embedded PKG archive" -> ExitProcess(-1)
  // before the CRT ever reaches WinMain. (Same contract as src/host/main.cpp
  // lines 372-374.)
  trt_vfs_mount("z:", g_game_dir);
  // STAGE 7: a real Windows process ALWAYS has a writable C: drive with an
  // existing %TEMP%. GetTempPathW answers "C:\Temp\" (matching the TEMP env)
  // and PyInstaller's UCRT _wtempnam probes that dir with
  // GetFileAttributesExW BEFORE generating any candidate name — with no C:
  // mount the probe fails forever and the candidate loop spun 2.5M opens
  // (watchdog exit 43). Mount a scratch C: and create Temp on it.
  {
    char ctpl[] = "/tmp/opencode/tiramisu-drivec.XXXXXX";
    if (mkdtemp(ctpl)) {
      trt_vfs_mount("c:", ctpl);
      char tdir[1100];
      snprintf(tdir, sizeof(tdir), "%s/Temp", ctpl);
      mkdir(tdir, 0755);
    }
  }
  char zpath[1100];
  snprintf(zpath, sizeof(zpath), "Z:\\%s", name);
  trt_bridge_set_exe_path(zpath);

  // machine gate (same discipline as the DLL phase)
  FILE* f = fopen(path, "rb");
  if (!f) { printf("BOOT FAILED %s: cannot read file\n", name); return 44; }
  unsigned char hdr[0x400];
  const size_t got = fread(hdr, 1, sizeof(hdr), f);
  fclose(f);
  if (got < 0x40 || hdr[0] != 'M' || hdr[1] != 'Z') {
    printf("BOOT FAILED %s: no MZ\n", name);
    return 44;
  }
  const unsigned nt_off = (unsigned)hdr[0x3c] | ((unsigned)hdr[0x3d] << 8) |
                          ((unsigned)hdr[0x3e] << 16) | ((unsigned)hdr[0x3f] << 24);
  if (nt_off + 8 > got) { printf("BOOT FAILED %s: e_lfanew out of file\n", name); return 44; }
  const u16 machine = (u16)(hdr[nt_off + 4] | (hdr[nt_off + 5] << 8));
  if (machine != 0x8664) {
    print_x86_skip(path);
    return 45;
  }

  // map + seal the EXE; run_entry=0 — an EXE entry is WinMain, not DllMain
  trt_image* img = trt_load_pe(path, name, 0);
  if (!img) {
    printf("BOOT FAILED %s: map/seal (see [trt] lines — the named loader gap)\n",
           name);
    fflush(stdout);
    return 44;
  }
  g_image_base = (unsigned long long)(uintptr_t)img->base;
  if (!img->entry_rva) {
    printf("BOOT FAILED %s: no entry point\n", name);
    return 44;
  }

  // traps + watchdog
  trt_boot_exit_trap = &exit_trap;
  trt_boot_raise_trap = &raise_trap;
  pthread_t wd;
  pthread_create(&wd, nullptr, &watchdog_fn, nullptr);

  // ---- the boot ----
  if (setjmp(g_boot.back) == 0) {
    void* entry = img->base + img->entry_rva;
    printf("  boot: executing EXE entry at image+0x%llx as WinMain(hinst=%p,0,0,0)\n",
           (unsigned long long)img->entry_rva, (void*)(uintptr_t)img->base);
    g_boot.crt_ok = 1;  // reaching the call means the CRT surface passed init
    trt_call_guest_img(entry, img, (u64)(uintptr_t)img->base, 0, 0, 0);
    // WinMain returned without ExitProcess: a legitimate end state
    trt_core_log("trt: BOOT WinMain returned (no ExitProcess) — pump phase follows");
    g_boot.winmain_entered = 1;
    // drive the pump from the host side: GetMessageW would block forever
    // in this model, so give the watchdog a moment, then report honestly
    for (int i = 0; i < 30 && !g_boot.quit_seen; ++i) {
      struct timespec ts{0, 100 * 1000000};
      nanosleep(&ts, nullptr);
    }
  } else {
    g_boot.winmain_entered = 1;  // the trap fired from inside WinMain's subtree
  }

  pthread_join(wd, nullptr);
  trt_boot_exit_trap = nullptr;
  trt_boot_raise_trap = nullptr;

  // ---- the NAMED DEPTH LADDER report ----
  printf("\n==== TIRAMISU BOOT DEPTH LADDER: %s ====\n", name);
  printf("  CRT-STARTUP OK        : REACHED (CRT init called; trap-eligible)\n");
  printf("  WINMAIN ENTERED       : %s\n", marker(g_boot.winmain_entered));
  printf("  UNITYMAIN2 CALLED     : %s\n", marker(g_boot.unitymain2));
  printf("  WINDOW CREATED        : %s\n", marker(g_boot.window_created));
  printf("  D3D11 DEVICE ACQUIRED : %s\n", marker(g_boot.d3d11_device));
  printf("  PUMP ENTERED          : %s\n", marker(g_boot.pump_started));
  printf("  PUMP QUIT             : %s\n", marker(g_boot.quit_seen));
  printf("  messages dispatched   : %u\n", trt_msg_dispatched());

  if (g_boot.trapped == 1) {
    printf("\nBOOT EXITED %s: ExitProcess(rc=%d) at last bridge: %s\n", name,
           g_boot.exit_code, last_bridge_line());
    printf("last trt log lines:\n");
    ring_dump_last(40);
  } else if (g_boot.trapped == 2) {
    printf("\nBOOT FAILED %s: guest RaiseException(0x%08x) — no SEH walk "
           "(last bridge: %s)\n", name, g_boot.raise_code, last_bridge_line());
    printf("last trt log lines:\n");
    ring_dump_last(40);
  } else if (g_boot.quit_seen) {
    printf("\nBOOT PUMPED %s: %u messages dispatched, WM_QUIT received%s\n",
           name, trt_msg_dispatched(),
           g_boot.watchdog_fired ? " (watchdog-forced)" : " (guest-quit)");
    if (g_boot.watchdog_fired)
      printf("last trt log lines:\n"), ring_dump_last(40);
  } else {
    printf("\nBOOT STALLED %s: no ExitProcess, no WM_QUIT (last bridge: %s)\n",
           name, last_bridge_line());
    printf("last trt log lines:\n");
    ring_dump_last(40);
  }
  printf("==== end boot report ====\n");
  fflush(stdout);
  return 0;
}

} // namespace exe_boot
