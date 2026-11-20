// tiramisu/tests/test_mono_celeste.cpp
// Path B harness: Celeste.exe under the embedded Mono runtime (Phase
// 1-equivalent gate for Path B). The game runs in-process on a worker
// thread; the test PASSES when the game's own log shows it reached the
// title screen (GAME DISPLAYED + DONE LOADING), and FAILS on timeout or a
// Mono embedding error. The game loop is killed by _exit once the gate
// markers are seen — the test's job is the boot, not the play session.
//
// Usage:
//   test_mono_celeste <celeste-exe> <fna-out-dir> <libfmod-real.so>
//                     <libcsteamworks-real.so> <libmono.so> [timeout-secs]
// Env:
//   TIRAMISU_MONO_INTERPRETER=1  run Mono in interpreter mode (no JIT)
// Requires a display (run under xvfb-run; see build-test.sh section 45).
// Missing game files => SKIP (exit 0): other machines have no Celeste.
//
// The game runs with cwd + HOME redirected to a scratch dir so its own
// writes (logs, saves) never touch the game folder. The game folder itself
// is only ever read.
//
// REAL-BRIDGE ASSERTIONS (this test FAILS if the old spike-parity stubs
// are loaded instead of the real bridges):
//   * the .so files must export trt_mono_bridge_is_real()==1 and
//     trt_mono_bridge_name()=="fmod-real"/"steam-real" (the stubs
//     export neither);
//   * a counting voice hook installed into libfmod-real.so before the
//     game starts must receive real PCM during boot (the worldmap
//     ambience event starts during loading);
//   * trt_fmod_studio_global_event_count() must be > 0 after boot (real
//     banks parsed — the stubs load nothing);
//   * the Steam door chain SteamClient_ -> GetISteamUser ->
//     ISteamUser_GetSteamID must return the bridged ID
//     0x0110000100000001 through the real bridge.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../src/trt/mono_host.h"
#include "../src/trt/pe.h"
#include "../src/trt/trt.h"

namespace {

int g_saved_out = -1;   // the test's real stdout (fd 1 is the game pipe)
int g_log_fd = -1;      // captured game log
std::atomic<bool> g_saw_displayed{false};
std::atomic<bool> g_saw_loaded{false};
std::atomic<bool> g_saw_steam_abort{false};
std::atomic<bool> g_saw_unhandled{false};
std::atomic<bool> g_reader_done{false};
std::atomic<int> g_mono_rc{-1};
std::string g_mono_err;

// Real-bridge evidence, collected while the game runs.
std::atomic<int> g_pcm_submits{0};
std::atomic<unsigned long long> g_pcm_bytes{0};
std::atomic<int> g_pcm_rate{0};
std::atomic<int> g_pcm_channels{0};

int pcm_hook(unsigned long long voice, const void* pcm, unsigned bytes,
             unsigned rate, unsigned channels, unsigned bps,
             unsigned is_float, float volume, void* user) {
  (void)voice;
  (void)pcm;
  (void)bps;
  (void)is_float;
  (void)volume;
  (void)user;
  g_pcm_submits.fetch_add(1);
  g_pcm_bytes.fetch_add(bytes);
  g_pcm_rate.store((int)rate);
  g_pcm_channels.store((int)channels);
  return 1;
}

void verdict(const char* line) {
  // Unbuffered, straight to the real stdout: _exit() follows, so nothing
  // may sit in a stdio buffer.
  if (g_saved_out >= 0) {
    (void)!write(g_saved_out, line, std::strlen(line));
    (void)!write(g_saved_out, "\n", 1);
  }
}

bool file_exists(const char* path) {
  struct stat st;
  return path && ::stat(path, &st) == 0;
}

// Reader thread: drains the game pipe, tees every line to the real stdout
// + the log file, and watches for the gate markers.
void pipe_reader(int read_fd) {
  std::string pending;
  char buf[8192];
  for (;;) {
    const ssize_t n = read(read_fd, buf, sizeof(buf));
    if (n <= 0) break;
    if (g_log_fd >= 0) (void)!write(g_log_fd, buf, (size_t)n);
    if (g_saved_out >= 0) (void)!write(g_saved_out, buf, (size_t)n);
    pending.append(buf, (size_t)n);
    size_t pos;
    while ((pos = pending.find('\n')) != std::string::npos) {
      const std::string line = pending.substr(0, pos);
      pending.erase(0, pos + 1);
      if (line.find("GAME DISPLAYED") != std::string::npos)
        g_saw_displayed.store(true);
      if (line.find("DONE LOADING") != std::string::npos)
        g_saw_loaded.store(true);
      if (line.find("Steam not found") != std::string::npos)
        g_saw_steam_abort.store(true);
      if (line.find("Unhandled Exception") != std::string::npos)
        g_saw_unhandled.store(true);
    }
  }
  g_reader_done.store(true);
}

void game_thread_fn(trt_mono_config cfg, const char* const* argv, int argc) {
  int exit_code = 0;
  const int rc = trt_mono_run(&cfg, argv, argc, &exit_code);
  g_mono_err = trt_mono_last_error();
  char line[700];
  std::snprintf(line, sizeof(line),
                "[harness] trt_mono_run returned rc=%d (%s) exit_code=%d",
                rc, trt_errstr(rc), exit_code);
  verdict(line);
  g_mono_rc.store(rc);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 6) {
    std::fprintf(stderr,
                 "usage: %s <celeste-exe> <fna-out-dir> <libfmod-real.so> "
                 "<libcsteamworks-real.so> <libmono.so> [timeout-secs]\n",
                 argv[0]);
    return 2;
  }
  const char* exe = argv[1];
  const char* fna_out = argv[2];
  const char* fmod_so = argv[3];
  const char* steam_so = argv[4];
  const char* libmono = argv[5];
  const int timeout_secs = argc > 6 ? std::atoi(argv[6]) : 90;

