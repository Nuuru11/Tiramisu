// tiramisu/tests/test_settings_linkage.cpp
// The trt bridge layer resolves its settings hooks as weak `extern "C"`
// symbols. The getters used to live only in namespace longan (C++ linkage),
// so the linker never matched the bridges' declarations: the hooks stayed
// null and six documented knobs (OS personality, write-watch, pad vibration,
// input deadzone, memory budget, audio buffer) silently reported their
// built-in defaults, whatever the user set.
//
// This test declares the hooks exactly as the bridges do but STRONG: if
// src/host/settings.cpp stops exporting any of them the binary fails to
// link, and if one stops forwarding the stored value the check fails.
#include "../src/host/settings.h"

#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

// ---- the bridge-side contract (same names, same C linkage) ----
extern "C" {
void trt_settings_os(int*, int*, int*);
bool trt_settings_write_watch();
bool trt_settings_pad_vibration();
int trt_settings_input_deadzone(void);
int trt_settings_memory_budget_mb(void);
int trt_settings_audio_buffer_ms();
}

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg)                                 \
  do {                                                   \
    if (cond) {                                          \
      ++g_pass;                                          \
      printf("  ok: %s\n", (msg));                       \
    } else {                                             \
      ++g_fail;                                          \
      printf("  FAIL: %s (line %d)\n", (msg), __LINE__); \
    }                                                    \
  } while (0)

int main() {
  printf("=== Settings linkage: bridge hooks resolve and forward ===\n");

#ifdef _WIN32
  const std::string root = "./trt_settings_linkage_test";
  _mkdir(root.c_str());
#else
  const std::string root = "/tmp/trt_settings_linkage_test";
  mkdir(root.c_str(), 0755);
#endif

  // The symbols exist (a missing definition is already a link error above).
  CHECK(&trt_settings_os != nullptr, "trt_settings_os is exported with C linkage");
  CHECK(&trt_settings_write_watch != nullptr,
        "trt_settings_write_watch is exported with C linkage");
  CHECK(&trt_settings_pad_vibration != nullptr,
        "trt_settings_pad_vibration is exported with C linkage");
  CHECK(&trt_settings_input_deadzone != nullptr,
        "trt_settings_input_deadzone is exported with C linkage");
  CHECK(&trt_settings_memory_budget_mb != nullptr,
        "trt_settings_memory_budget_mb is exported with C linkage");
  CHECK(&trt_settings_audio_buffer_ms != nullptr,
        "trt_settings_audio_buffer_ms is exported with C linkage");

  // Store values that differ from every built-in default, then read them
  // back through the bridge-side hooks.
  longan::host_settings s;
  s.os_major = 10;
  s.os_minor = 0;
  s.os_build = 22621;
  s.win11_personality = false;
  s.write_watch = false;
  s.pad_vibration = false;
  s.input_deadzone = 25;
  s.memory_budget_mb = 3072;
  s.audio_buffer_ms = 30;
  longan::settings_store(s, root);

  int mj = 0, mn = 0, bd = 0;
  trt_settings_os(&mj, &mn, &bd);
  CHECK(mj == 10 && mn == 0 && bd == 22621,
        "trt_settings_os forwards the stored OS personality (22621)");

  CHECK(trt_settings_write_watch() == false,
        "write_watch=false reaches the GetWriteWatch bridge");
  CHECK(trt_settings_pad_vibration() == false,
        "pad_vibration=false reaches the rumble bridge");
  CHECK(trt_settings_input_deadzone() == 25,
        "input_deadzone=25 reaches the input bridge");
  CHECK(trt_settings_memory_budget_mb() == 3072,
        "memory_budget_mb=3072 reaches the msvcrt allocator");
  CHECK(trt_settings_audio_buffer_ms() == 30,
        "audio_buffer_ms=30 reaches the WASAPI bridge");

  // win11_personality still floors the reported build at 22000 even when the
  // stored build is lower — the same rule the dashboard promises.
  s.os_build = 19045;
  s.win11_personality = true;
  longan::settings_store(s, root);
  mj = mn = bd = 0;
  trt_settings_os(&mj, &mn, &bd);
  CHECK(bd >= 22000, "win11_personality floors the reported build at 22000");

  printf("%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
