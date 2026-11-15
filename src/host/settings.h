// tiramisu/src/host/settings.h
// Tiramisu host settings: persisted to LocalState\settings.ini as plain
// `key=value` text. The guest-facing knobs (reported OS version, GC
// write-watch override) are consumed by the trt bridge layer via the
// trt_settings_* getters — a game that checks the OS version or gates its
// allocator on a registry surface gets what the user set, not a constant.
//
// Pure std::string/file I/O: offline-gateable on Linux, no WinRT.
#ifndef TIRAMISU_SETTINGS_H
#define TIRAMISU_SETTINGS_H

#include <string>

namespace longan {

struct host_settings {
  // ---- game defaults ----
  std::string default_input = "auto";  // auto | controller | kbm | pad2kbm
  int mouse_speed = 50;                // gamepad-as-mouse speed, 1..100
  int wheel_speed = 50;                // pad dpad wheel speed, 1..100
  bool pad_vibration = true;           // let guests rumble the pad

  // ---- OS personality reported to guests (games gate on this) ----
  int os_major = 10;
  int os_minor = 0;
  int os_build = 19045;                // Win10 22H2
  bool win11_personality = false;      // 10.0.22000+ (some engines require)

  // ---- compatibility surfaces ----
  bool write_watch = true;             // GetWriteWatch honest impl (GC)
  bool steam_auth = true;              // allow Steam QR link (own account)
  bool show_qr = true;                 // QR overlay on the TV dashboard
  int log_lines = 14;                  // dashboard log tail lines, 4..40

  std::string to_ini() const;
  // Apply parsed ini on top of the current values (missing keys keep
  // defaults). Returns the number of keys applied (0 for an empty file).
  int apply_ini(const std::string& text);
};

// Load from <local_state>\settings.ini (missing file = defaults) and store
// as the live settings. Returns the applied-key count.
int settings_load(const std::string& local_state);
// Save the live settings back to the same file. Returns 0 on success.
int settings_save(const std::string& local_state);
// Replace the live settings (clamped to valid ranges) and persist.
void settings_store(const host_settings& s, const std::string& local_state);
host_settings settings_get();

// ---- consumed by the trt bridge layer ----
// OS version the guest sees (GetVersionExW / GetVersionExW bridge reads it).
void trt_settings_os(int* major, int* minor, int* build);
// Whether the write-watch bridge answers with the real implementation
// (true) or forces the honest fail (false; user disabled GWW).
bool trt_settings_write_watch();
// Whether guest pad-rumble requests reach the pad (false = swallow).
bool trt_settings_pad_vibration();

} // namespace longan

#endif
