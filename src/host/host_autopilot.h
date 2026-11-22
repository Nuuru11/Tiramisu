// tiramisu/src/host/host_autopilot.h
// Explicit in-console autopilot state owned by main.cpp. The host loop arms
// it on a transfer, marks each launch, and records the launch outcome; the
// webxfer state machine applies the actual repair and re-queues. Keeping the
// transitions here lets the host side be gated without booting a console.
#ifndef TIRAMISU_HOST_AUTOPILOT_H
#define TIRAMISU_HOST_AUTOPILOT_H

#include <string>

namespace longan {

struct host_autopilot_state {
  std::string game;
  int attempt = 0;
  // idle -> armed -> launched -> fix-pending -> (fixed | no-fix | blocked)
  std::string status = "idle";
};

inline host_autopilot_state& host_ap_state() {
  static host_autopilot_state s;
  return s;
}

inline void host_ap_arm(const std::string& game) {
  host_autopilot_state& s = host_ap_state();
  s.game = game;
  s.attempt = 0;
  s.status = "armed";
}

inline void host_ap_on_launch(const std::string& game) {
  host_autopilot_state& s = host_ap_state();
  if (s.game.empty() || s.game == game) {
    s.game = game;
    s.status = "launched";
  }
}

inline void host_ap_on_result(const std::string& game,
                              const std::string& result) {
  host_autopilot_state& s = host_ap_state();
  if (s.game != game) return;
  if (result == "started") s.status = "fix-pending";
  else if (result == "not found") s.status = "blocked";
}

inline void host_ap_on_fix(int attempt) {
  host_autopilot_state& s = host_ap_state();
  s.attempt = attempt;
  s.status = "fixed";
}

inline void host_ap_on_no_fix() { host_ap_state().status = "no-fix"; }

}  // namespace longan
#endif