  if (!file_exists(exe)) {
    std::printf("SKIP mono/celeste: no game at %s\n", exe);
    return 0;
  }
  const std::string core_dll = std::string(fna_out) + "/Microsoft.Xna.Framework.dll";
  const std::string gfx_dll =
      std::string(fna_out) + "/Microsoft.Xna.Framework.Graphics.dll";
  const std::string game_dll =
      std::string(fna_out) + "/Microsoft.Xna.Framework.Game.dll";
  for (const char* p : {core_dll.c_str(), gfx_dll.c_str(), game_dll.c_str(),
                        fmod_so, steam_so, libmono}) {
    if (!file_exists(p)) {
      std::printf("SKIP mono/celeste: missing input %s\n", p);
      return 0;
    }
  }

  // NO-STUB ASSERTION 1: the .so files must be the REAL bridges. dlopen
  // them here (dlopen of the same path later returns the same object
  // Mono uses) and check the marker exports the stubs do not have.
  void* hf = ::dlopen(fmod_so, RTLD_NOW | RTLD_LOCAL);
  if (!hf) {
    std::printf("FAIL mono/celeste: dlopen %s: %s\n", fmod_so, ::dlerror());
    return 1;
  }
  void* hs = ::dlopen(steam_so, RTLD_NOW | RTLD_LOCAL);
  if (!hs) {
    std::printf("FAIL mono/celeste: dlopen %s: %s\n", steam_so, ::dlerror());
    return 1;
  }
  {
    ::dlerror();
    auto fmod_is_real = (int (*)())::dlsym(hf, "trt_mono_bridge_is_real");
    auto fmod_name =
        (const char* (*)())::dlsym(hf, "trt_mono_bridge_name");
    auto steam_is_real = (int (*)())::dlsym(hs, "trt_mono_bridge_is_real");
    auto steam_name =
        (const char* (*)())::dlsym(hs, "trt_mono_bridge_name");
    if (!fmod_is_real || !fmod_name || fmod_is_real() != 1 ||
        std::strcmp(fmod_name(), "fmod-real") != 0) {
      std::printf("FAIL mono/celeste: %s is NOT the real FMOD bridge "
                  "(missing trt_mono_bridge_is_real/name markers — "
                  "a stub .so was passed)\n",
                  fmod_so);
      return 1;
    }
    if (!steam_is_real || !steam_name || steam_is_real() != 1 ||
        std::strcmp(steam_name(), "steam-real") != 0) {
      std::printf("FAIL mono/celeste: %s is NOT the real Steam bridge "
                  "(missing trt_mono_bridge_is_real/name markers — "
                  "a stub .so was passed)\n",
                  steam_so);
      return 1;
    }
  }
  // Install the counting voice hook into the FMOD .so BEFORE the game
  // starts, so boot audio (the worldmap ambience) is observed.
  {
    ::dlerror();
    auto set_hook =
        (void (*)(void*, void*))::dlsym(hf, "trt_audio_set_voice_hook");
    if (!set_hook) {
      std::printf("FAIL mono/celeste: %s has no trt_audio_set_voice_hook\n",
                  fmod_so);
      return 1;
    }
    set_hook((void*)pcm_hook, nullptr);
  }

