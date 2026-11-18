#pragma once

#include "host_log.h"
#include "../trt/input.h"

#include <GameInput.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>

extern "C" u32 TRT_STDCALL trt_MapVirtualKeyW(u32 code, u32 type);

namespace longan {

// GameInput belongs to the host. Its readings feed the same bridge as
// CoreWindow events, so a guest never needs to load the console's DLL.
class native_gameinput {
 public:
  void open() {
    HMODULE module = GetModuleHandleW(L"GameInput.dll");
    if (!module) module = LoadLibraryW(L"GameInput.dll");
    if (!module) return;
    using create_fn = HRESULT (WINAPI*)(IGameInput**);
    const auto create = reinterpret_cast<create_fn>(GetProcAddress(module, "GameInputCreate"));
    if (create && SUCCEEDED(create(input_.put())) && input_) {
      // The CoreWindow loop only polls while this app has focus.
      log_line("input: native GameInput ready");
    }
  }

  bool keyboard_active() const { return keyboard_active_.load(); }
  bool mouse_active() const { return mouse_active_.load(); }

  void suspend() {
    // The CoreWindow focus handler releases both native and guest input.
    keyboard_active_.store(false);
    mouse_active_.store(false);
    keyboard_reading_ = nullptr;
    mouse_reading_ = nullptr;
    keys_ = {};
    mouse_ = {};
    mouse_baseline_ = false;
    keyboard_missing_since_ = 0;
    mouse_missing_since_ = 0;
  }

  // Return a left release so the UI can handle a click on the launcher.
  bool poll() {
    if (!input_) return false;
    drain(GameInputKindKeyboard, keyboard_reading_, [&](IGameInputReading& reading) {
      update_keyboard(reading);
    });
    bool clicked = false;
    drain(GameInputKindMouse, mouse_reading_, [&](IGameInputReading& reading) {
      clicked |= update_mouse(reading);
    });
    return clicked;
  }

 private:
  void log_reading_status(GameInputKind kind, HRESULT result, IGameInputReading* reading) {
    auto& last_logged = kind == GameInputKindKeyboard
        ? keyboard_status_log_at_ : mouse_status_log_at_;
    const ULONGLONG now = GetTickCount64();
    if (now - last_logged < 5000) return;
    last_logged = now;
    char line[160]{};
    const auto sequence = reading ? reading->GetSequenceNumber(kind) : 0;
    if (kind == GameInputKindKeyboard) {
      std::array<GameInputKeyState, 256> states{};
      const auto count = reading ? std::min<size_t>(states.size(),
          reading->GetKeyState(static_cast<uint32_t>(states.size()), states.data())) : 0;
      std::snprintf(line, sizeof(line),
          "input: GameInput keyboard hr=0x%08lx seq=%llu active=%zu",
          static_cast<unsigned long>(result), static_cast<unsigned long long>(sequence), count);
    } else {
      GameInputMouseState state{};
      const bool available = reading && reading->GetMouseState(&state);
      std::snprintf(line, sizeof(line),
          "input: GameInput mouse hr=0x%08lx seq=%llu state=%u buttons=0x%02x",
          static_cast<unsigned long>(result), static_cast<unsigned long long>(sequence),
          available ? 1u : 0u, available ? state.buttons : 0u);
    }
    log_line(line);
  }

  struct key {
    u16 scan{};
    bool extended{};
    bool down{};
  };

  void release_keyboard() {
    for (unsigned vk = 8; vk < keys_.size(); ++vk) {
      if (keys_[vk].down)
        trt_input_key_event2(static_cast<u16>(vk), false, keys_[vk].scan,
                             keys_[vk].extended, false);
    }
    keys_ = {};
    keyboard_active_.store(false);
  }

  void release_mouse() {
    for (unsigned button = 0; button < 5; ++button) {
      if (mouse_.buttons & (1u << button)) trt_input_mouse_button(button, false);
    }
    mouse_ = {};
    mouse_baseline_ = false;
    mouse_active_.store(false);
  }

  template<class Apply>
  void drain(GameInputKind kind, winrt::com_ptr<IGameInputReading>& last, Apply apply) {
    auto& missing_since = kind == GameInputKindKeyboard
        ? keyboard_missing_since_ : mouse_missing_since_;
    winrt::com_ptr<IGameInputReading> current;
    const HRESULT result = input_->GetCurrentReading(kind, nullptr, current.put());
    log_reading_status(kind, result, current.get());
    if (FAILED(result) || !current) {
      const auto now = GetTickCount64();
      if (!missing_since) missing_since = now;
      if (last && now - missing_since >= 500) {
        if (kind == GameInputKindKeyboard) release_keyboard();
        else release_mouse();
        last = nullptr;
      }
      return;
    }
    missing_since = 0;
    if (!last) {
      apply(*current);
      last = std::move(current);
      return;
    }

    // Device status can be stale while this Xbox still returns live readings.
    // Use the reading stream itself to decide whether input is available.
    // Drain short taps from the history when it advances. Some runtime paths
    // expose fresh snapshots even when the historical iterator stops.
    const auto current_sequence = current->GetSequenceNumber(kind);
    bool advanced = false;
    for (unsigned i = 0; i < 64; ++i) {
      const auto last_sequence = last->GetSequenceNumber(kind);
      if (current_sequence && current_sequence <= last_sequence) break;
      winrt::com_ptr<IGameInputReading> next;
      if (FAILED(input_->GetNextReading(last.get(), kind, nullptr, next.put())) ||
          !next || next.get() == last.get()) break;
      const auto next_sequence = next->GetSequenceNumber(kind);
      if (next_sequence && next_sequence <= last_sequence) break;
      apply(*next);
      last = std::move(next);
      advanced = true;
    }
    if (!advanced || current_sequence > last->GetSequenceNumber(kind)) {
      if (!advanced && current_sequence > last->GetSequenceNumber(kind)) {
        bool& reported = kind == GameInputKindKeyboard
            ? keyboard_snapshot_reported_ : mouse_snapshot_reported_;
        if (!reported) {
          reported = true;
          log_line(kind == GameInputKindKeyboard
                       ? "input: native keyboard uses latest readings"
                       : "input: native mouse uses latest readings");
        }
      }
      apply(*current);
      last = std::move(current);
    }
  }

