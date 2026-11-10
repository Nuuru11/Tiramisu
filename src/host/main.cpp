// tiramisu/src/host/main.cpp
// Tiramisu host — the UWP "Game" app that boots the Tiramisu runtime and runs a
// Windows PE64 in-process on Xbox Series Dev Mode.
//
// Shape (proven console envelope): single-file C++/WinRT app,
// implements<IFrameworkViewSource, IFrameworkView>, CoreApplication::Run
// from wWinMain, flip-model D3D11 on the CoreWindow, XInput poll, LocalState
// logging (no ETW on console). The guest executes on the SAME thread with
// the SAME Win64 ABI as the host — no trampolines, no processes, no RWX.
#include "host_log.h"
#include "swapchain.h"
#include "../trt/trt.h"

#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Core.h>

#include <d3d11.h>
#include <xinput.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "xinput.lib")

using namespace winrt;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::UI::Core;

namespace {

std::string wstr_to_u8(winrt::hstring const& h) {
  std::string s;
  for (wchar_t c : h) s.push_back(c >= 0x20 && c < 0x7F ? (char)c : '?');
  return s;
}

std::string package_install_path() {
  try {
    return wstr_to_u8(
        winrt::Windows::ApplicationModel::Package::Current()
            .InstalledLocation().Path());
  } catch (...) {
    return {};
  }
}

// ---------------- UWP memory backend (W^X: RW alloc, protect flips) ----------
void* uwp_alloc(size_t bytes, u64 preferred_base) {
  (void)preferred_base;  // AppContainer cannot honor fixed bases
  return VirtualAllocFromApp(nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                             PAGE_READWRITE);
}
int uwp_protect(void* base, size_t bytes, unsigned prot) {
  DWORD fl = 0;
  if ((prot & TRT_PROT_X)) fl = PAGE_EXECUTE_READ;       // no W anywhere near X
  else if ((prot & TRT_PROT_W)) fl = PAGE_READWRITE;
  else fl = PAGE_READONLY;
  DWORD old = 0;
  return VirtualProtectFromApp(base, bytes, fl, &old) ? 0 : 1;
}
void uwp_release(void* base, size_t) { VirtualFree(base, 0, MEM_RELEASE); }
void uwp_log(const char* line) { tiramisu::log_line(line); }

// ---------------- system-route binder (console real DLLs) --------------------
// d3d11/dxgi/ws2_32/bcrypt/kernelbase ship in the console OS; bind straight
// to them. This is the load-bearing shortcut xboxnative proved on hardware.
void* system_bind(const char* module, const char* symbol) {
  static wchar_t last_mod[64] = L"";
  static HMODULE h = nullptr;
  wchar_t wmod[64];
  size_t n = 0;
  while (module[n] && n < 62) {
    wmod[n] = (wchar_t)module[n];
    ++n;
  }
  wmod[n] = 0;
  if (h == nullptr || wcscmp(last_mod, wmod) != 0) {
    h = GetModuleHandleW(wmod);
    if (!h) h = LoadLibraryW(wmod);
    wcscpy(last_mod, wmod);
  }
  return h ? (void*)GetProcAddress(h, symbol) : nullptr;
}

// ---------------- smoke probe set (skill: one TV look answers everything) ---
void run_probes() {
  char line[192];

  // memory: games get 5 GB as a Game package, 1 GB as an App
  MEMORYSTATUSEX ms{sizeof(MEMORYSTATUSEX)};
  if (GlobalMemoryStatusEx(&ms)) {
    snprintf(line, sizeof(line),
             "probe: memory avail=%llu MB total=%llu MB",
             (unsigned long long)(ms.ullAvailPhys / (1024 * 1024)),
             (unsigned long long)(ms.ullTotalPhys / (1024 * 1024)));
    tiramisu::log_line(line);
  }

  // CreateProcess: the sandbox must refuse it (architecture depends on this)
  HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
  auto create_proc = (decltype(&CreateProcessW))GetProcAddress(k32, "CreateProcessW");
  if (create_proc) {
    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    const BOOL ok = create_proc(L"notepad.exe", nullptr, nullptr, nullptr, FALSE,
                                0, nullptr, nullptr, &si, &pi);
    snprintf(line, sizeof(line), "probe: CreateProcessW -> %s (sandbox verdict)",
             ok ? "ALLOWED (unexpected!)" : "BLOCKED (expected)");
    tiramisu::log_line(line);
  } else {
    tiramisu::log_line("probe: CreateProcessW absent from kernel32 (expected in APP family)");
  }

  // gamepad
  DWORD pads = 0;
  for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
    XINPUT_STATE st{};
    if (XInputGetState(i, &st) == ERROR_SUCCESS) ++pads;
  }
  snprintf(line, sizeof(line), "probe: gamepads=%lu", (unsigned long)pads);
  tiramisu::log_line(line);
}

// ---------------- the runtime boot + guest run -------------------------------
struct guest_run_result {
  int map_rc;
  u32 entry_ret;
  u64 entry_rva;
  char detail[256];
};

