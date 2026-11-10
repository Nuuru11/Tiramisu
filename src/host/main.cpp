// tiramisu/src/host/main.cpp
// Tiramisu host — the UWP "Game" app for Xbox Series Dev Mode that:
//   * boots the trt runtime and executes Windows PE64s in-process
//   * serves the phone transfer page (QR on TV -> upload game zips -> launch)
//   * feeds guests keyboard/mouse (CoreWindow events) + controller (XInput),
//     including gamepad-as-mouse for KBM-only games
//   * optional Steam QR login (own account; depot pipeline is M6)
#include "host_log.h"
#include "swapchain.h"
#include "ui.h"
#include "webxfer.h"
#include "../trt/trt.h"
#include "../trt/input.h"

#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Devices.Input.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.Web.Http.h>

#include <d2d1_1.h>
#include <dwrite.h>
#include <d3d11.h>
#include <xinput.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

using namespace winrt;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::UI::Core;

namespace {

std::string g_local_state;
std::string g_games_dir;
std::string g_guest_status = "idle";
std::string g_steam_status = "press Y to link Steam (your own account)";
std::string g_steam_qr;  // URL to render as QR (steam or transfer)
std::atomic<bool> g_steam_busy{false};
std::atomic<bool> g_shutdown{false};
std::string g_pending_launch;
bool g_mouse_mode = false;
bool g_booted = false;

// ---------------- helpers ----------------
std::string ws2u8(winrt::hstring const& h) {
  const std::wstring w(h.c_str());
  std::string s;
  for (wchar_t c : w) s.push_back(c >= 0x20 && c < 0x7F ? (char)c : '?');
  return s;
}

std::string ws2u8(const std::wstring& w) {
  std::string s;
  for (wchar_t c : w) s.push_back(c >= 0x20 && c < 0x7F ? (char)c : '?');
  return s;
}

std::string str2u8(const std::string& s) { return s; }
std::wstring u82ws(const std::string& s) { return std::wstring(s.begin(), s.end()); }

std::string package_install_path() {
  try {
    return ws2u8(winrt::Windows::ApplicationModel::Package::Current()
                     .InstalledLocation()
                     .Path());
  } catch (...) {
    return {};
  }
}

std::string lan_ip() {
  using namespace winrt::Windows::Networking::Connectivity;
  try {
    const auto hosts = NetworkInformation::GetHostNames();
    for (unsigned i = 0; i < hosts.Size(); ++i) {
      const auto h = hosts.GetAt(i);
      if (h.Type() == winrt::Windows::Networking::HostNameType::Ipv4) {
        const std::string ip = ws2u8(h.DisplayName());
        if (ip.rfind("10.", 0) == 0 || ip.rfind("192.168.", 0) == 0 ||
            ip.rfind("172.", 0) == 0)
          return ip;
      }
    }
    if (hosts.Size() > 0) return ws2u8(hosts.GetAt(0).DisplayName());
  } catch (...) {
  }
  return "?";
}

// ---------------- UWP memory backend (W^X: no RWX, ever) ----------------
void* uwp_alloc(size_t bytes, u64 preferred_base) {
  (void)preferred_base;  // AppContainer cannot honor fixed bases
  return VirtualAllocFromApp(nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                             PAGE_READWRITE);
}
int uwp_protect(void* base, size_t bytes, unsigned prot) {
  DWORD fl = PAGE_READONLY;
  if (prot & TRT_PROT_X) fl = PAGE_EXECUTE_READ;
  else if (prot & TRT_PROT_W) fl = PAGE_READWRITE;
  DWORD old = 0;
  return VirtualProtectFromApp(base, bytes, fl, &old) ? 0 : 1;
}
void uwp_release(void* base, size_t) { VirtualFree(base, 0, MEM_RELEASE); }
void uwp_log(const char* line) { longan::log_line(line); }

// host pump the guest's GetMessageW calls: dispatcher + pad, nothing else
void host_pump() {
  CoreWindow::GetForCurrentThread()
      .Dispatcher()
      .ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
  trt_input_frame_begin();
  XINPUT_STATE xs{};
  if (XInputGetState(0, &xs) == ERROR_SUCCESS)
    trt_input_pad_state(xs.Gamepad.sThumbLX, xs.Gamepad.sThumbLY,
                        xs.Gamepad.sThumbRX, xs.Gamepad.sThumbRY,
                        xs.Gamepad.wButtons, xs.Gamepad.bLeftTrigger,
                        xs.Gamepad.bRightTrigger);
}

// ---------------- system-route binder (console real DLLs) ----------------
void* system_bind(const char* module, const char* symbol) {
  static wchar_t last_mod[64] = L"";
  static HMODULE h = nullptr;
  wchar_t wmod[64];
  size_t n = 0;
  while (module[n] && n < 62) { wmod[n] = (wchar_t)module[n]; ++n; }
  wmod[n] = 0;
  if (!h || wcscmp(last_mod, wmod) != 0) {
    h = GetModuleHandleW(wmod);
    if (!h) h = LoadLibraryW(wmod);
    wcscpy(last_mod, wmod);
  }
  return h ? (void*)GetProcAddress(h, symbol) : nullptr;
}

// ---------------- smoke probes ----------------
void run_probes() {
  char line[192];
  MEMORYSTATUSEX ms{sizeof(MEMORYSTATUSEX)};
  if (GlobalMemoryStatusEx(&ms)) {
    snprintf(line, sizeof(line), "probe: memory avail=%llu MB / %llu MB",
             (unsigned long long)(ms.ullAvailPhys / (1024 * 1024)),
             (unsigned long long)(ms.ullTotalPhys / (1024 * 1024)));
    longan::log_line(line);
  }
  HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
  auto create_proc =
      (decltype(&CreateProcessW))GetProcAddress(k32, "CreateProcessW");
  if (create_proc) {
    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    const BOOL ok = create_proc(L"notepad.exe", nullptr, nullptr, nullptr,
                                FALSE, 0, nullptr, nullptr, &si, &pi);
    snprintf(line, sizeof(line), "probe: CreateProcessW -> %s",
             ok ? "ALLOWED (unexpected!)" : "BLOCKED (expected)");
    longan::log_line(line);
  }
  DWORD pads = 0;
  for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
    XINPUT_STATE st{};
    if (XInputGetState(i, &st) == ERROR_SUCCESS) ++pads;
  }
  snprintf(line, sizeof(line), "probe: gamepads=%lu", (unsigned long)pads);
  longan::log_line(line);
}

