#pragma once

#include <mutex>
#include <string>

// One in-memory snapshot, retained when another title starts.
namespace limbus_report {
struct snapshot {
  std::mutex mutex;
  unsigned attempt = 0;
  bool active = false;
  std::string api, client, init;
};
inline snapshot& state() {
  static snapshot s;
  return s;
}
inline void select_app(unsigned id) {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.active = id == 1973530;
  if (s.active) {
    ++s.attempt;
    s.api = s.client = s.init = "Not observed";
  }
}
inline void module(bool client, const char* result) {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.active) (client ? s.client : s.api) = result;
}
inline void initialization(const char* result) {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.active) s.init = result;
}
inline std::string read() {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.attempt) return "No Limbus Company launch observed this session.";
  return "Attempt " + std::to_string(s.attempt) + "\nsteam_api64.dll: " + s.api +
         "\nsteamclient64.dll: " + s.client + "\nInitialization: " + s.init;
}
}
