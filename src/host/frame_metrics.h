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
  void trim(time cutoff) {
    while (frames.size() >= 2 && frames[1] <= cutoff) frames.pop_front();
  }
public:
  void reset(bool running, time now = clock::now()) {
    std::lock_guard<std::mutex> guard(mutex);
    frames.clear();
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
    frames.push_back(now);
  }
  struct sample { double fps = 0; unsigned slow = 0; bool ready = false; };
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
    for (size_t i = 1; i < frames.size(); ++i)
      if (frames[i] - frames[i-1] > std::chrono::milliseconds(33)) ++out.slow;
    return out;
  }
};
inline frame_window<>& game_frames() { static frame_window<> window; return window; }
}
