#pragma once
#include <chrono>
#include <deque>
#include <mutex>

namespace longan {
template<class clock = std::chrono::steady_clock>
class frame_window {
  using time = typename clock::time_point;
  std::mutex mutex;
  std::deque<time> frames;
  time start{};
  bool active = false;
public:
  struct launch_sample {
    long long first_ms = -1, first_memory_mib = -1, last_memory_mib = -1;
    bool dx_attempted = false, dx_created = false;
    int dx_result = 0;
    unsigned width = 0, height = 0;
  };
private:
  launch_sample launch;
  long long (*memory_mib)() = nullptr;
  void trim(time cutoff) {
    while (frames.size() >= 2 && frames[1] <= cutoff) frames.pop_front();
  }
public:
  void begin_report(long long (*read_memory)()) {
    std::lock_guard<std::mutex> guard(mutex);
    frames.clear();
    launch = {};
    start = clock::now();
    memory_mib = read_memory;
    active = read_memory != nullptr;
  }
  launch_sample launch_info() {
    std::lock_guard<std::mutex> guard(mutex);
    return launch;
  }
  void backbuffer(unsigned width, unsigned height) {
    std::lock_guard<std::mutex> guard(mutex);
    if (active) {
      launch.width = width;
      launch.height = height;
    }
  }
  void memory_sample(long long mib) {
    std::lock_guard<std::mutex> guard(mutex);
    if (active) launch.last_memory_mib = mib;
  }
  void dx_device(int result, bool created) {
    std::lock_guard<std::mutex> guard(mutex);
    if (!active) return;
    launch.dx_attempted = true;
    launch.dx_created |= created;
    launch.dx_result = result;
  }
  void reset(bool running, time now = clock::now()) {
    std::lock_guard<std::mutex> guard(mutex);
    frames.clear();
    if (running) launch = {};
    active = running;
    start = now;
  }
  // A forcibly stopped guest cannot be allowed to die holding the sample lock.
  std::unique_lock<std::mutex> lock_for_guest_termination() {
    return std::unique_lock<std::mutex>(mutex);
  }
  void record() {
    std::lock_guard<std::mutex> guard(mutex);
    if (!active) return;
    const auto now = clock::now();
    trim(now - std::chrono::seconds(30));
    if (launch.first_ms < 0) {
      launch.first_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
      if (memory_mib) launch.first_memory_mib = memory_mib();
    }
    frames.push_back(now);
  }
  struct sample { double fps = 0, mean_ms = -1; unsigned slow = 0; bool ready = false; };
  sample read() {
    std::lock_guard<std::mutex> guard(mutex);
    sample out;
    if (!active || frames.empty()) return out;
    const auto now = clock::now();
    const auto cutoff = now - std::chrono::seconds(30);
    trim(cutoff);
    const auto from = start > cutoff ? start : cutoff;
    const auto seconds = std::chrono::duration<double>(now - from).count();
    if (seconds <= 0) return out;
    size_t count = frames.size() - (frames.front() <= cutoff ? 1 : 0);
    out.fps = count / seconds;
    out.ready = true;
    if (frames.size() > 1)
      out.mean_ms = std::chrono::duration<double, std::milli>(frames.back() - frames.front()).count() /
                    (frames.size() - 1);
    for (size_t i = 1; i < frames.size(); ++i)
      if (frames[i] - frames[i-1] > std::chrono::milliseconds(33)) ++out.slow;
    return out;
  }
};
inline frame_window<>& game_frames() { static frame_window<> window; return window; }
}
