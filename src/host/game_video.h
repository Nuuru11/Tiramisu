#pragma once
#include <atomic>
#include <chrono>
#include <fstream>
#include <string>
#include <thread>

namespace longan {
struct game_video {
  inline static std::atomic<int> cap{0}, sync{-1};
  static void load(const std::string& dir) {
    std::ifstream file(dir + "/tiramisu_video.txt");
    std::string fps, vsync;
    file >> fps >> vsync;
    cap = fps == "30" ? 30 : fps == "60" ? 60 : fps == "120" ? 120 : 0;
    sync = vsync == "on" ? 1 : vsync == "off" ? 0 : -1;
  }
  static unsigned present(unsigned requested) {
    using clock = std::chrono::steady_clock;
    static thread_local clock::time_point previous{};
    const int rate = cap.load(std::memory_order_relaxed);
    if (rate) {
      const auto deadline = previous + std::chrono::nanoseconds(1000000000 / rate);
      if (clock::now() < deadline) std::this_thread::sleep_until(deadline);
      previous = clock::now();
    }
    const int interval = sync.load(std::memory_order_relaxed);
    return interval < 0 ? requested : static_cast<unsigned>(interval);
  }
};
}