guest_run_result run_guest(const std::string& install_path,
                           const std::string& local_state,
                           const char* exe_name) {
  guest_run_result r{};
  char line[256];

  // runtime singletons
  trt_backend be{&uwp_alloc, &uwp_protect, &uwp_release, &uwp_log};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_module_register("user32.dll", TRT_ROUTE_STUB, nullptr, 0);
  trt_module_register("gdi32.dll", TRT_ROUTE_STUB, nullptr, 0);
  trt_module_register("winmm.dll", TRT_ROUTE_STUB, nullptr, 0);
  trt_module_register("ole32.dll", TRT_ROUTE_SYSTEM, nullptr, 0);
  trt_module_register("d3d11.dll", TRT_ROUTE_SYSTEM, nullptr, 0);
  trt_module_register("dxgi.dll", TRT_ROUTE_SYSTEM, nullptr, 0);
  trt_module_register("ws2_32.dll", TRT_ROUTE_SYSTEM, nullptr, 0);
  trt_module_register("bcrypt.dll", TRT_ROUTE_SYSTEM, nullptr, 0);
  trt_set_system_loader(&system_bind);

  // VFS: C:\ -> LocalState/drive_c, Z:\ -> package (read-only game files)
  const std::string drive_c = local_state + "\\drive_c";
  trt_vfs_mount("c:", drive_c.c_str());
  trt_vfs_mount("z:", install_path.c_str());

  trt_resolver rs;
  trt_resolver_default(&rs);

  // read the guest exe (ships inside the package)
  const std::string exe_path = install_path + "\\" + exe_name;
  std::ifstream f(exe_path, std::ios::binary);
  if (!f) {
    snprintf(line, sizeof(line), "trt: cannot open guest %s", exe_path.c_str());
    tiramisu::log_line(line);
    r.map_rc = TRT_ERR_BAD_ARGS;
    snprintf(r.detail, sizeof(r.detail), "guest file not found");
    return r;
  }
  std::vector<u8> buf((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());

  trt_image* img = nullptr;
  r.map_rc = trt_map_pe(&be, &rs, buf.data(), buf.size(), &img);
  if (r.map_rc != TRT_OK) {
    strncpy(r.detail, trt_errstr(r.map_rc), sizeof(r.detail) - 1);
    return r;
  }
  if (trt_seal_image(&be, img) != TRT_OK) {
    strncpy(r.detail, "seal failed", sizeof(r.detail) - 1);
    return r;
  }

  // TLS callbacks (guest v0 has none, but run them for real images)
  for (int i = 0; i < img->tls_callback_count; ++i) {
    using tls_cb = void (__stdcall*)(void*, DWORD, void*);
    ((tls_cb)(img->base + img->tls_callback_rvas[i]))(img->base,
                                                      DLL_PROCESS_ATTACH,
                                                      nullptr);
  }

  // execute: SAME process, SAME Win64 ABI, RX pages, no trampolines
  using entry_t = u32 (*)(u64, u64, u64);
  entry_t entry = (entry_t)(img->base + img->entry_rva);
  r.entry_ret = entry(0, 0, 0);
  r.entry_rva = img->entry_rva;
  snprintf(line, sizeof(line), "trt: GUEST RETURNED 0x%08x", (u32)r.entry_ret);
  tiramisu::log_line(line);
  return r;
}

} // namespace

struct tiramisu_host : implements<tiramisu_host, IFrameworkViewSource, IFrameworkView> {
  CoreWindow m_window{nullptr};
  bool m_booted = false;
  unsigned m_frames = 0;

  IFrameworkView CreateView() { return *this; }

  void Initialize(CoreApplicationView const&) {}
  void Load(hstring const&) {}
  void Uninitialize() {}

  void SetWindow(CoreWindow const& window) {
    m_window = window;
    const auto sc = tiramisu::swapchain_create(window);
    char line[192];
    snprintf(line, sizeof(line),
             "probe: d3d11 device hr=0x%08lx feature_level=%u swapchain "
             "hr=0x%08lx %ux%u flip-sequential",
             (unsigned long)sc.device_hr, sc.feature_level,
             (unsigned long)sc.sc_hr, sc.width, sc.height);
    tiramisu::log_line(line);
  }

  void Run() {
    CoreWindow::GetForCurrentThread().Activate();

    // boot + execute the guest once, before the render loop
    if (!m_booted) {
      m_booted = true;
      const std::string install = package_install_path();
      const std::string local_state = tiramisu::log_open(install);
      char line[256];
      snprintf(line, sizeof(line),
               "tiramisu host boot: package=%s localstate=%s", install.c_str(),
               local_state.c_str());
      tiramisu::log_line(line);
      run_probes();
      const auto gr = run_guest(install, local_state, "guest_hello.exe");
      snprintf(line, sizeof(line),
               "probe: guest map_rc=%s entry_ret=0x%08x detail=%s",
               trt_errstr(gr.map_rc), (u32)gr.entry_ret, gr.detail);
      tiramisu::log_line(line);
    }

    while (true) {
      CoreWindow::GetForCurrentThread()
          .Dispatcher()
          .ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);

      XINPUT_STATE xs{};
      static bool edge = false;
      if (XInputGetState(0, &xs) == ERROR_SUCCESS &&
          (xs.Gamepad.wButtons & XINPUT_GAMEPAD_A)) {
        // button A: re-run the guest (one-shot per press)
        if (!edge) {
          edge = true;
          tiramisu::log_line("input: gamepad A — re-running guest");
          const std::string install = package_install_path();
          const std::string local_state =
              wstr_to_u8(winrt::Windows::Storage::ApplicationData::Current()
                             .LocalFolder()
                             .Path());
          const auto gr = run_guest(install, local_state, "guest_hello.exe");
          char line[160];
          snprintf(line, sizeof(line), "probe: guest rerun rc=%s ret=0x%08x",
                   trt_errstr(gr.map_rc), (u32)gr.entry_ret);
          tiramisu::log_line(line);
        }
      } else {
        edge = false;
      }

      tiramisu::swapchain_render_frame((m_frames % 180) / 180.0f);
      tiramisu::swapchain_present();
      ++m_frames;
    }
  }
};

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  init_apartment(apartment_type::multi_threaded);
  CoreApplication::Run(make<tiramisu_host>());
  return 0;
}
