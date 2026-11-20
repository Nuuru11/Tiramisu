// tiramisu/src/trt/mono_host.cpp
// Mono embedding for managed (.NET) games — Path B (Celeste).
// See mono_host.h for the contract. The Linux path below is the real
// embedding (dlopen libmono-2.0, mono_jit_init, mono_jit_exec); the
// Windows path is an honest "not available yet" (no Mono runtime for UWP
// in-tree). Main is entered via mono_jit_exec — the same entry the mono
// CLI uses — because it sets the entry assembly: invoking Main by hand
// via mono_runtime_invoke leaves Assembly.GetEntryAssembly() NULL, and
// Celeste's Engine static constructor dereferences it (NRE in
// Monocle.Engine..cctor). The AppDomain base directory is set to the
// game's directory so content and relative paths resolve like the CLI.
#include "mono_host.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// ---- logging + last error (platform-independent) ----
void (*g_log)(const char* line) = nullptr;

void mono_log(const char* line) {
  if (g_log) {
    g_log(line);
  } else {
    std::fprintf(stderr, "mono: %s\n", line);
  }
}

char g_last_error[512] = "";

int fail(const char* detail) {
  std::snprintf(g_last_error, sizeof(g_last_error), "%s", detail);
  char line[600];
  std::snprintf(line, sizeof(line), "FAIL %s", detail);
  mono_log(line);
  return TRT_ERR_MONO;
}

// ---- PE parsing helpers (platform-independent) ----
u16 rd16(const u8* p) {
  return (u16)((u16)p[0] | ((u16)p[1] << 8));
}
u32 rd32(const u8* p) {
  return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

}  // namespace

extern "C" const char* trt_mono_last_error(void) { return g_last_error; }

extern "C" int trt_pe_is_managed(const u8* file, size_t file_size) {
  // DOS header: MZ + e_lfanew.
  if (!file || file_size < 64) return 0;
  if (rd16(file) != 0x5A4D) return 0;
  const u32 pe_off = rd32(file + 0x3C);
  // PE signature + COFF header (20 bytes) + optional-header magic.
  if ((size_t)pe_off + 24 + 2 > file_size) return 0;
  if (std::memcmp(file + pe_off, "PE\0\0", 4) != 0) return 0;
  const u8* coff = file + pe_off + 4;
  const u16 opt_size = rd16(coff + 16);
  const u8* opt = coff + 20;
  if ((size_t)pe_off + 24 + opt_size > file_size) return 0;
  if (opt_size < 2) return 0;
  const u16 magic = rd16(opt);
  // Data-directory count lives at different offsets in PE32 (0x5C) vs
  // PE32+ (0x6C); directory 14 (COM descriptor) follows the count.
  size_t dd_base;
  if (magic == 0x10B) {  // PE32
    if (opt_size < 0x60 + 8 * 15) return 0;
    dd_base = 0x60;
  } else if (magic == 0x20B) {  // PE32+
    if (opt_size < 0x70 + 8 * 15) return 0;
    dd_base = 0x70;
  } else {
    return 0;
  }
  const u8* dd = opt + dd_base + 14 * 8;
  const u32 clr_rva = rd32(dd);
  const u32 clr_size = rd32(dd + 4);
  return clr_rva != 0 && clr_size != 0;
}

#if defined(_WIN32)
// ---- Windows/UWP: no Mono runtime in-tree yet. Honest failure, not a fake. ----
extern "C" int trt_mono_run(const trt_mono_config* cfg,
                            const char* const* args, int nargs,
                            int* exit_code_out) {
  (void)args;
  (void)nargs;
  (void)exit_code_out;
  if (cfg && cfg->log) g_log = cfg->log;
  return fail("Mono embedding is not available in this build: no Mono "
              "runtime for UWP in-tree yet (needs a UWP Mono build; "
              "offline Linux harness only for now)");
}

#else
// ---- Linux: real embedding via dlopen'd libmono-2.0 ----
#include <dlfcn.h>
#include <unistd.h>

// The Mono headers provide exact declarations; the function-pointer table
// below is built from them with decltype, so a signature drift fails at
// compile time instead of corrupting a call at runtime.
#include <mono/jit/jit.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/blob.h>
#include <mono/metadata/image.h>
#include <mono/metadata/loader.h>
#include <mono/metadata/metadata.h>
#include <mono/metadata/mono-config.h>
#include <mono/metadata/object.h>

// domain.h is not shipped in the Ubuntu libmono-2.0-dev header set, so
// mono_domain_set_config is declared directly (signature per Mono's
// embedding docs and upstream mono/metadata/domain.h; the symbol is
// exported by libmonosgen-2.0 and resolved via dlsym like the rest).
extern "C" void mono_domain_set_config(MonoDomain* domain, const char* base_dir,
                                       const char* config_file_name);