  void update_keyboard(IGameInputReading& reading) {
    std::array<GameInputKeyState, 256> states{};
    const auto count = std::min<size_t>(
        states.size(), reading.GetKeyState(static_cast<uint32_t>(states.size()), states.data()));
    std::array<key, 256> next{};
    std::array<u32, 256> text{};
    for (size_t i = 0; i < count; ++i) {
      const auto& state = states[i];
      const u32 scan = state.scanCode ? state.scanCode
                                      : trt_MapVirtualKeyW(state.virtualKey, 4);
      const bool extended = (scan & 0xff00u) == 0xe000u;
      u16 vk = state.virtualKey;
      if (vk == 0x10 || vk == 0x11 || vk == 0x12) {
        const u32 mapped = trt_MapVirtualKeyW(scan, 3);
        if (mapped) vk = static_cast<u16>(mapped);
      }
      if (vk < 8 || vk >= next.size() || (vk >= 195 && vk <= 218)) continue;
      next[vk] = {static_cast<u16>(scan & 0xffu), extended, true};
      if (!state.isDeadKey && state.codePoint >= 32) text[vk] = state.codePoint;
    }
    const bool shortcut = next[0xa2].down || next[0xa3].down ||
                          next[0xa4].down || next[0xa5].down;
    for (unsigned vk = 8; vk < next.size(); ++vk) {
      if (keys_[vk].down == next[vk].down) continue;
      const auto& state = next[vk].down ? next[vk] : keys_[vk];
      if (!keyboard_active_.exchange(true) && !keyboard_seen_) {
        keyboard_seen_ = true;
        log_line("input: native GameInput keyboard transition applied");
      }
      if (!next[vk].down || !trt_key_down(static_cast<u16>(vk))) {
        trt_input_key_event2(static_cast<u16>(vk), next[vk].down, state.scan,
                             state.extended, false);
      }
      if (next[vk].down && text[vk] && !shortcut) trt_input_char_event(text[vk]);
    }
    keys_ = next;
  }

  bool update_mouse(IGameInputReading& reading) {
    GameInputMouseState next{};
    if (!reading.GetMouseState(&next)) return false;
    if (mouse_baseline_) {
      const auto delta = [](int64_t after, int64_t before) {
        return static_cast<i32>(std::clamp(after - before, int64_t(-32767), int64_t(32767)));
      };
      const auto dx = delta(next.positionX, mouse_.positionX);
      const auto dy = delta(next.positionY, mouse_.positionY);
      if (dx || dy) {
        mouse_active_.store(true);
        if (!mouse_motion_seen_) {
          mouse_motion_seen_ = true;
          log_line("input: native GameInput mouse movement applied");
        }
        trt_input_mouse_delta_raw(dx, dy);
      }
      const auto wheel = delta(next.wheelY, mouse_.wheelY);
      if (wheel) {
        mouse_active_.store(true);
        trt_input_wheel(wheel);
      }
    }
    const unsigned changed = next.buttons ^ mouse_.buttons;
    for (unsigned button = 0; button < 5; ++button) {
      const unsigned mask = 1u << button;
      if (!(changed & mask)) continue;
      mouse_active_.store(true);
      if (!mouse_button_seen_) {
        mouse_button_seen_ = true;
        log_line("input: native GameInput mouse button applied");
      }
      const bool down = (next.buttons & mask) != 0;
      if (trt_mouse_button(button) != down) trt_input_mouse_button(button, down);
    }
    const bool clicked = (changed & 1u) && !(next.buttons & 1u);
    mouse_ = next;
    mouse_baseline_ = true;
    return clicked;
  }

  winrt::com_ptr<IGameInput> input_;
  winrt::com_ptr<IGameInputReading> keyboard_reading_;
  winrt::com_ptr<IGameInputReading> mouse_reading_;
  std::array<key, 256> keys_{};
  GameInputMouseState mouse_{};
  bool mouse_baseline_ = false;
  bool keyboard_seen_ = false;
  bool mouse_motion_seen_ = false;
  bool mouse_button_seen_ = false;
  bool keyboard_snapshot_reported_ = false;
  bool mouse_snapshot_reported_ = false;
  ULONGLONG keyboard_status_log_at_ = 0;
  ULONGLONG mouse_status_log_at_ = 0;
  ULONGLONG keyboard_missing_since_ = 0;
  ULONGLONG mouse_missing_since_ = 0;
  std::atomic<bool> keyboard_active_{false};
  std::atomic<bool> mouse_active_{false};
};

}  // namespace longan
