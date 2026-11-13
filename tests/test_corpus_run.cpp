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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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

extern "C" void trt_log_ring(const char* line) {
  ring_put(line);
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
// IAT would corrupt rcx/rdx/r8/r9 arg registers. It answers 0 — the
// honest-minimal answer: fail-visible (the bridge log names every call
// through trt_thunk_dispatch only for STUB binds; SYSTEM binds log at bind
// time), and never a fake success the console could not reproduce.
u64 TRT_GUEST_ABI console_system_fn(void) { return 0; }

void* console_system_load(const char* module, const char* symbol) {
  if (!module) return nullptr;
  if (!whitelist_has(module)) return nullptr;
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
                 nullptr};
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
    printf("usage: test_corpus_run <dll> [<dll> ...]\n");
    return 2;
  }

  g_ring = (trt_ring*)mmap(nullptr, sizeof(trt_ring), PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (g_ring == MAP_FAILED) { fprintf(stderr, "ring mmap failed\n"); return 2; }
  memset(g_ring, 0, sizeof(*g_ring));

  int n_ok = 0, n_failed = 0, n_no_verdict = 0, n_skipped = 0;
  for (int a = 1; a < argc; ++a) {
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
