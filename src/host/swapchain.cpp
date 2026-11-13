// tiramisu/src/host/swapchain.cpp
// D3D11 flip-model swapchain on a CoreWindow — the exact pipeline shape the
// reference projects proved on console (DX11 FL 11.0 both modes on Series X|S).
// Renders an animated gradient quad: a TV screenshot proves present works.
#include "swapchain.h"

#include "../trt/bridge_d3d.h"
#include "../trt/bridge_d3dx.h"

#include <d3dcompiler.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace longan {

namespace {
winrt::com_ptr<ID3D11Device1> g_device;
winrt::com_ptr<ID3D11DeviceContext1> g_ctx;
winrt::com_ptr<IDXGISwapChain1> g_swap;
winrt::com_ptr<ID3D11RenderTargetView> g_rtv;
winrt::com_ptr<IDXGIFactory2> g_factory;
unsigned g_w = 1280, g_h = 720;

const char* k_vs = R"(struct VSOut { float4 p : SV_POSITION; float2 uv : UV; };
VSOut main(uint id : SV_VertexID) {
  VSOut o;
  float2 quad[3] = { float2(-1,-1), float2(3,-1), float2(-1,3) };
  float2 uv[3]   = { float2(0,1), float2(2,1), float2(0,-1) };
  o.p = float4(quad[id], 0, 1);
  o.uv = uv[id];
  return o;
})";
const char* k_ps = R"(struct VSOut { float4 p : SV_POSITION; float2 uv : UV; };
float4 main(VSOut i) : SV_Target {
  float3 a = float3(0.055, 0.067, 0.086);   // #0E1116 canvas
  float3 b = float3(0.125, 0.502, 0.553);   // Tiramisu teal
  float3 c = lerp(a, b, i.uv.x * i.uv.y);
  return float4(c, 1);
})";
} // namespace

swapchain_result swapchain_create(winrt::Windows::UI::Core::CoreWindow const& w) {
  swapchain_result r{};
  auto bounds = w.Bounds();
  g_w = (unsigned)bounds.Width;
  g_h = (unsigned)bounds.Height;
  r.width = g_w;
  r.height = g_h;

  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
  D3D_FEATURE_LEVEL achieved{};
  winrt::com_ptr<ID3D11Device> dev;
  winrt::com_ptr<ID3D11DeviceContext> ctx;
  r.device_hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                  flags, levels, 1, D3D11_SDK_VERSION,
                                  dev.put(), &achieved, ctx.put());
  if (FAILED(r.device_hr)) return r;
  r.feature_level = (unsigned)achieved;
  g_device = dev.as<ID3D11Device1>();
  g_ctx = ctx.as<ID3D11DeviceContext1>();

  winrt::com_ptr<IDXGIDevice1> dxgi_dev;
  g_device.as(dxgi_dev);
  winrt::com_ptr<IDXGIAdapter> adapter;
  dxgi_dev->GetAdapter(adapter.put());
  winrt::com_ptr<IDXGIFactory2> factory;
  adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void());

  DXGI_SWAP_CHAIN_DESC1 sc{};
  sc.Width = g_w;
  sc.Height = g_h;
  sc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sc.SampleDesc.Count = 1;
  sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sc.BufferCount = 2;
  sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  sc.Flags = 0;
  IUnknown* win = (IUnknown*)winrt::get_abi(w);
  r.sc_hr = factory->CreateSwapChainForCoreWindow(g_device.get(), win, &sc,
                                                  nullptr, g_swap.put());
  if (FAILED(r.sc_hr)) return r;

  winrt::com_ptr<ID3D11Texture2D> back;
  g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), back.put_void());
  g_device->CreateRenderTargetView(back.get(), nullptr, g_rtv.put());

  // publish the GPU to the runtime: a guest game's D3D11 calls must land on
  // THIS device and THIS swapchain (it has no HWND of its own)
  g_factory = factory;
  trt_gpu_hooks hooks{};
  hooks.device = g_device.get();
  hooks.context = g_ctx.get();
  hooks.swapchain = g_swap.get();
  hooks.factory = factory.get();
  hooks.feature_level = r.feature_level;
  hooks.width = g_w;
  hooks.height = g_h;
  trt_gpu_set_hooks(&hooks);
  // the guest's D3DCompile route: hand the SYSTEM d3dcompiler_47 to the
  // d3dx bridge (CompileFromFile needs it)
  trt_d3dx_set_compiler((void*)&D3DCompile);
  return r;
}

void swapchain_render_frame(float t) {
  if (!g_ctx || !g_rtv) return;
  static winrt::com_ptr<ID3D11VertexShader> vs;
  static winrt::com_ptr<ID3D11PixelShader> ps;
  static bool compiled = false;
  if (!compiled) {
    compiled = true;
    winrt::com_ptr<ID3DBlob> vb, pb, err;
    if (SUCCEEDED(D3DCompile(k_vs, strlen(k_vs), nullptr, nullptr, nullptr,
                             "main", "vs_5_0", 0, 0, vb.put(), err.put())))
      g_device->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(),
                                   nullptr, vs.put());
    if (SUCCEEDED(D3DCompile(k_ps, strlen(k_ps), nullptr, nullptr, nullptr,
                             "main", "ps_5_0", 0, 0, pb.put(), err.put())))
      g_device->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(),
                                  nullptr, ps.put());
  }
  const float tealtint = 0.10f + 0.05f * (t * 6.2831853f);
  const float clear[4] = {0.055f + tealtint * 0.3f, 0.067f, 0.086f + tealtint, 1.0f};
  g_ctx->ClearRenderTargetView(g_rtv.get(), clear);
  if (vs && ps) {
    g_ctx->OMSetRenderTargets(1, g_rtv.put(), nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(vs.get(), nullptr, 0);
    g_ctx->PSSetShader(ps.get(), nullptr, 0);
    g_ctx->Draw(3, 0);
  }
}

void swapchain_present() {
  if (g_swap) g_swap->Present(1, 0);
}

void* swapchain_dxgi() { return g_swap.get(); }
void* swapchain_d3d() { return g_device.get(); }

void swapchain_shutdown() {
  g_rtv = nullptr;
  g_swap = nullptr;
  g_ctx = nullptr;
  g_device = nullptr;
}

} // namespace longan