namespace {

#define MONO_FN(name) decltype(&name) p_##name = nullptr
struct MonoApi {
  MONO_FN(mono_config_parse);
  MONO_FN(mono_set_dirs);
  MONO_FN(mono_set_assemblies_path);
  MONO_FN(mono_jit_parse_options);
  MONO_FN(mono_jit_init);
  MONO_FN(mono_jit_cleanup);
  MONO_FN(mono_domain_set_config);
  MONO_FN(mono_install_assembly_load_hook);
  MONO_FN(mono_domain_assembly_open);
  MONO_FN(mono_assembly_get_image);
  MONO_FN(mono_jit_exec);
  MONO_FN(mono_dllmap_insert);
};
#undef MONO_FN

MonoApi g_mono;
void* g_libmono = nullptr;

// dllmaps for the assembly-load hook (set for the duration of one run;
// Mono has no hook uninstall, and one run per process is the contract).
const trt_mono_dllmap* g_hook_dllmaps = nullptr;
int g_hook_dllmap_count = 0;

void assembly_load_hook(MonoAssembly* assembly, void* user_data) {
  (void)user_data;
  if (!g_hook_dllmaps || g_hook_dllmap_count <= 0) return;
  MonoImage* image = g_mono.p_mono_assembly_get_image(assembly);
  if (!image) return;
  // Insert BEFORE any P/Invoke in this image can resolve: the hook fires
  // at load time, and P/Invoke targets bind lazily on first call.
  for (int i = 0; i < g_hook_dllmap_count; ++i) {
    g_mono.p_mono_dllmap_insert(image, g_hook_dllmaps[i].module, nullptr,
                                g_hook_dllmaps[i].target, nullptr);
  }
}

bool load_libmono(const char* explicit_path, char* detail, size_t detail_cap) {
  const char* candidates[4];
  int n = 0;
  if (explicit_path && explicit_path[0]) candidates[n++] = explicit_path;
  candidates[n++] = "libmono-2.0.so.1";
  candidates[n++] = "libmonosgen-2.0.so.1";
  candidates[n++] = "libmonoboehm-2.0.so.1";
  const char* last_err = "";
  for (int i = 0; i < n; ++i) {
    g_libmono = dlopen(candidates[i], RTLD_NOW | RTLD_GLOBAL);
    if (g_libmono) {
      char line[512];
      std::snprintf(line, sizeof(line), "libmono loaded: %s", candidates[i]);
      mono_log(line);
      return true;
    }
    const char* e = dlerror();
    if (e) last_err = e;
  }
  std::snprintf(detail, detail_cap, "dlopen libmono failed: %s", last_err);
  return false;
}

#define RESOLVE(api, name)                                                    \
  do {                                                                        \
    api.p_##name = reinterpret_cast<decltype(&name)>(dlsym(g_libmono, #name)); \
    if (!api.p_##name) {                                                      \
      std::snprintf(detail, detail_cap, "libmono lacks symbol %s", #name);    \
      return false;                                                           \
    }                                                                         \
  } while (0)

bool resolve_api(char* detail, size_t detail_cap) {
  MonoApi& api = g_mono;
  RESOLVE(api, mono_config_parse);
  RESOLVE(api, mono_set_dirs);
  RESOLVE(api, mono_set_assemblies_path);
  RESOLVE(api, mono_jit_parse_options);
  RESOLVE(api, mono_jit_init);
  RESOLVE(api, mono_jit_cleanup);
  RESOLVE(api, mono_domain_set_config);
  RESOLVE(api, mono_install_assembly_load_hook);
  RESOLVE(api, mono_domain_assembly_open);
  RESOLVE(api, mono_assembly_get_image);
  RESOLVE(api, mono_jit_exec);
  RESOLVE(api, mono_dllmap_insert);
  return true;
}
#undef RESOLVE

}  // namespace

extern "C" int trt_mono_run(const trt_mono_config* cfg,
                            const char* const* args, int nargs,
                            int* exit_code_out) {
  g_last_error[0] = 0;
  if (exit_code_out) *exit_code_out = 0;
  if (!cfg || !cfg->exe_path || !cfg->exe_path[0])
    return fail("trt_mono_run: no exe_path");
  if (cfg->log) g_log = cfg->log;

  char detail[512];
  if (!load_libmono(cfg->libmono_path, detail, sizeof(detail)))
    return fail(detail);
  if (!resolve_api(detail, sizeof(detail))) return fail(detail);
  MonoApi& api = g_mono;

  // Machine config first (system dllmaps for the framework assemblies),
  // then dirs, then the assembly search path.
  api.p_mono_config_parse(nullptr);
  api.p_mono_set_dirs(cfg->framework_dir ? cfg->framework_dir : "/usr/lib",
                      cfg->config_dir ? cfg->config_dir : "/etc");
  if (cfg->assemblies_path && cfg->assemblies_path[0])
    api.p_mono_set_assemblies_path(cfg->assemblies_path);

  // Interpreter mode (no JIT) must be selected before mono_jit_init.
  // JIT is the default: it is what the Path B spike proved.
  if (cfg->interpreter) {
    char* opt_argv[2] = {(char*)"tiramisu-mono", (char*)"--interpreter"};
    api.p_mono_jit_parse_options(2, opt_argv);
    mono_log("mode: interpreter (no JIT)");
  } else {
    mono_log("mode: JIT");
  }

  // Install the dllmap hook BEFORE opening any assembly so every image —
  // the exe, its managed DLLs, lazily-loaded assemblies — gets the maps.
  g_hook_dllmaps = cfg->dllmaps;
  g_hook_dllmap_count = cfg->dllmap_count;
  api.p_mono_install_assembly_load_hook(assembly_load_hook, nullptr);

  {
    char line[700];
    std::snprintf(line, sizeof(line), "opening %s", cfg->exe_path);
    mono_log(line);
  }
  MonoDomain* domain = api.p_mono_jit_init(
      cfg->domain_name ? cfg->domain_name : "tiramisu-mono");
  if (!domain) return fail("mono_jit_init returned NULL");

  // Set the AppDomain base directory to the game's directory and apply
  // the game's .config file if it has one (Celeste.exe.config pins the
  // v4.0/.NET 4.5 runtime). Must happen before the main assembly is
  // opened. Without this, BaseDirectory is the process cwd and content
  // loads look in the wrong place (TitleContainer is BaseDirectory-
  // relative); the mono CLI does exactly this in its startup path.
  {
    const char* slash = std::strrchr(cfg->exe_path, '/');
    char game_dir[1024];
    if (!slash) {
      std::snprintf(game_dir, sizeof(game_dir), ".");
    } else if (slash == cfg->exe_path) {
      std::snprintf(game_dir, sizeof(game_dir), "/");
    } else {
      const size_t len = (size_t)(slash - cfg->exe_path);
      if (len >= sizeof(game_dir))
        return fail("exe_path directory too long");
      std::memcpy(game_dir, cfg->exe_path, len);
      game_dir[len] = 0;
    }
    char config_path[1120];
    std::snprintf(config_path, sizeof(config_path), "%s.config", cfg->exe_path);
    const char* config_file =
        (::access(config_path, F_OK) == 0) ? config_path : nullptr;
    api.p_mono_domain_set_config(domain, game_dir, config_file);
    char line[1200];
    std::snprintf(line, sizeof(line), "domain base dir: %s%s", game_dir,
                  config_file ? " (exe .config applied)" : "");
    mono_log(line);
  }

  MonoAssembly* assembly =
      api.p_mono_domain_assembly_open(domain, cfg->exe_path);
  if (!assembly) {
    std::snprintf(detail, sizeof(detail),
                  "mono_domain_assembly_open failed for %s", cfg->exe_path);
    return fail(detail);
  }

  // Enter Main via mono_jit_exec — the same entry the mono CLI uses. It
  // sets the entry assembly (so Assembly.GetEntryAssembly() works —
  // Celeste's Engine static constructor dereferences it) and invokes
  // Main with argv[1..] as its string[] args; argv[0] is the exe path.
  // It returns the game's exit code, or -1 when Main is missing or an
  // unhandled managed exception escapes (the exception + managed stack
  // trace are printed to stderr by the runtime itself). A game whose
  // Main legitimately returns -1 would be misreported as a failure;
  // Celeste's Main is void, so -1 is unambiguous here.
  mono_log("invoking managed Main");
  std::vector<char*> exec_argv;
  exec_argv.reserve((size_t)(nargs > 0 ? nargs : 0) + 1);
  exec_argv.push_back(const_cast<char*>(cfg->exe_path));
  for (int i = 0; i < nargs; ++i)
    exec_argv.push_back(const_cast<char*>(args[i] ? args[i] : ""));
  const int exec_rc = api.p_mono_jit_exec(
      domain, assembly, (int)exec_argv.size(), exec_argv.data());
  if (exec_rc == -1)
    return fail("mono_jit_exec failed: no Main or unhandled managed "
                "exception (see stderr)");
  if (exit_code_out) *exit_code_out = exec_rc;
  {
    char line[96];
    std::snprintf(line, sizeof(line), "managed Main returned (exit code %d)",
                  exec_rc);
    mono_log(line);
  }
  api.p_mono_jit_cleanup(domain);
  return TRT_OK;
}

#endif  // _WIN32
