// tiramisu/src/host/settings.cpp
#include "settings.h"

#include "../trt/trt.h"

#include <cstdio>
#include <mutex>
#include <string>

namespace longan {

namespace {

std::mutex g_mu;
host_settings g_live;

#ifdef _WIN32
const char k_sep = '\\';
#else
const char k_sep = '/';  // offline-test path (Linux)
#endif

int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (unsigned char)s[a] <= ' ') ++a;
  while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
  return s.substr(a, b - a);
}

} // namespace

std::string host_settings::to_ini() const {
  char buf[640];
  std::string s;
  s += "# Tiramisu host settings — edited from the dashboard (TV or web).\n";
  s += "# Missing keys keep their built-in defaults.\n";
  snprintf(buf, sizeof(buf), "default_input=%s\n", default_input.c_str());
  s += buf;
  snprintf(buf, sizeof(buf), "mouse_speed=%d\nwheel_speed=%d\n",
           mouse_speed, wheel_speed);
  s += buf;
  s += pad_vibration ? "pad_vibration=1\n" : "pad_vibration=0\n";
  snprintf(buf, sizeof(buf), "os_major=%d\nos_minor=%d\nos_build=%d\n",
           os_major, os_minor, os_build);
  s += buf;
  s += win11_personality ? "win11_personality=1\n" : "win11_personality=0\n";
  s += write_watch ? "write_watch=1\n" : "write_watch=0\n";
  s += steam_auth ? "steam_auth=1\n" : "steam_auth=0\n";
  s += show_qr ? "show_qr=1\n" : "show_qr=0\n";
  snprintf(buf, sizeof(buf), "log_lines=%d\n", log_lines);
  s += buf;
  return s;
}

int host_settings::apply_ini(const std::string& text) {
  int applied = 0;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t eol = text.find('\n', pos);
    if (eol == std::string::npos) eol = text.size();
    std::string line = trim(text.substr(pos, eol - pos));
    pos = eol + 1;
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = trim(line.substr(0, eq));
    const std::string val = trim(line.substr(eq + 1));
    if (key.empty()) continue;
    ++applied;
    if (key == "default_input") {
      if (val == "auto" || val == "controller" || val == "kbm" ||
          val == "pad2kbm")
        default_input = val;
    } else if (key == "mouse_speed") {
      mouse_speed = clampi(atoi(val.c_str()), 1, 100);
    } else if (key == "wheel_speed") {
      wheel_speed = clampi(atoi(val.c_str()), 1, 100);
    } else if (key == "pad_vibration") {
      pad_vibration = (val == "1" || val == "true");
    } else if (key == "os_major") {
      os_major = clampi(atoi(val.c_str()), 5, 12);
    } else if (key == "os_minor") {
      os_minor = clampi(atoi(val.c_str()), 0, 99);
    } else if (key == "os_build") {
      os_build = clampi(atoi(val.c_str()), 0, 99999);
    } else if (key == "win11_personality") {
      win11_personality = (val == "1" || val == "true");
    } else if (key == "write_watch") {
      write_watch = (val == "1" || val == "true");
    } else if (key == "steam_auth") {
      steam_auth = (val == "1" || val == "true");
    } else if (key == "show_qr") {
      show_qr = (val == "1" || val == "true");
    } else if (key == "log_lines") {
      log_lines = clampi(atoi(val.c_str()), 4, 40);
    }
    // unknown keys are kept in the file but ignored — never fatal
  }
  return applied;
}

int settings_load(const std::string& local_state) {
  FILE* f = fopen((local_state + k_sep + "settings.ini").c_str(), "rb");
  if (!f) return 0;
  std::string text;
  char buf[1024];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
  fclose(f);
  std::lock_guard<std::mutex> lk(g_mu);
  const int applied = g_live.apply_ini(text);
  return applied;
}

int settings_save(const std::string& local_state) {
  std::lock_guard<std::mutex> lk(g_mu);
  const std::string text = g_live.to_ini();
  FILE* f = fopen((local_state + k_sep + "settings.ini").c_str(), "wb");
  if (!f) return -1;
  const size_t w = fwrite(text.data(), 1, text.size(), f);
  fclose(f);
  return (w == text.size()) ? 0 : -1;
}

void settings_store(const host_settings& s, const std::string& local_state) {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    host_settings c = s;  // clamp the same way apply_ini does
    c.mouse_speed = clampi(c.mouse_speed, 1, 100);
    c.wheel_speed = clampi(c.wheel_speed, 1, 100);
    c.os_major = clampi(c.os_major, 5, 12);
    c.os_minor = clampi(c.os_minor, 0, 99);
    c.os_build = clampi(c.os_build, 0, 99999);
    c.log_lines = clampi(c.log_lines, 4, 40);
    if (c.default_input != "auto" && c.default_input != "controller" &&
        c.default_input != "kbm" && c.default_input != "pad2kbm")
      c.default_input = "auto";
    g_live = c;
  }
  settings_save(local_state);
}

host_settings settings_get() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_live;  // by value: callers must not race a concurrent settings_store
}

// ---- bridge-facing getters (lock-free snapshot of the ints that matter) ----
void trt_settings_os(int* major, int* minor, int* build) {
  std::lock_guard<std::mutex> lk(g_mu);
  int mj = g_live.os_major, mn = g_live.os_minor, bd = g_live.os_build;
  if (g_live.win11_personality && bd < 22000) bd = 22000;
  if (major) *major = mj;
  if (minor) *minor = mn;
  if (build) *build = bd;
}

bool trt_settings_write_watch() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_live.write_watch;
}

bool trt_settings_pad_vibration() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_live.pad_vibration;
}

} // namespace longan
