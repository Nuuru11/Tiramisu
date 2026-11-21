// tiramisu/src/trt/trt_trace.cpp
// Allocation-free diagnostic trace writer -- implementation.
// Portable (no WinRT / Windows-only APIs): compiled into libtrt.a (the
// gate globs src/trt/*.cpp) and into the UWP host (build-host.sh lists
// this file explicitly). See trt_trace.h for the contract.
#include "trt_trace.h"

#include <cstdio>
#include <mutex>

// ---- Allocation-free trace writer (trt_trace) ----
// Own FILE* (opened once at log_open, warm-written so its stdio buffer
// is allocated before any freeze can occur) + own mutex. The trace path
// performs NO heap allocation: no std::string, no fopen/fclose per call,
// no ring buffer. It keeps writing to tiramisu.log even when the CRT
// heap lock is held (H1) or g_file_mu is stuck (H2). Writes go to the
// SAME tiramisu.log file (the only log channel readable via /api/logs).
namespace {
FILE* g_trace_log = nullptr;
std::mutex g_trace_mu;
}  // namespace

extern "C" void trt_trace_init(const char* log_path) {
  std::lock_guard<std::mutex> lk(g_trace_mu);
  if (g_trace_log) fclose(g_trace_log);
  g_trace_log = nullptr;
  if (!log_path) return;
  g_trace_log = fopen(log_path, "a");
  if (g_trace_log) {
    // Warm-up write: forces the stdio buffer allocation NOW (before any
    // freeze), so later trt_trace calls allocate nothing.
    fputs("---- trace writer open ----\n", g_trace_log);
    fflush(g_trace_log);
  }
}

extern "C" void trt_trace(const char* line) {
  if (!line) return;
  std::lock_guard<std::mutex> lk(g_trace_mu);
  if (!g_trace_log) return;
  fputs(line, g_trace_log);
  fputc('\n', g_trace_log);
  fflush(g_trace_log);
}

extern "C" void trt_trace_close(void) {
  std::lock_guard<std::mutex> lk(g_trace_mu);
  if (g_trace_log) fclose(g_trace_log);
  g_trace_log = nullptr;
}