  // Managed-exe sanity: the game must have a CLR header (else the native
  // loader owns it and this test is meaningless).
  {
    FILE* f = std::fopen(exe, "rb");
    if (!f) {
      std::printf("SKIP mono/celeste: cannot open %s\n", exe);
      return 1;
    }
    std::vector<u8> head(4096);
    const size_t n = std::fread(head.data(), 1, head.size(), f);
    std::fclose(f);
    if (!trt_pe_is_managed(head.data(), n)) {
      std::printf("FAIL mono/celeste: %s has no CLR header (not managed)\n",
                  exe);
      return 1;
    }
  }

  // Scratch cwd + HOME: the game's own writes land here, never in the
  // game folder.
  const std::string scratch =
      "/tmp/mono-celeste-" + std::to_string((long long)getpid());
  if (::mkdir(scratch.c_str(), 0755) != 0) {
    std::fprintf(stderr, "FAIL: cannot mkdir %s\n", scratch.c_str());
    return 1;
  }
  ::setenv("HOME", scratch.c_str(), 1);
  if (::chdir(scratch.c_str()) != 0) {
    std::fprintf(stderr, "FAIL: cannot chdir %s\n", scratch.c_str());
    return 1;
  }
  // FNA defaults to SDL3; mineplex has SDL2 only. (The next step's
  // Tiramisu_FNAPlatform backend removes SDL entirely on the console.)
  ::setenv("FNA_PLATFORM_BACKEND", "SDL2", 1);

  // Pipe the game's stdout/stderr through the marker reader.
  g_saved_out = ::dup(STDOUT_FILENO);
  int pipefd[2];
  if (::pipe(pipefd) != 0) {
    std::fprintf(stderr, "FAIL: pipe() failed\n");
    return 1;
  }
  const std::string log_path = scratch + "/celeste.log";
  g_log_fd = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  std::thread reader(pipe_reader, pipefd[0]);
  // From here on, fd 1/2 are the game pipe. The test's own output goes
  // through verdict() (saved stdout) or the reader's tee.
  ::dup2(pipefd[1], STDOUT_FILENO);
  ::dup2(pipefd[1], STDERR_FILENO);
  ::close(pipefd[1]);

  {
    char line[1024];
    std::snprintf(line, sizeof(line),
                  "[harness] mono/celeste: exe=%s fna=%s timeout=%ds "
                  "scratch=%s log=%s",
                  exe, fna_out, timeout_secs, scratch.c_str(),
                  log_path.c_str());
    verdict(line);
  }

  trt_mono_dllmap maps[4] = {
      {"fmod", fmod_so},
      {"fmodstudio", fmod_so},
      {"CSteamworks", steam_so},
      {"sdkencryptedappticket", steam_so},
  };
  trt_mono_config cfg = {};
  cfg.exe_path = exe;
  cfg.libmono_path = libmono;
  cfg.assemblies_path = fna_out;
  cfg.dllmaps = maps;
  cfg.dllmap_count = 4;
  cfg.interpreter =
      std::getenv("TIRAMISU_MONO_INTERPRETER") &&
              std::strcmp(std::getenv("TIRAMISU_MONO_INTERPRETER"), "1") == 0
          ? 1
          : 0;
  cfg.domain_name = "tiramisu-mono-celeste";

