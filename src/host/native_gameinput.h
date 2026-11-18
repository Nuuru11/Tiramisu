#pragma once

#include "host_log.h"
#include "../trt/input.h"

#include <GameInput.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <cstdint>

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
      input_->SetFocusPolicy(GameInputDisableBackgroundInput);
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
  struct key {
    u16 scan{};
    bool extended{};
    bool down{};
  };

  template<class Apply>
  void drain(GameInputKind kind, winrt::com_ptr<IGameInputReading>& last, Apply apply) {
    if (last) {
      winrt::com_ptr<IGameInputDevice> device;
      last->GetDevice(device.put());
      if (device && !(device->GetDeviceStatus() & GameInputDeviceConnected)) {
        if (kind == GameInputKindKeyboard) {
          for (unsigned vk = 8; vk < keys_.size(); ++vk) {
            if (keys_[vk].down)
              trt_input_key_event2(static_cast<u16>(vk), false, keys_[vk].scan,
                                   keys_[vk].extended, false);
          }
          keys_ = {};
          keyboard_active_.store(false);
        } else {
          for (unsigned button = 0; button < 5; ++button) {
            if (mouse_.buttons & (1u << button)) trt_input_mouse_button(button, false);
          }
          mouse_ = {};
          mouse_baseline_ = false;
          mouse_active_.store(false);
        }
        last = nullptr;
      }
    }
    if (!last) {
      winrt::com_ptr<IGameInputReading> current;
      if (SUCCEEDED(input_->GetCurrentReading(kind, nullptr, current.put())) && current) {
        apply(*current);
        last = std::move(current);
      }
      return;
    }
    // Walk transitions rather than sampling the latest state: a tap may
    // start and finish between two UI frames.
    for (unsigned i = 0; i < 128; ++i) {
      winrt::com_ptr<IGameInputReading> next;
      const HRESULT hr = input_->GetNextReading(last.get(), kind, nullptr, next.put());
      if (hr == GAMEINPUT_E_REFERENCE_READING_TOO_OLD) {
        winrt::com_ptr<IGameInputReading> current;
        if (SUCCEEDED(input_->GetCurrentReading(kind, nullptr, current.put())) && current) {
          apply(*current);
          last = std::move(current);
        }
        break;
      }
      if (FAILED(hr) || !next) break;
      apply(*next);
      last = std::move(next);
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
    if (!keyboard_active_.exchange(true))
      log_line("input: native GameInput keyboard selected");
    const bool shortcut = next[0xa2].down || next[0xa3].down ||
                          next[0xa4].down || next[0xa5].down;
    for (unsigned vk = 8; vk < next.size(); ++vk) {
      if (keys_[vk].down == next[vk].down) continue;
      const auto& state = next[vk].down ? next[vk] : keys_[vk];
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
    if (!mouse_active_.exchange(true))
      log_line("input: native GameInput mouse selected");
    if (mouse_baseline_) {
      const auto delta = [](int64_t after, int64_t before) {
        return static_cast<i32>(std::clamp(after - before, int64_t(-32767), int64_t(32767)));
      };
      const auto dx = delta(next.positionX, mouse_.positionX);
      const auto dy = delta(next.positionY, mouse_.positionY);
      if (dx || dy) trt_input_mouse_delta_raw(dx, dy);
      const auto wheel = delta(next.wheelY, mouse_.wheelY);
      if (wheel) trt_input_wheel(wheel);
    }
    const unsigned changed = next.buttons ^ mouse_.buttons;
    for (unsigned button = 0; button < 5; ++button) {
      const unsigned mask = 1u << button;
      if (changed & mask)
        trt_input_mouse_button(button, (next.buttons & mask) != 0);
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
  std::atomic<bool> keyboard_active_{false};
  std::atomic<bool> mouse_active_{false};
};

}  // namespace longan
