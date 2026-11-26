#include "../src/host/frame_metrics.h"
#include <cassert>
struct test_clock : std::chrono::steady_clock {
  static inline time_point t{};
  static time_point now() { return t; }
};
int main() {
  using namespace std::chrono_literals;
  longan::frame_window<test_clock> window;
  assert(!window.read().ready);
  window.reset(true);
  for(int i=0;i<100;++i){test_clock::t+=10ms;window.record();}
  assert(window.read().fps==100 && window.read().slow==0);
  test_clock::t+=34ms;window.record();assert(window.read().slow==1);
  test_clock::t+=31s;assert(window.read().fps==0 && window.read().slow==0);
  window.reset(false);window.record();assert(!window.read().ready);
}