  const char* game_argv[1] = {nullptr};
  std::thread game(game_thread_fn, cfg, game_argv, 0);
  game.detach();

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeout_secs);
  for (;;) {
    if (g_saw_displayed.load() && g_saw_loaded.load()) {
      // NO-STUB ASSERTIONS 2-4 (after the gate markers): real PCM
      // reached the voice hook, real banks were parsed, and the Steam
      // door chain returns the bridged ID. Give the feeder thread up
      // to 3 s after the markers to submit its first chunk.
      const auto pcm_deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (g_pcm_submits.load() == 0 &&
             std::chrono::steady_clock::now() < pcm_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (g_pcm_submits.load() == 0 || g_pcm_bytes.load() == 0) {
        verdict("FAIL mono/celeste: title screen reached but NO PCM "
                "reached the voice hook — the real FMOD bridge did not "
                "play the boot ambience");
        _exit(1);
      }
      {
        ::dlerror();
        auto global_events = (int (*)())::dlsym(
            hf, "trt_fmod_studio_global_event_count");
        if (!global_events || global_events() <= 0) {
          verdict("FAIL mono/celeste: title screen reached but the "
                  "Studio engine reports zero parsed events — no real "
                  "banks were loaded");
          _exit(1);
        }
        char line[256];
        std::snprintf(line, sizeof(line),
                      "[harness] real-bridge evidence: PCM %d submits, "
                      "%llu bytes, %d Hz %d ch; Studio events parsed: %d",
                      g_pcm_submits.load(), g_pcm_bytes.load(),
                      g_pcm_rate.load(), g_pcm_channels.load(),
                      global_events());
        verdict(line);
      }
      {
        ::dlerror();
        auto steam_client = (void* (*)())::dlsym(hs, "SteamClient_");
        auto get_user = (void* (*)(int, int, const char*))::dlsym(
            hs, "ISteamClient_GetISteamUser");
        auto get_id =
            (unsigned long long (*)())::dlsym(hs, "ISteamUser_GetSteamID");
        if (!steam_client || !get_user || !get_id) {
          verdict("FAIL mono/celeste: Steam door chain symbols missing "
                  "from the .so");
          _exit(1);
        }
        void* client = steam_client();
        void* user = client ? get_user(1, 1, "SteamUser023") : nullptr;
        (void)user;  // the door takes the user object from the bridge
        unsigned long long id = get_id();
        if (id != 0x0110000100000001ULL) {
          char line[256];
          std::snprintf(line, sizeof(line),
                        "FAIL mono/celeste: Steam GetSteamID returned "
                        "%llu, expected 76561197960265729",
                        id);
          verdict(line);
          _exit(1);
        }
      }
      verdict("PASS mono/celeste: GAME DISPLAYED + DONE LOADING "
              "(title screen reached under embedded Mono) + real FMOD "
              "PCM + real banks + bridged Steam ID");
      _exit(0);
    }
    const int rc = g_mono_rc.load();
    if (rc >= 0) {
      char line[1024];
      std::snprintf(line, sizeof(line),
                    "FAIL mono/celeste: game thread ended early rc=%d (%s) "
                    "displayed=%d loaded=%d err=%s",
                    rc, trt_errstr(rc), (int)g_saw_displayed.load(),
                    (int)g_saw_loaded.load(), g_mono_err.c_str());
      verdict(line);
      _exit(1);
    }
    if (g_saw_steam_abort.load()) {
      verdict("FAIL mono/celeste: game aborted 'Steam not found!' "
              "(Steam bridge Init/IsSteamRunning did not return true)");
      _exit(1);
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      char line[1024];
      std::snprintf(line, sizeof(line),
                    "FAIL mono/celeste: timeout %ds (displayed=%d loaded=%d "
                    "unhandled=%d) — see %s",
                    timeout_secs, (int)g_saw_displayed.load(),
                    (int)g_saw_loaded.load(), (int)g_saw_unhandled.load(),
                    log_path.c_str());
      verdict(line);
      _exit(1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}
