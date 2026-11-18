// tiramisu/src/host/host_log.cpp
// LocalState logger. Console reality (skill: xbox-devmode-uwp): ETW absent,
// launchers must write a log into LocalState — Device Portal pulls it back.
// Also keeps an in-memory ring so the TV dashboard and the web dashboard can
// show the live log without re-reading the file.
#include "host_log.h"

#include <winrt/base.h>
#include <winrt/Windows.Storage.h>

#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

namespace longan {

namespace {
FILE* g_log = nullptr;
std::string g_local_state;

std::mutex g_ring_mu;
std::deque<std::string> g_ring;
constexpr unsigned k_ring_max = 200;

void ring_push(const char* line) {
  std::lock_guard<std::mutex> lk(g_ring_mu);
  std::string s = line;
  if (s.size() > 220) s.resize(220);
  if (g_ring.size() >= k_ring_max) g_ring.pop_front();
  g_ring.push_back(std::move(s));
}
} // namespace

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
  if (!line) return;
  ring_push(line);
  if (!g_log) return;
  fputs(line, g_log);
  fputc('\n', g_log);
  fflush(g_log);
}

void log_get_recent(char* out, size_t cap) {
  if (!out || !cap) return;
  out[0] = 0;
  std::lock_guard<std::mutex> lk(g_ring_mu);
  size_t used = 0;
  for (const auto& s : g_ring) {
    const size_t need = s.size() + 1;
    if (used + need >= cap) break;
    memcpy(out + used, s.c_str(), need);
    used += need;
  }
}

unsigned log_recent_count() {
  std::lock_guard<std::mutex> lk(g_ring_mu);
  return (unsigned)g_ring.size();
}

size_t log_read_disk_tail(char* out, size_t cap) {
  if (!out || !cap) return 0;
  out[0] = 0;
  if (g_local_state.empty()) return 0;
  const std::string path = g_local_state + "\\\\tiramisu.log";
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return 0;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  if (sz < 0) sz = 0;
  long off = 0;
  if ((size_t)sz > cap - 1) off = sz - (long)(cap - 1);  // tail only
  fseek(f, off, SEEK_SET);
  size_t n = fread(out, 1, (size_t)(sz - off), f);
  out[n] = 0;
  fclose(f);
  return n;
}

void log_close() {
  if (g_log) fclose(g_log);
  g_log = nullptr;
}

} // namespace longan
