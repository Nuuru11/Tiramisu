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
#include <chrono>

#include "../trt/trt_trace.h"

namespace longan {

namespace {
FILE* g_log = nullptr;
std::string g_local_state;

std::mutex g_ring_mu;
std::deque<std::string> g_ring;
constexpr unsigned k_ring_max = 200;

// On-disk log rotation: the log used to grow without bound (a wedged app
// spamming the log fills LocalState) and log_line had no lock around the
// FILE* writes (interleaved lines from the web + guest threads). 8 MB per
// file, 2 backups = at most 24 MB of log on disk.
std::timed_mutex g_file_mu;  // timed: log_line try_lock diagnoses H2
long long g_bytes_written = 0;
constexpr long long k_log_max_bytes = 8ll * 1024 * 1024;

// Rotate the log file. Caller must hold g_file_mu. Closes the current
// file, shifts .1 -> .2 and .log -> .1, opens a fresh .log.
void rotate_locked() {
  if (g_log) { fclose(g_log); g_log = nullptr; }
  const std::string base = g_local_state + "\\tiramisu.log";
  remove((base + ".2").c_str());
  rename((base + ".1").c_str(), (base + ".2").c_str());
  rename(base.c_str(), (base + ".1").c_str());
  g_log = fopen(base.c_str(), "a");
  g_bytes_written = 0;
  if (g_log) {
    const char* marker = "---- log rotated ----\n";
    fputs(marker, g_log);
    fflush(g_log);
    g_bytes_written += (long long)strlen(marker);
  }
}

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
  {
    std::lock_guard<std::timed_mutex> lk(g_file_mu);
    if (g_log) fclose(g_log);
    g_log = fopen(path.c_str(), "a");
    g_bytes_written = 0;
    if (g_log) {
      // seed the counter from the existing file size so a log that
      // already grew past the cap rotates at open instead of growing
      // without bound
      _fseeki64(g_log, 0, SEEK_END);
      long long sz = _ftelli64(g_log);
      if (sz > 0) g_bytes_written = sz;
      if (g_bytes_written >= k_log_max_bytes) rotate_locked();
    }
  }
  trt_trace_init(path.c_str());
  log_line("---- tiramisu session ----");
  return g_local_state;
}

void log_line(const char* line) {
  if (!line) return;
  ring_push(line);
  // H2 diagnostic: if g_file_mu is held >5s (log deadlock), do NOT
  // block forever -- leave a marker via the allocation-free trace
  // writer instead.
  std::unique_lock<std::timed_mutex> lk(g_file_mu, std::defer_lock);
  if (!lk.try_lock_for(std::chrono::seconds(5))) {
    trt_trace("TRACE LOG-DEADLOCK: g_file_mu held >5s (H2)");
    return;
  }
  if (!g_log) return;
  fputs(line, g_log);
  fputc('\n', g_log);
  fflush(g_log);
  g_bytes_written += (long long)strlen(line) + 1;
  if (g_bytes_written >= k_log_max_bytes) rotate_locked();
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
  // serialize with log_line's writes and with rotation (which renames
  // the file out from under a concurrent reader). Try-lock: if the file
  // mutex is stuck (freeze), return empty instead of hanging /api/logs.
  std::unique_lock<std::timed_mutex> lk(g_file_mu, std::defer_lock);
  if (!lk.try_lock_for(std::chrono::seconds(5))) return 0;
  const std::string path = g_local_state + "\\\\tiramisu.log";
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return 0;
  _fseeki64(f, 0, SEEK_END);
  long long sz = _ftelli64(f);
  if (sz < 0) sz = 0;
  long long off = 0;
  if ((size_t)sz > cap - 1) off = sz - (long long)(cap - 1);  // tail only
  _fseeki64(f, off, SEEK_SET);
  size_t n = fread(out, 1, (size_t)(sz - off), f);
  out[n] = 0;
  fclose(f);
  return n;
}

void log_close() {
  std::lock_guard<std::timed_mutex> lk(g_file_mu);
  if (g_log) fclose(g_log);
  g_log = nullptr;
  g_bytes_written = 0;
  trt_trace_close();
}

} // namespace longan
