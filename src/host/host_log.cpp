// tiramisu/src/host/host_log.cpp
// LocalState logger. Console reality (skill: xbox-devmode-uwp): ETW absent,
// launchers must write a log into LocalState — Device Portal pulls it back.
#include "host_log.h"

#include <winrt/base.h>
#include <winrt/Windows.Storage.h>

#include <cstdio>
#include <string>

namespace tiramisu {

namespace {
FILE* g_log = nullptr;
std::string g_local_state;
}

std::string log_open(const std::string& package_path) {
  using namespace winrt::Windows::Storage;
  std::wstring ws;
  try {
    ws = ApplicationData::Current().LocalFolder().Path();
  } catch (...) {
    // no ApplicationData (rare; e.g. wrong app model) — fall back beside exe
    ws = std::wstring(package_path.begin(), package_path.end());
  }
  // WinRT paths are plain NT paths (C:\Data\Users\...\LocalState)
  g_local_state.assign(ws.begin(), ws.end());
  const std::string path = g_local_state + "\\tiramisu.log";
  if (g_log) fclose(g_log);
  g_log = fopen(path.c_str(), "a");
  log_line("---- tiramisu session ----");
  return g_local_state;
}

void log_line(const char* line) {
  if (!g_log) return;
  fputs(line, g_log);
  fputc('\n', g_log);
  fflush(g_log);
}

void log_close() {
  if (g_log) fclose(g_log);
  g_log = nullptr;
}

} // namespace tiramisu
