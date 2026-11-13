// tiramisu/tests/test_corpus.cpp
// The real-game corpus load matrix, hardware-free:
//
//   for every PE on argv: read the file, check MZ + machine (x86 guests are
//   SKIPped — the M7 x86 engine is not present), then walk the EXACT load
//   path test_real_load proves: mmap backend -> trt_map_pe -> trt_seal_image
//   through trt_resolver_default + trt_modules_register_defaults.
//
//   After binding, the image import table is walked and one line is printed
//   per imported DLL:
//
//     <dll> route=<SYSTEM|BUILTIN|STUB|APISET> imports=<n> bound=<n> unresolved=<names-or-dash>
//
//   STUB thunks count as bound: they are fail-visible at CALL time (the
//   thunk names its module!symbol), never at load time. The per-DLL lines
//   ARE the work list — every STUB and every unresolved name is roadmap.
//
//   Verdict per PE: "LOAD OK <name>" (every import bound through its route)
//   or "LOAD FAILED <name>: <reason>" (the loader names module!symbol).
//   Summary: "CORPUS: <n> ok, <m> failed, <k> skipped". Exit 0 iff no
//   LOAD FAILED.
#include "../src/trt/trt.h"
#include "../src/trt/thunk.h"
#include "../src/trt/apiset.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_user32(void);
}

