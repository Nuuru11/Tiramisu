// tiramisu/tests/test_fna_platform.cpp
// Path B step 2 harness: the Tiramisu_FNAPlatform backend initializes
// headless under embedded Mono and reports a fully populated function
// table. The real checks live in the managed probe
// (tests/probe_fna_platform.cs); this driver feeds native input into
// the trt input layer (pre-run events + a mid-run mouse delta) and runs
// the probe under trt_mono_run.
//
// Usage:
//   test_fna_platform <probe-exe> <fna-out-dir> <libmono.so> <mode>
//   mode "config": backend selection via trt_mono_config.fna_platform
//                  ("tiramisu") -- the production path.
//   mode "env":    backend selection via the TIRAMISU_FNA_PLATFORM=1
//                  env var with fna_platform left unset.
// No game files, no display, no xvfb: the backend is headless offline.
// Missing inputs => SKIP (exit 0).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../src/trt/input.h"
#include "../src/trt/mono_host.h"
#include "../src/trt/pe.h"
#include "../src/trt/trt.h"

namespace {

std::atomic<int> g_mono_rc{-1};
std::atomic<int> g_exit_code{-1};
std::string g_mono_err;

bool file_exists(const char* path) {
  struct stat st;
  return path && ::stat(path, &st) == 0;
}

void game_thread_fn(trt_mono_config cfg) {
  int exit_code = 0;
  const int rc = trt_mono_run(&cfg, nullptr, 0, &exit_code);
  g_mono_err = trt_mono_last_error();
  g_exit_code.store(exit_code);
  g_mono_rc.store(rc);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr,
                 "usage: %s <probe-exe> <fna-out-dir> <libmono.so> "
                 "<config|env>\n",
                 argv[0]);
    return 2;
  }
  const char* probe_exe = argv[1];
  const char* fna_out = argv[2];
  const char* libmono = argv[3];
  const char* mode = argv[4];
  const bool use_config = std::strcmp(mode, "config") == 0;
  const bool use_env = std::strcmp(mode, "env") == 0;
  if (!use_config && !use_env) {
    std::fprintf(stderr, "FAIL fna/platform: unknown mode %s\n", mode);
    return 1;
  }

  if (!file_exists(probe_exe)) {
    std::printf("SKIP fna/platform: no probe at %s\n", probe_exe);
    return 0;
  }
  const std::string core_dll =
      std::string(fna_out) + "/Microsoft.Xna.Framework.dll";
  if (!file_exists(core_dll.c_str()) || !file_exists(libmono)) {
    std::printf("SKIP fna/platform: missing FNA core or libmono\n");
    return 0;
  }

  // Pre-run input feeds (the probe's PollEvents drains these):
  // absolute mouse move, left button down, wheel +120, key 'A' down,
  // and a printable char. The first absolute move positions with no
  // delta, so the relative-mode accumulator starts empty.
  trt_input_mouse_move(100, 200);
  trt_input_mouse_button(0, true);
  trt_input_wheel(120);
  trt_input_key_event2(0x41 /*VK_A*/, true, 0x1E, 0, 0);
  trt_input_char_event(0x78 /*'x'*/);

  if (use_env) {
    ::setenv("TIRAMISU_FNA_PLATFORM", "1", 1);
    ::unsetenv("FNA_PLATFORM_BACKEND");
  }

  std::printf("[harness] fna/platform: mode=%s probe=%s\n", mode, probe_exe);

  trt_mono_config cfg = {};
  cfg.exe_path = probe_exe;
  cfg.libmono_path = libmono;
  cfg.assemblies_path = fna_out;
  cfg.fna_platform = use_config ? "tiramisu" : nullptr;
  cfg.domain_name = "tiramisu-fna-probe";

  std::thread game(game_thread_fn, cfg);
  game.detach();

  // Mid-run mouse deltas for the probe's relative-mode check: the probe
  // enables relative mode early (draining the empty accumulator) and
  // then polls for this delta. Fed twice in case a slow probe drains
  // the first feed on enable; the feeds are 2s apart so they can never
  // accumulate into one poll read. The loop exits as soon as the probe
  // finishes, so the fast path takes ~3s.
  const auto start = std::chrono::steady_clock::now();
  bool fed1 = false, fed2 = false;
  for (;;) {
    const int rc = g_mono_rc.load();
    if (rc >= 0) {
      const int exit_code = g_exit_code.load();
      if (rc == TRT_OK && exit_code == 0) {
        std::printf("PASS fna/platform (%s): probe green\n", mode);
        return 0;
      }
      std::printf("FAIL fna/platform (%s): trt_mono_run rc=%d (%s) "
                  "probe exit=%d err=%s\n",
                  mode, rc, trt_errstr(rc), exit_code, g_mono_err.c_str());
      return 1;
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    if (!fed1 && elapsed >= std::chrono::seconds(3)) {
      trt_input_mouse_delta_raw(7, -4);
      fed1 = true;
    }
    if (!fed2 && elapsed >= std::chrono::seconds(5)) {
      trt_input_mouse_delta_raw(7, -4);
      fed2 = true;
    }
    if (elapsed >= std::chrono::seconds(30)) {
      std::printf("FAIL fna/platform (%s): timeout 30s\n", mode);
      _exit(1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}
