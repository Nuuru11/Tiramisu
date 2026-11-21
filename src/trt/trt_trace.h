// tiramisu/src/trt/trt_trace.h
// Allocation-free diagnostic trace writer (Celeste hang tracing).
// Writes to tiramisu.log via a pre-opened FILE* + its own mutex.
// NO heap allocation in the trace path (no std::string, no fopen per
// call, no ring buffer) -- it must keep working when the CRT heap lock
// is held (H1) or g_file_mu is stuck (H2).
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void trt_trace_init(const char* log_path);
void trt_trace(const char* line);
void trt_trace_close(void);
#ifdef __cplusplus
}
#endif