// ---------------- game shelf ----------------
struct game_entry {
  std::string name;
  std::string exe;
};
std::vector<game_entry> g_games;
int g_selected = 0;

void rescan_games() {
  g_games.clear();
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA((g_games_dir + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    const std::string n = fd.cFileName;
    if (n == "." || n == "..") continue;
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
    WIN32_FIND_DATAA fx;
    HANDLE hx =
        FindFirstFileA((g_games_dir + "\\" + n + "\\*.exe").c_str(), &fx);
    if (hx == INVALID_HANDLE_VALUE) continue;
    std::string best;
    u64 best_sz = 0;
    do {
      const std::string e = fx.cFileName;
      const u64 sz = ((u64)fx.nFileSizeHigh << 32) | fx.nFileSizeLow;
      const size_t dot = e.rfind('.');
      const std::string stem = dot == std::string::npos ? e : e.substr(0, dot);
      if (_stricmp(stem.c_str(), n.c_str()) == 0) { best = e; break; }
      if (sz > best_sz) { best = e; best_sz = sz; }
    } while (FindNextFileA(hx, &fx));
    FindClose(hx);
    if (!best.empty()) g_games.push_back({n, best});
  } while (FindNextFileA(h, &fd));
  FindClose(h);
  if (g_selected >= (int)g_games.size()) g_selected = 0;
}

// ---------------- guest launch ----------------
struct guest_run_result {
  int map_rc;
  char detail[256];
};

guest_run_result run_game(const std::string& name, const std::string& exe,
                          const std::string& game_dir) {
  guest_run_result r{};
  char line[256];

  trt_backend be{&uwp_alloc, &uwp_protect, &uwp_release, &uwp_log};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();
  trt_module_register("gdi32.dll", TRT_ROUTE_STUB, nullptr, 0);
  trt_module_register("winmm.dll", TRT_ROUTE_STUB, nullptr, 0);
  trt_module_register("d3d9.dll", TRT_ROUTE_STUB, nullptr, 0);
  trt_module_register("ole32.dll", TRT_ROUTE_SYSTEM, nullptr, 0);
  trt_module_register("d3d11.dll", TRT_ROUTE_SYSTEM, nullptr, 0);
  trt_module_register("dxgi.dll", TRT_ROUTE_SYSTEM, nullptr, 0);
  trt_module_register("ws2_32.dll", TRT_ROUTE_SYSTEM, nullptr, 0);
  trt_module_register("bcrypt.dll", TRT_ROUTE_SYSTEM, nullptr, 0);
  trt_set_system_loader(&system_bind);
  trt_set_pump(&host_pump);  // guests that pump keep CoreWindow/XInput alive

  const std::string drive_c = g_local_state + "\\drive_c";
  trt_vfs_mount("c:", drive_c.c_str());
  trt_vfs_mount("z:", game_dir.c_str());
  trt_bridge_set_exe_path(("Z:\\" + exe).c_str());

  trt_resolver rs;
  trt_resolver_default(&rs);

  const std::string exe_path = game_dir + "\\" + exe;
  std::ifstream f(exe_path, std::ios::binary);
  if (!f) {
    snprintf(r.detail, sizeof(r.detail), "cannot open %s", exe_path.c_str());
    return r;
  }
  std::vector<u8> buf((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());

  snprintf(line, sizeof(line), "trt: launching %s (%zu bytes)", exe_path.c_str(),
           buf.size());
  longan::log_line(line);

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
  for (int i = 0; i < img->tls_callback_count; ++i) {
    using tls_cb = void (*)(void*, u32, void*);
    ((tls_cb)(img->base + img->tls_callback_rvas[i]))(img->base, 1, nullptr);
  }
  using entry_t = u32 (*)(u64, u64, u64);
  // NOTE for games: this call runs the guest's main; games pump via the
  // builtin user32 GetMessageW which yields to the host Sleep between events.
  ((entry_t)(img->base + img->entry_rva))(0, 0, 0);
  strncpy(r.detail, "guest returned", sizeof(r.detail) - 1);
  return r;
}

// ---------------- Steam QR login (own account) ----------------
std::wstring uri_escape(const std::wstring& s) {
  try {
    return std::wstring(
        winrt::Windows::Foundation::Uri::EscapeComponent(s).c_str());
  } catch (...) {
    return s;
  }
}

void steam_begin() {
  if (g_steam_busy.exchange(true)) return;
  g_steam_status = "steam: requesting QR…";
  std::thread([] {
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
      using namespace winrt::Windows::Web::Http;
      using namespace winrt::Windows::Data::Json;
      HttpClient http;
      // form body assembled by hand: input_protobuf_encoded=false + escaped
      // input JSON (the WebAPI's JSON mode)
      auto post_form = [&http](const wchar_t* endpoint,
                               const std::wstring& input_json)
          -> winrt::Windows::Foundation::IAsyncOperationWithProgress<
              HttpResponseMessage, HttpProgress> {
        const std::wstring body =
            L"input_protobuf_encoded=false&input=" + uri_escape(input_json);
        HttpStringContent content(body,
                                  Windows::Storage::Streams::UnicodeEncoding::Utf8,
                                  L"application/x-www-form-urlencoded");
        return http.PostAsync(
            winrt::Windows::Foundation::Uri(endpoint), content);
      };
      const std::wstring begin_json =
          L"{\"client_id\":\"2654200164598180\",\"device_id\":"
          L"\"tiramisu-xbox\",\"refresh_token\":\"\"}";
      auto resp = post_form(
          L"https://api.steampowered.com/IAuthenticationService/v1/"
          L"BeginAuthSessionViaQR",
          begin_json)
                      .get();
      const std::string text = ws2u8(resp.Content().ReadAsStringAsync().get());
      auto j = JsonObject::Parse(winrt::to_hstring(text));
      auto r2 = j.GetNamedObject(L"response");
      g_steam_qr = str2u8(ws2u8(r2.GetNamedString(L"challenge_url")));
      const std::string client_id =
          str2u8(ws2u8(r2.GetNamedString(L"client_id")));
      const std::string request_id =
          str2u8(ws2u8(r2.GetNamedString(L"request_id")));
      g_steam_status = "steam: scan the QR with the Steam app";
      for (int i = 0; i < 36 && !g_shutdown; ++i) {
        Sleep(5000);
        auto pr =
            post_form(L"https://api.steampowered.com/IAuthenticationService/"
                      L"v1/PollAuthSessionStatus",
                      L"{\"client_id\":\"" + u82ws(client_id) +
                          L"\",\"request_id\":\"" + u82ws(request_id) +
                          L"\"}")
                .get();
        const std::string pt = ws2u8(pr.Content().ReadAsStringAsync().get());
        auto pj = JsonObject::Parse(winrt::to_hstring(pt));
        auto pr2 = pj.GetNamedObject(L"response");
        if (pr2.HasKey(L"refresh_token")) {
          const std::string token =
              str2u8(ws2u8(pr2.GetNamedString(L"refresh_token")));
          FILE* tf = fopen((g_local_state + "\\steam.token").c_str(), "wb");
          if (tf) {
            fwrite(token.data(), 1, token.size(), tf);
            fclose(tf);
          }
          g_steam_status =
              "steam: LINKED — depot DL is M6; play via file transfer now";
          g_steam_qr.clear();
          break;
        }
      }
      if (g_steam_qr.size()) {
        g_steam_qr.clear();
        g_steam_status = "steam: QR expired — press Y to retry";
      }
    } catch (hresult_error const& e) {
      g_steam_status = "steam error: " + ws2u8(e.message());
    } catch (...) {
      g_steam_status = "steam error: network/parse (endpoint may need protobuf)";
    }
    g_steam_busy = false;
  }).detach();
}

// ---------------- the app ----------------
struct tiramisu_host
    : implements<tiramisu_host, IFrameworkViewSource, IFrameworkView> {
  CoreWindow m_window{nullptr};
  unsigned m_frames = 0;
  unsigned m_rescan_at = 0;

  IFrameworkView CreateView() { return *this; }
  void Initialize(CoreApplicationView const&) {}
  void Load(hstring const&) {}
  void Uninitialize() {}

  void SetWindow(CoreWindow const& window) {
    m_window = window;
    const auto sc = longan::swapchain_create(window);
    char line[256];
    snprintf(line, sizeof(line),
             "probe: d3d11 hr=0x%08lx fl=%u swapchain hr=0x%08lx %ux%u flip",
             (unsigned long)sc.device_hr, sc.feature_level,
             (unsigned long)sc.sc_hr, sc.width, sc.height);
    longan::log_line(line);

    ID2D1Factory1* d2df = nullptr;
    D2D1_FACTORY_OPTIONS fo{};
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                      __uuidof(ID2D1Factory1), &fo, (void**)&d2df);
    IDWriteFactory* dwf = nullptr;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        (IUnknown**)&dwf);
    void* d2d_device_v = nullptr;
    ID3D11Device1* d3d = (ID3D11Device1*)longan::swapchain_d3d();
    if (d3d) {
      IDXGIDevice* dxgidev = nullptr;
      d3d->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgidev);
      if (dxgidev) {
        ID2D1Device* d2ddev = nullptr;
        d2df->CreateDevice(dxgidev, &d2ddev);
        d2d_device_v = d2ddev;
        dxgidev->Release();
      }
    }
    longan::ui_init(sc.width, sc.height, d2df, dwf, d2d_device_v);

    m_kd = window.KeyDown([](CoreWindow const&, KeyEventArgs const& a) {
      const auto s = a.KeyStatus();
      trt_input_key_event2((u16)a.VirtualKey(), true, (u16)s.ScanCode,
                           s.IsExtendedKey ? 1 : 0, s.WasKeyDown ? 1 : 0);
    });
    m_ku = window.KeyUp([](CoreWindow const&, KeyEventArgs const& a) {
      const auto s = a.KeyStatus();
      trt_input_key_event2((u16)a.VirtualKey(), false, (u16)s.ScanCode,
                           s.IsExtendedKey ? 1 : 0, 0);
    });
    m_cr = window.CharacterReceived(
        [](CoreWindow const&, CharacterReceivedEventArgs const& a) {
          trt_input_char_event((u32)a.KeyCode());
        });
    m_pp = window.PointerPressed([](CoreWindow const&,
                                    PointerEventArgs const& a) {
      const auto k = a.CurrentPoint().Properties().PointerUpdateKind();
      trt_input_mouse_button(k == winrt::Windows::UI::Input::PointerUpdateKind::RightButtonPressed
                                 ? 1
                                 : k == winrt::Windows::UI::Input::PointerUpdateKind::MiddleButtonPressed
                                       ? 2
                                       : 0,
                             true);
    });
    m_pr = window.PointerReleased([](CoreWindow const&,
                                     PointerEventArgs const& a) {
      const auto k = a.CurrentPoint().Properties().PointerUpdateKind();
      trt_input_mouse_button(k == winrt::Windows::UI::Input::PointerUpdateKind::RightButtonReleased
                                 ? 1
                                 : k == winrt::Windows::UI::Input::PointerUpdateKind::MiddleButtonReleased
                                       ? 2
                                       : 0,
                             false);
    });
    m_pm = window.PointerMoved([](CoreWindow const&,
                                  PointerEventArgs const& a) {
      const auto p = a.CurrentPoint().Position();
      trt_input_mouse_move((int)p.X, (int)p.Y);
    });
    m_pw = window.PointerWheelChanged([](CoreWindow const&,
                                         PointerEventArgs const& a) {
      trt_input_wheel(
          (i32)a.CurrentPoint().Properties().MouseWheelDelta());
    });

    // REAL relative mouse deltas — the Xbox UWP mouse API. The virtual
    // cursor clamps at screen edges; this stream does not, which is what
    // FPS/mouse-look games need. Falls back gracefully to pointer-diff.
    namespace wdi = winrt::Windows::Devices::Input;
    m_mm = wdi::MouseDevice::GetForCurrentView().MouseMoved(
        [](wdi::MouseDevice const&, wdi::MouseEventArgs const& a) {
          const auto d = a.MouseDelta();
          trt_input_mouse_delta_raw((i32)d.X, (i32)d.Y);
        });
  }

  void boot_all() {
    const std::string install = package_install_path();
    g_local_state =
        ws2u8(winrt::Windows::Storage::ApplicationData::Current()
                  .LocalFolder()
                  .Path());
    g_games_dir = g_local_state + "\\games";
    longan::log_open(install);
    char line[256];
    snprintf(line, sizeof(line), "tiramisu boot: pkg=%s state=%s",
             install.c_str(), g_local_state.c_str());
    longan::log_line(line);
    run_probes();
    const std::string url = longan::webxfer_start(g_local_state, 1920);
    snprintf(line, sizeof(line), "transfer: %s (QR on TV)", url.c_str());
    longan::log_line(line);
    rescan_games();

    // builtin self-test: proves the whole pipeline on hardware
    const auto gr = run_game("selftest", "guest_hello.exe", package_install_path());
    snprintf(line, sizeof(line), "selftest: rc=%s (%s)", trt_errstr(gr.map_rc),
             gr.detail);
    longan::log_line(line);
    g_guest_status = std::string("selftest ") + trt_errstr(gr.map_rc);
  }

  void Run() {
    CoreWindow::GetForCurrentThread().Activate();
    if (!g_booted) { g_booted = true; boot_all(); }

    int mouse_vx = 960, mouse_vy = 540;
    while (true) {
      CoreWindow::GetForCurrentThread()
          .Dispatcher()
          .ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
      trt_input_frame_begin();

      // ---- controller ----
      XINPUT_STATE xs{};
      if (XInputGetState(0, &xs) == ERROR_SUCCESS) {
        trt_input_pad_state(xs.Gamepad.sThumbLX, xs.Gamepad.sThumbLY,
                            xs.Gamepad.sThumbRX, xs.Gamepad.sThumbRY,
                            xs.Gamepad.wButtons, xs.Gamepad.bLeftTrigger,
                            xs.Gamepad.bRightTrigger);
        static bool d_up = false, d_dn = false, a_btn = false, y_btn = false,
                    rs_btn = false;
        const u16 b = xs.Gamepad.wButtons;
        if ((b & XINPUT_GAMEPAD_DPAD_UP) && !d_up) {
          d_up = true;
          if (g_selected > 0) --g_selected;
        } else if (!(b & XINPUT_GAMEPAD_DPAD_UP)) d_up = false;
        if ((b & XINPUT_GAMEPAD_DPAD_DOWN) && !d_dn) {
          d_dn = true;
          if (g_selected + 1 < (int)g_games.size()) ++g_selected;
        } else if (!(b & XINPUT_GAMEPAD_DPAD_DOWN)) d_dn = false;
        if ((b & XINPUT_GAMEPAD_A) && !a_btn) {
          a_btn = true;
          if (!g_games.empty()) g_pending_launch = g_games[g_selected].name;
        } else if (!(b & XINPUT_GAMEPAD_A)) a_btn = false;
        if ((b & XINPUT_GAMEPAD_Y) && !y_btn) {
          y_btn = true;
          steam_begin();
        } else if (!(b & XINPUT_GAMEPAD_Y)) y_btn = false;
        if ((b & XINPUT_GAMEPAD_RIGHT_THUMB) && !rs_btn) {
          rs_btn = true;
          g_mouse_mode = !g_mouse_mode;
        } else if (!(b & XINPUT_GAMEPAD_RIGHT_THUMB)) rs_btn = false;

        // gamepad-as-mouse (KBM games without a real mouse)
        if (g_mouse_mode) {
          const int dz = 8000;
          int sx = xs.Gamepad.sThumbRX;
          int sy = -xs.Gamepad.sThumbRY;
          sx = sx > dz ? sx - dz : sx < -dz ? sx + dz : 0;
          sy = sy > dz ? sy - dz : sy < -dz ? sy + dz : 0;
          mouse_vx += (int)(sx / 32767.0f * 1400.0f / 60.0f);
          mouse_vy += (int)(sy / 32767.0f * 1400.0f / 60.0f);
          if (mouse_vx < 0) mouse_vx = 0;
          if (mouse_vx > 1919) mouse_vx = 1919;
          if (mouse_vy < 0) mouse_vy = 0;
          if (mouse_vy > 1079) mouse_vy = 1079;
          trt_input_mouse_move(mouse_vx, mouse_vy);
          // triggers = mouse buttons (byte thresholds, not button flags)
          static bool was_rt = false, was_lt = false;
          const bool rt = xs.Gamepad.bRightTrigger >
                          XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
          const bool lt = xs.Gamepad.bLeftTrigger >
                          XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
          if (rt != was_rt) trt_input_mouse_button(0, rt);
          if (lt != was_lt) trt_input_mouse_button(1, lt);
          was_rt = rt;
          was_lt = lt;
        }
      } else {
        trt_input_pad_state(0, 0, 0, 0, 0, 0, 0);
      }

      // ---- launch handoff (web + pad) ----
      std::string webname;
      if (longan::webxfer_poll_launch(&webname)) g_pending_launch = webname;
      if (!g_pending_launch.empty()) {
        const std::string want = g_pending_launch;
        g_pending_launch.clear();
        for (const auto& ge : g_games)
          if (ge.name == want) {
            g_guest_status = "running " + ge.name + "…";
            const auto gr = run_game(ge.name, ge.exe, g_games_dir + "\\" + ge.name);
            char line[192];
            snprintf(line, sizeof(line), "launch %s: rc=%s (%s)",
                     ge.name.c_str(), trt_errstr(gr.map_rc), gr.detail);
            longan::log_line(line);
            g_guest_status = std::string(want) + ": " + trt_errstr(gr.map_rc);
            break;
          }
      }

      // ---- UI ----
      if (m_frames - m_rescan_at >= 120) {
        m_rescan_at = m_frames;
        rescan_games();
      }
      std::vector<longan::ui_game> uig;
      for (const auto& ge : g_games)
        uig.push_back({u82ws(ge.name), u82ws(ge.exe)});
      longan::ui_set_games(uig, g_selected);
      std::vector<std::wstring> lines;
      lines.push_back(u82ws(longan::webxfer_status_line()));
      lines.push_back(u82ws("guest: " + g_guest_status));
      lines.push_back(u82ws(g_steam_status +
                            (g_mouse_mode ? "  |  MOUSE MODE" : "")));
      lines.push_back(L"D-pad: select   A: launch   Y: steam   RS: mouse mode");
      longan::ui_set_lines(lines);
      longan::ui_set_qr(g_steam_busy ? g_steam_qr : longan::webxfer_url());
      longan::ui_render();

      longan::swapchain_render_frame((m_frames % 180) / 180.0f);
      longan::swapchain_present();
      ++m_frames;
    }
  }

  winrt::event_token m_kd{}, m_ku{}, m_cr{}, m_pp{}, m_pr{}, m_pm{}, m_pw{},
      m_mm{};
};

} // namespace

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  init_apartment(apartment_type::multi_threaded);
  CoreApplication::Run(make<tiramisu_host>());
  return 0;
}
