// tiramisu/src/host/swapchain.h
#ifndef TIRAMISU_SWAPCHAIN_H
#define TIRAMISU_SWAPCHAIN_H

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <winrt/base.h>
#include <winrt/Windows.UI.Core.h>

namespace tiramisu {

struct swapchain_result {
  HRESULT device_hr;
  HRESULT sc_hr;
  unsigned width, height;
  unsigned feature_level;  // achieved D3D_FEATURE_LEVEL
};

// create a flip-model D3D11 swapchain over the CoreWindow. FLIP_DISCARD +
// BGRA: the only composition-friendly model on Xbox (no exclusive fullscreen).
swapchain_result swapchain_create(winrt::Windows::UI::Core::CoreWindow const& w);

void swapchain_render_frame(float t);   // t in [0,1) animates the proof quad
void swapchain_present();
void swapchain_shutdown();

} // namespace tiramisu

#endif
