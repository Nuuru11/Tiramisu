// tiramisu/src/guest/guest_d3d.c
// A guest that drives the D3D11/DXGI path a PC game drives, using the
// REAL SDK headers (so the vtable slots are the SDK's, not ours):
//
//   D3D11CreateDevice        -> must give the HOST's device + context
//   CreateDXGIFactory1       -> must give a factory whose
//   EnumAdapters1            ->   adapter enumeration works
//   CreateSwapChainForHwnd   ->   gives the HOST's swapchain (the guest's
//                                own window is a fake HWND: the console has
//                                no real window for a flip swapchain)
//   GetBuffer/CreateRenderTargetView/Clear/Draw/Present
//                            -> the frame must reach the host's backbuffer
//
// It returns a bitmask: one bit per step that actually happened.
#include <d3d11.h>
#include <dxgi1_2.h>

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

__declspec(dllimport) HRESULT GSTD D3D11CreateDevice(
    IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel,
    ID3D11DeviceContext **ppImmediateContext);

__declspec(dllimport) HRESULT GSTD CreateDXGIFactory2(REFIID riid, void **ppFactory);

static float g_color[4] = {0.1f, 0.2f, 0.3f, 1.0f};

unsigned int GSTD d3d_boot(unsigned long long a, unsigned long long b,
                           unsigned long long c) {
  (void)a; (void)b; (void)c;
  unsigned int bits = 0;

  ID3D11Device *dev = nullptr;
  ID3D11DeviceContext *ctx = nullptr;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                 nullptr, 0, D3D11_SDK_VERSION, &dev, &level,
                                 &ctx);
  bits |= (hr == S_OK && dev != nullptr && ctx != nullptr) ? 0x1u : 0u;
  if (!dev || !ctx) return bits;

  IDXGIFactory2 *factory = nullptr;
  hr = CreateDXGIFactory2(__uuidof(IDXGIFactory2), (void **)&factory);
  bits |= (hr == S_OK && factory != nullptr) ? 0x2u : 0u;
  if (!factory) return bits;

  IDXGIAdapter1 *adapter = nullptr;
  hr = factory->EnumAdapters1(0, &adapter);
  bits |= (hr == S_OK && adapter != nullptr) ? 0x4u : 0u;

  // IDXGIFactory2::CreateSwapChainForHwnd takes the DESC1 form
  // (device, hwnd, desc1, fullscreenDesc, restrictToOutput, out)
  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = 1280;
  desc.Height = 720;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.SampleDesc.Count = 1;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;

  IDXGISwapChain1 *sc = nullptr;
  hr = factory->CreateSwapChainForHwnd(dev, (HWND)(uintptr_t)0x1234, &desc,
                                       nullptr, nullptr, &sc);
  bits |= (hr == S_OK && sc != nullptr) ? 0x8u : 0u;
  if (!sc) return bits;

  ID3D11Texture2D *back = nullptr;
  hr = sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back);
  bits |= (hr == S_OK && back != nullptr) ? 0x10u : 0u;

  ID3D11RenderTargetView *rtv = nullptr;
  hr = dev->CreateRenderTargetView(back, nullptr, &rtv);
  bits |= (hr == S_OK && rtv != nullptr) ? 0x20u : 0u;

  if (rtv) {
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->ClearRenderTargetView(rtv, g_color);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(nullptr, nullptr, 0);
    ctx->Draw(3, 0);
    bits |= 0x40u;  // the draw path ran
  }

  hr = sc->Present(0, 0);
  bits |= (hr == S_OK) ? 0x80u : 0u;

  return bits;
}
