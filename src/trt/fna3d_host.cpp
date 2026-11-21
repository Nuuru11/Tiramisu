// tiramisu/src/trt/fna3d_host.cpp
// Host-side handoff of Tiramisu's D3D11 device + CoreWindow swapchain to
// FNA3D. See fna3d_host.h for the ownership contract. The Windows/UWP
// path below is the real handoff (FNA3D_D3D11_SetHostDevice, compiled
// into the host from third_party/fna3d-src); the Linux path is an
// honest "not available" (no D3D11 on Linux).
#include "fna3d_host.h"

#include <cstdio>
#include <cstring>

#include "bridge_d3d.h"

namespace {

void (*g_log)(const char* line) = nullptr;
char g_last_error[512] = "";
int g_attached = 0;

void fna3d_log(const char* line) {
  if (g_log) {
    g_log(line);
  } else {
    std::fprintf(stderr, "fna3d: %s\n", line);
  }
}

int fail(const char* detail) {
  std::snprintf(g_last_error, sizeof(g_last_error), "%s", detail);
  char line[600];
  std::snprintf(line, sizeof(line), "FAIL %s", detail);
  fna3d_log(line);
  return TRT_ERR_FNA3D;
}

}  // namespace

extern "C" const char* trt_fna3d_last_error(void) { return g_last_error; }

extern "C" int trt_fna3d_is_attached(void) { return g_attached; }

#if defined(_WIN32)
// ---- Windows/UWP: real handoff into the FNA3D D3D11 driver ----
#include "FNA3D.h"
#include <winrt/base.h>  // winrt::hresult_error
#include <winrt/Windows.System.h>  // MemoryManager::AppMemoryUsage (patch 08)

namespace {

// FNA3D log routing: installed once on attach (when the caller supplied
// a log callback), for the process lifetime. FNA3D has no unhook API,
// so detach deliberately leaves these installed; the callback must
// stay valid (the host's logger is a process-lifetime function).
void fna3d_log_info(const char* msg) {
  char line[1100];
  std::snprintf(line, sizeof(line), "FNA3D info: %s", msg);
  fna3d_log(line);
}

void fna3d_log_warn(const char* msg) {
  char line[1100];
  std::snprintf(line, sizeof(line), "FNA3D warn: %s", msg);
  fna3d_log(line);
}

void fna3d_log_error(const char* msg) {
  char line[1100];
  std::snprintf(line, sizeof(line), "FNA3D error: %s", msg);
  fna3d_log(line);
}

}  // namespace

extern "C" int trt_fna3d_attach(const trt_fna3d_attach_config* cfg) {
  g_last_error[0] = 0;
  if (g_attached) return fail("already attached (detach first)");
  if (!cfg || !cfg->device || !cfg->swapchain)
    return fail("trt_fna3d_attach: device and swapchain are required");
  if (cfg->log) {
    g_log = cfg->log;
    FNA3D_HookLogFunctions(fna3d_log_info, fna3d_log_warn, fna3d_log_error);
  }
  // Pre-register: the next FNA3D_CreateDevice consumes this one-shot
  // registration and adopts the host objects (AddRef; released, never
  // destroyed, in FNA3D_DestroyDevice).
  if (!FNA3D_D3D11_SetHostDevice(cfg->device, cfg->context, cfg->swapchain))
    return fail("FNA3D_D3D11_SetHostDevice rejected the host device "
                "(no D3D11 driver in this build?)");
  g_attached = 1;
  fna3d_log("attached: host D3D11 device + swapchain handed to FNA3D");
  return TRT_OK;
}

extern "C" void trt_fna3d_detach(void) {
  // Clears a PENDING registration; if a device already consumed it,
  // this is a no-op and the adopted references are released by
  // FNA3D_DestroyDevice instead. Never touches the host objects.
  FNA3D_D3D11_ClearHostDevice();
  if (g_attached) {
    g_attached = 0;
    fna3d_log("detached");
  }
}

extern "C" int trt_fna3d_attach_from_hooks(void (*log)(const char* line)) {
  const trt_gpu_hooks* hooks = trt_gpu_hooks_get();
  if (!hooks)
    return fail("no GPU hooks published (host swapchain not created?)");
  if (!hooks->device || !hooks->swapchain)
    return fail("GPU hooks lack device/swapchain");
  trt_fna3d_attach_config cfg = {};
  cfg.device = hooks->device;
  cfg.context = hooks->context;
  cfg.swapchain = hooks->swapchain;
  cfg.log = log;
  return trt_fna3d_attach(&cfg);
}

extern "C" u64 trt_fna3d_app_memory_usage(void) {
  // Memory instrumentation for the Celeste OOM investigation
  // (oom-analysis.md): the FNA3D D3D11 driver logs this after each
  // large texture upload (patch 08). Returns 0 if the query fails.
  try {
    using winrt::Windows::System::MemoryManager;
    return (u64)MemoryManager::AppMemoryUsage();
  } catch (const winrt::hresult_error&) {
    return 0;
  }
}

#else
// ---- Linux: no D3D11. Honest failure, not a fake. ----
extern "C" int trt_fna3d_attach(const trt_fna3d_attach_config* cfg) {
  (void)cfg;
  return fail("FNA3D D3D11 handoff is not available on Linux (no D3D11; "
              "the offline harness renders via FNA3D/OpenGL)");
}

extern "C" void trt_fna3d_detach(void) {
  // Nothing was ever attached; nothing to clear.
}

extern "C" int trt_fna3d_attach_from_hooks(void (*log)(const char* line)) {
  (void)log;
  return fail("FNA3D D3D11 handoff is not available on Linux (no D3D11; "
              "the offline harness renders via FNA3D/OpenGL)");
}

extern "C" u64 trt_fna3d_app_memory_usage(void) {
  // No WinRT on Linux; the D3D11 driver is not built for Linux either.
  return 0;
}

#endif  // _WIN32
