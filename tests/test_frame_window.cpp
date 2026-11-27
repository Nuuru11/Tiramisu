#include "../src/host/frame_metrics.h"
#include <cassert>
struct test_clock : std::chrono::steady_clock {
  static inline time_point t{};
  static time_point now() { return t; }
};
int main() {
  using namespace std::chrono_literals;
  longan::frame_window<test_clock> window;
  window.begin_report([]() -> long long { return 432; });
  window.dx_device(-1, false);
  assert(window.launch_info().dx_attempted && !window.launch_info().dx_created);
  window.dx_device(0, true);
  test_clock::t+=40ms;
  window.record();
  test_clock::t+=5ms;
  window.record();
  assert(window.read().mean_ms==5);
  window.reset(false);
  assert(window.launch_info().dx_created && window.launch_info().first_ms==40);
  assert(window.launch_info().first_memory_mib==432);
  window.begin_report(nullptr);
  window.record();
  assert(window.launch_info().first_ms<0 && !window.launch_info().dx_attempted);
  assert(!window.read().ready);
  window.reset(true);
  for(int i=0;i<100;++i){test_clock::t+=10ms;window.record();}
  assert(window.read().fps==100 && window.read().slow==0);
  test_clock::t+=34ms;window.record();assert(window.read().slow==1);
  test_clock::t+=31s;assert(window.read().fps==0 && window.read().slow==0);
  window.reset(false);window.record();assert(!window.read().ready);
}
