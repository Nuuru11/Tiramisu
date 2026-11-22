// tiramisu/tests/test_host_autopilot.cpp
// Gate for the explicit main.cpp autopilot state transitions.
#include "../src/host/host_autopilot.h"

#include <cstdio>
#include <string>

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { ++fails; printf("  FAIL: %s (line %d)\n", m, __LINE__); } } while (0)

int main() {
  using namespace longan;
  host_autopilot_state& s = host_ap_state();

  CHECK(s.status == "idle" && s.game.empty(), "host autopilot starts idle");

  host_ap_arm("RACCOIN");
  CHECK(s.game == "RACCOIN" && s.status == "armed" && s.attempt == 0,
        "transfer arms the host autopilot for the title");

  host_ap_on_launch("RACCOIN");
  CHECK(s.status == "launched", "launch handoff marks the attempt");

  host_ap_on_result("RACCOIN", "started");
  CHECK(s.status == "fix-pending", "launch outcome opens the fix window");

  host_ap_on_fix(1);
  CHECK(s.status == "fixed" && s.attempt == 1,
        "an applied repair is recorded with its attempt number");

  host_ap_on_launch("RACCOIN");
  host_ap_on_result("RACCOIN", "not found");
  CHECK(s.status == "blocked", "missing title is a terminal host state");

  host_ap_arm("GoNext");
  host_ap_on_launch("GoNext");
  host_ap_on_result("GoNext", "started");
  host_ap_on_no_fix();
  CHECK(s.status == "no-fix" && s.game == "GoNext",
        "no declared repair is a named, terminal state");

  // A stale result for a different title must not change live state.
  host_ap_arm("Celeste");
  host_ap_on_result("RACCOIN", "started");
  CHECK(s.status == "armed" && s.game == "Celeste",
        "results from an older title are ignored");

  printf("  result: %s\n", fails ? "FAIL" : "ALL PASS");
  return fails ? 1 : 0;
}