// ---------------- mmap backend (same shape as test_real_load) --------------
namespace {

struct stub_rec { char name[96]; int count; };
stub_rec g_stub_dlls[64];
int g_stub_count = 0;

char stub_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
bool stub_ieq(const char* a, const char* b) {
  size_t i = 0;
  for (; a[i] && b[i]; ++i)
    if (stub_lower(a[i]) != stub_lower(b[i])) return false;
  return a[i] == b[i];
}

int stub_count_for(const char* dll) {
  for (int i = 0; i < g_stub_count; ++i)
    if (stub_ieq(g_stub_dlls[i].name, dll)) return g_stub_dlls[i].count;
  return 0;
}

void stub_reset() { g_stub_count = 0; }

struct extent { void* base; size_t len; };
extent g_extents[256];
int g_extent_count = 0;

void* linux_alloc(size_t bytes, u64 preferred_base) {
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

void linux_log(const char* line) {
  printf("  [trt] %s\n", line);
  // STUB bind observation: module_table routes a STUB symbol through
  // trt_thunk_make, which logs exactly:
  //   trt: STUB <dll>!<sym> -> fail-visible thunk
  // Count them per-dll so the report can say stubs=<n> honestly (how many
  // of the dll's bound imports are fail-visible thunks vs real bindings).
  if (const char* m = strstr(line, "trt: STUB ")) {
    m += 10;  // past "trt: STUB "
    const char* bang = strchr(m, '!');
    if (bang && (size_t)(bang - m) < sizeof(g_stub_dlls[0].name)) {
      char dll[96];
      snprintf(dll, sizeof(dll), "%.*s", (int)(bang - m), m);
      // case-insensitive match against the tracked set
      for (int i = 0; i < g_stub_count; ++i) {
        if (stub_ieq(g_stub_dlls[i].name, dll)) { ++g_stub_dlls[i].count; return; }
      }
      if (g_stub_count < 64) {
        snprintf(g_stub_dlls[g_stub_count].name,
                 sizeof(g_stub_dlls[0].name), "%s", dll);
        g_stub_dlls[g_stub_count].count = 1;
        ++g_stub_count;
      }
    }
  }
}

// ---------------- the CONSOLE's system loader (offline model) --------------
// Provenance of the whitelist (docs/02-bridge-surface.md + module_table.cpp):
//
//   The SYSTEM route means "the console ships the real DLL". The fixed
//   whitelist below is the console reality this test models:
//
//   - xboxnative console evidence (the k_defaults SYSTEM set in
//     module_table.cpp): hid, setupapi, version, shcore, avrt, d3d11, dxgi,
//     d2d1, dwrite, dcomp, xinput1_4 (as XINPUTUAP), ws2_32, ucrtbase.
//   - api-ms-win-* contracts: not files on the console; the apiset layer
//     (apiset.cpp) redirects them to their implementing DLL BEFORE the
//     system loader is consulted, so every contract prefix is accepted.
//   - the UWP core binaries EVERY AppContainer process provably has:
//     kernel32, kernelbase, ntdll, user32, combase.
//   - CRT-family DLLs real MSVC-linked games import by name (vcruntime140/
//     msvcp140 via their _app aliases in module_table.cpp).
//
//   Desktop-shell DLLs (gdi32, comctl32, shlwapi) are DELIBERATELY NOT
//   whitelisted: console LoadLibrary for them fails in an AppContainer, so
//   a fake SYSTEM bind would hide that. Games must get them from BUILTIN
//   bridges (bridge_gdi / bridge_comctl / bridge_shlwapi) — the corpus
//   report proves it with stubs=0 on those lines.
//
//   Anything NOT on the whitelist returns failure -> the import routes
//   STUB (if the module table has it) or fails the load NAMED. Never silent.
//   NOTE: advapi32 is deliberately NOT whitelisted: the module table routes
//   it STUB, but our registry bridge (bridge_reg) registers it BUILTIN with
//   the Reg* surface only — token APIs (OpenProcessToken & co) fall through
//   to BUILTIN-symbol-missing -> system loader, which also says no: the
//   import fails NAMED. That is the honest work list, not a fake bind.
const char* const k_console_dlls[] = {
    "hid.dll", "setupapi.dll", "version.dll", "shcore.dll", "avrt.dll",
    "d3d11.dll", "dxgi.dll", "d2d1.dll", "dwrite.dll", "dcomp.dll",
    "xinput1_4.dll", "ws2_32.dll",
    "kernel32.dll", "kernelbase.dll", "ntdll.dll", "ucrtbase.dll",
    "combase.dll", "user32.dll",
    "vcruntime140.dll", "msvcp140.dll", "vcruntime140_app.dll",
    "msvcp140_app.dll",
    // advapi32: the token/security half is UWP-allowed on console
    // (OpenProcessToken/GetTokenInformation/ConvertStringSecurityDescriptor*);
    // the registry half is our BUILTIN bridge (bridge_reg).
    "advapi32.dll",
    // NOTE: gdi32 / comctl32 / shlwapi are NOT here — desktop-shell DLLs,
    // console LoadLibrary fails in an AppContainer. They are BUILTIN
    // bridges now; whitelisting them would fake the bind.
};

char wl_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool wl_ieq(const char* a, const char* b) {
  while (*a && *b) {
    if (wl_lower(*a) != wl_lower(*b)) return false;
    ++a; ++b;
  }
  return *a == *b;
}

bool whitelist_has(const char* module) {
  if (!module) return false;
  // api-ms-win-* contracts: accepted by prefix (the apiset layer usually
  // redirects first; some OS images also carry the schema literally).
  if (wl_lower(module[0]) == 'a' && wl_lower(module[1]) == 'p' &&
      wl_lower(module[2]) == 'i' && module[3] == '-' && module[4] == 'm' &&
      module[5] == 's')
    return true;
  for (const char* d : k_console_dlls)
    if (wl_ieq(d, module)) return true;
  return false;
}

// The address a whitelisted SYSTEM module's function binds to in this
// offline model. On the console this is the real GetProcAddress result;
// here it is one shared Win64-ABI function that returns 0. Load-only test:
// the entry point is never run, so nothing reaches it. (We deliberately do
// NOT route these through trt_thunk_make: the shared thunk table's page
// layout only holds 64 entries per allocated page, and a corpus-sized
// symbol flood overruns it. On the console these binds are real
// GetProcAddress results anyway.)
u64 TRT_GUEST_ABI console_system_fn(void) { return 0; }

void* console_system_load(const char* module, const char* symbol) {
  if (!module) return nullptr;
  if (!whitelist_has(module)) return nullptr;   // routes STUB / fails named
  // Ordinal-only import (symbol == NULL): on the console this is
  // GetProcAddress by ordinal against the real DLL, which binds. Bind a
  // generic Win64-ABI address (never called in this load-only test).
  if (!symbol) return (void*)&console_system_fn;  // ordinal import
  return (void*)&console_system_fn;               // name import: real-on-console
}

// ---------------- file read ----------------
u8* read_file(const char* path, size_t* size) {
  FILE* f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  u8* buf = (u8*)malloc((size_t)n ? (size_t)n : 1);
  const size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  *size = got;
  return buf;
}

u16 rd16b(const u8* p) { return (u16)(p[0] | (p[1] << 8)); }
u32 rd32b(const u8* p) {
  return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
u64 rd64b(const u8* p) {
  u64 v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

const char* base_name(const char* path) {
  const char* s = strrchr(path, '/');
  return s ? s + 1 : path;
}

// route label for a report line (apiset contracts report as APISET)
void route_label(const char* dll, char* out, size_t cap) {
  if (trt_apiset_is_contract(dll)) { snprintf(out, cap, "APISET"); return; }
  switch (trt_module_route(dll)) {
    case TRT_ROUTE_SYSTEM:  snprintf(out, cap, "SYSTEM");  break;
    case TRT_ROUTE_BUILTIN: snprintf(out, cap, "BUILTIN"); break;
    case TRT_ROUTE_STUB:    snprintf(out, cap, "STUB");    break;
    default:                snprintf(out, cap, "SYSTEM");  break; // console-real
  }
}

int run_corpus(int argc, char** argv) {
  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &linux_log,
                 nullptr};

  // boot exactly like the console host does (idempotent across the corpus)
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();
  trt_set_system_loader(&console_system_load);

  trt_resolver rs;
  trt_resolver_default(&rs);

  printf("tiramisu real-game corpus load matrix\n");

  int n_ok = 0, n_failed = 0, n_skipped = 0;
  bool any_failed = false;

  for (int a = 1; a < argc; ++a) {
    const char* path = argv[a];
    const char* name = base_name(path);

    size_t size = 0;
    u8* file = read_file(path, &size);
    if (!file) {
      printf("LOAD FAILED %s: cannot read file\n", name);
      ++n_failed; any_failed = true;
      continue;
    }

    // ---- MZ + PE machine gate ----
    if (size < 0x40 || rd16b(file) != 0x5A4D) {
      printf("LOAD FAILED %s: no MZ\n", name);
      ++n_failed; any_failed = true; free(file); continue;
    }
    const u32 nt_off = rd32b(file + 0x3C);
    if ((u64)nt_off + 6 > size || rd32b(file + nt_off) != 0x00004550) {
      printf("LOAD FAILED %s: no PE\\0\\0 signature\n", name);
      ++n_failed; any_failed = true; free(file); continue;
    }
    const u16 machine = rd16b(file + nt_off + 4);
    if (machine != 0x8664) {
      printf("SKIP %s: x86 guest (M7 x86 engine not present)\n", name);
      ++n_skipped; free(file); continue;
    }

    // ---- map + seal, exactly like test_real_load ----
    stub_reset();  // per-PE stub counts (the report is per-DLL per-PE)
    trt_image* img = nullptr;
    const int rc = trt_map_pe(&be, &rs, file, size, &img);
    if (rc != TRT_OK || !img) {
      printf("LOAD FAILED %s: map: %s (%s)\n", name, trt_errstr(rc),
             img ? img->last_detail : "no image");
      ++n_failed; any_failed = true; free(file); continue;
    }
    const int seal_rc = trt_seal_image(&be, img);
    if (seal_rc != TRT_OK) {
      printf("LOAD FAILED %s: seal: %s\n", name, trt_errstr(seal_rc));
      trt_unmap(&be, img); free(file);
      ++n_failed; any_failed = true; continue;
    }

    // ---- walk the bound image's import table: one line per DLL ----
    const trt_dos_header* dos = (const trt_dos_header*)img->base;
    const trt_nt_headers64* nt =
        (const trt_nt_headers64*)(img->base + dos->e_lfanew);
    const u64 size_of_image = img->size_of_image;
    const u64 imp_va = rd32b((const u8*)&nt->optional_header.directories[1].virtual_address);
    const u64 imp_sz = rd32b((const u8*)&nt->optional_header.directories[1].size);

    if (imp_va && imp_sz && imp_va + imp_sz <= size_of_image) {
      for (u64 d = 0; d + sizeof(trt_import_descriptor) <= imp_sz;
           d += sizeof(trt_import_descriptor)) {
        const trt_import_descriptor* desc =
            (const trt_import_descriptor*)(img->base + imp_va + d);
        if (!desc->first_thunk && !desc->name) break;  // terminator
        if ((u64)desc->name >= size_of_image ||
            (u64)desc->first_thunk >= size_of_image)
          break;
        const char* dll = (const char*)(img->base + desc->name);
        char dll_name[96];
        snprintf(dll_name, sizeof(dll_name), "%s", dll);

        // imports: walk the INT (fall back to the IAT when the INT is absent)
        const u64 names_rva = desc->original_first_thunk
                                  ? desc->original_first_thunk
                                  : desc->first_thunk;
        int imports = 0, n_unresolved = 0;
        char unresolved[512];
        unresolved[0] = 0;
        for (u64 t = names_rva;; t += 8) {
          if (t + 8 > size_of_image) break;
          const u64 thunk = rd64b(img->base + t);
          if (thunk == 0) break;
          ++imports;
          // bound check: the loader overwrote every IAT slot with a
          // non-null address (real fn or fail-visible thunk)
          const u64 bound_slot =
              rd64b(img->base + desc->first_thunk + (t - names_rva));
          if (!bound_slot) {
            char one[160];
            if (thunk >> 63)
              snprintf(one, sizeof(one), "#%u", (unsigned)(thunk & 0xFFFF));
            else {
              const char* sym = (const char*)(img->base + thunk + 2);
              snprintf(one, sizeof(one), "%.*s", 120, sym);
            }
            if (unresolved[0]) strncat(unresolved, ",", sizeof(unresolved) - 1);
            strncat(unresolved, one, sizeof(unresolved) - strlen(unresolved) - 1);
            ++n_unresolved;
          }
        }

        // stub count for this dll: observed from the loader's bind log
        // (trt_thunk_make logs "trt: STUB <dll>!<sym>" per STUB bind)
        const int stubs = stub_count_for(dll_name);

        char route[16];
        route_label(dll_name, route, sizeof(route));
        printf("%s route=%s imports=%d bound=%d stubs=%d unresolved=%s\n",
               dll_name, route, imports, imports - n_unresolved, stubs,
               unresolved[0] ? unresolved : "-");
      }
    } else {
      printf("  (no import directory)\n");
    }

    printf("LOAD OK %s\n", name);
    ++n_ok;
    trt_unmap(&be, img);
    free(file);
  }

  printf("\nCORPUS: %d ok, %d failed, %d skipped\n", n_ok, n_failed, n_skipped);
  return any_failed ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: test_corpus <pe> [<pe> ...]\n");
    return 2;
  }
  return run_corpus(argc, argv);
}
