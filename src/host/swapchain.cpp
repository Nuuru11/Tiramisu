// tiramisu/src/host/swapchain.cpp
// D3D11 & D3D12 flip-model swapchain management on a CoreWindow.
// Host UI always uses D3D11 + D2D1; when a guest game requests a D3D12
// swapchain via IDXGIFactory::CreateSwapChain*(command_queue, ...), the
// CoreWindow swapchain is cleanly rebound to that ID3D12CommandQueue and
// restored to D3D11 when the guest exits back to the launcher.
#include "swapchain.h"
#include "host_log.h"
#include "d3d9_backend.hpp"
#include "gl_backend.hpp"
#include "vk_backend.hpp"

#include "../trt/bridge_d3d.h"
#include "../trt/bridge_d3d12.h"
#include "../trt/bridge_d3dx.h"
#include "../trt/input.h"
#include <algorithm>

#include <d3d11on12.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include "settings.h"
#include "d3d12_probe.h"
#include <atomic>
#include <chrono>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace longan {

namespace {
winrt::com_ptr<IUnknown> g_window;
winrt::com_ptr<ID3D11Device1> g_device;
winrt::com_ptr<ID3D11DeviceContext1> g_ctx;
winrt::com_ptr<IDXGISwapChain1> g_swap;
winrt::com_ptr<IDXGISwapChain3> g_swap3;
winrt::com_ptr<ID3D11RenderTargetView> g_rtv;
winrt::com_ptr<ID3D11Texture2D> g_guest_backbuffer;
winrt::com_ptr<IDXGIFactory2> g_factory;
winrt::com_ptr<IDXGIFactory6> g_factory6;
winrt::com_ptr<ID3D12Device> g_d3d12_device;
winrt::com_ptr<IDXGISwapChain3> g_d3d12_swap;
bool g_in_d3d12 = false;
std::atomic<bool> g_pointer_kbm_active{false};
std::atomic<bool> g_launcher_cursor_visible{false};
unsigned g_w = 1280, g_h = 720;
unsigned g_fl = D3D_FEATURE_LEVEL_11_0;

const char* k_vs = R"(struct VSOut { float4 p : SV_POSITION; float2 uv : UV; };
VSOut main(uint id : SV_VertexID) {
  VSOut o;
  // Clockwise winding in D3D11 NDC (+Y up) so default CULL_BACK never culls!
  float2 quad[3] = { float2(-1,-1), float2(-1,3), float2(3,-1) };
  float2 uv[3]   = { float2(0,1),   float2(0,-1), float2(2,1) };
  o.p = float4(quad[id], 0, 1);
  o.uv = uv[id];
  return o;
})";

// Ultra-Crisp 60 FPS Sony PSP / PS3 XMB Silk Wave Shader.
// Lifted neutral studio matte-black canvas (#0F1013 -> #191B1F — true neutral
// black, NOT crushed pitch-black, ZERO pink/purple/navy) with 5 high-definition
// layered translucent silver-white silk ribbons, crisp luminous crests, caustic
// fold sheen, and zero-banding dithering.
const char* k_ps = R"(
cbuffer BgParams : register(b0) {
  float4 uTimeRes; // x = time (sec), y = width, z = height, w = 0
};
struct VSOut { float4 p : SV_POSITION; float2 uv : UV; };

// Compute one high-definition PSP XMB silk ribbon:
// returns (translucent silk veil, razor-sharp luminous crest, caustic sheen)
float3 pspRibbon(float2 uv, float t, float baseY, float amp, float freq,
                 float speed, float phase, float thickness) {
  float x = uv.x * 6.2831853;
  // Multi-harmonic organic wave with gentle phase modulation
  float ph = phase + 0.35 * sin(t * (speed * 0.37) + uv.x * 1.4);
  float y0 = baseY
           + amp * 0.54 * sin(x * freq + t * speed + ph)
           + amp * 0.28 * sin(x * (freq * 1.68) - t * (speed * 0.64) + phase * 1.8)
           + amp * 0.12 * cos(x * (freq * 2.55) + t * (speed * 1.18) - phase * 0.7)
           + amp * 0.06 * sin(x * (freq * 3.80) - t * (speed * 1.55) + phase * 2.3);
  float d = uv.y - y0;

  // Translucent layered silk drapery flowing below the wave crest
  float downVeil = exp(-pow(max(d, 0.0) / thickness, 1.85)) * smoothstep(-0.016, 0.014, d);
  // Soft volumetric glow hugging both sides of the ribbon spine
  float softGlow = exp(-abs(d) / (thickness * 0.26)) * 0.52;
  // Razor-sharp anti-aliased PSP pearlescent crest filament
  float crest = exp(-pow(d / 0.0028, 2.0));
  // Secondary ultra-fine mirror highlight slightly offset beneath crest
  float subCrest = exp(-pow((d - 0.009) / 0.0045, 2.0)) * 0.42;

  // Flowing specular light traveling along the folds of the silk
  float caustic = 0.65 + 0.35 * pow(0.5 + 0.5 * sin(x * (freq * 1.45) + t * (speed * 0.92) + phase), 2.0);
  float body = (downVeil * 0.68 + softGlow * 0.32) * caustic;
  float edge = (crest + subCrest) * (0.76 + 0.24 * caustic);
  return float3(body, edge, caustic * downVeil);
}

float4 main(VSOut i) : SV_Target {
  float t = uTimeRes.x * 0.58;
  float2 uv = i.uv;

  // Neutral lifted studio matte-black gradient (black, NOT pitch-crushed dark black!)
  float3 colTop = float3(0.058, 0.060, 0.066);  // #0F0F11 soft studio black
  float3 colMid = float3(0.102, 0.106, 0.116);  // #1A1B1E elevated neutral charcoal
  float3 colBot = float3(0.052, 0.054, 0.060);  // #0D0E0F matte obsidian
  float midBand = exp(-pow((uv.y - 0.56) / 0.38, 2.0));
  float3 col = lerp(lerp(colTop, colBot, uv.y), colMid, midBand);

  // Subtle neutral silver studio key-light breathing behind the ribbons
  float keyGlow = exp(-pow((uv.y - 0.58) / 0.26, 2.0)) *
                  (0.78 + 0.22 * sin(uv.x * 3.14159 + t * 0.32));
  col += float3(0.048, 0.050, 0.056) * keyGlow;

  // Ribbon 1 (Deep background atmospheric mist — cool neutral silver)
  float3 w1 = pspRibbon(uv, t, 0.44, 0.080, 0.62, 0.36, 0.3, 0.19);
  col += float3(0.28, 0.29, 0.32) * w1.x * 0.28;
  col += float3(0.62, 0.64, 0.68) * w1.y * 0.16;

  // Ribbon 2 (Upper-mid drifting silk — brushed titanium-silver)
  float3 w2 = pspRibbon(uv, t, 0.53, 0.088, 0.78, -0.48, 1.9, 0.15);
  col += float3(0.38, 0.39, 0.42) * w2.x * 0.32;
  col += float3(0.82, 0.84, 0.87) * w2.y * 0.28;

  // Ribbon 3 (Primary iconic PSP hero silk wave — brilliant liquid pearl-white)
  float3 w3 = pspRibbon(uv, t, 0.61, 0.074, 0.92, 0.62, 4.1, 0.12);
  col += float3(0.50, 0.51, 0.54) * w3.x * 0.38;
  col += float3(0.96, 0.97, 0.99) * w3.y * 0.50;
  col += float3(0.22, 0.23, 0.26) * w3.z * 0.16;

  // Ribbon 4 (Foreground counter-wave — crisp platinum filament)
  float3 w4 = pspRibbon(uv, t, 0.70, 0.058, 1.10, -0.74, 0.9, 0.092);
  col += float3(0.46, 0.47, 0.49) * w4.x * 0.30;
  col += float3(0.90, 0.91, 0.93) * w4.y * 0.38;

  // Ribbon 5 (Fine lower-screen whisper ribbon for full-frame balance)
  float3 w5 = pspRibbon(uv, t, 0.82, 0.040, 1.32, 0.85, 2.7, 0.070);
  col += float3(0.36, 0.37, 0.39) * w5.x * 0.22;
  col += float3(0.78, 0.79, 0.82) * w5.y * 0.24;

  // Gentle studio edge falloff (keeps corners rich matte-black, never crushed)
  float2 vp = uv - 0.5;
  float vig = 1.0 - dot(vp * float2(0.46, 0.58), vp * float2(0.46, 0.58));
  col *= saturate(0.80 + 0.24 * vig);

  // Interleaved gradient noise dither (eliminates 8-bit dark-gradient banding)
  float2 px = uv * uTimeRes.yz;
  float ign = frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715))));
  col += (ign - 0.5) / 255.0;

  return float4(saturate(col), 1.0);
})";

int TRT_GUEST_ABI host_d3d12_create_device(void* adapter, u32 min_fl,
                                           const void* riid, void** out_dev) {
  static const IID k_dev_iid = __uuidof(ID3D12Device);
  const HRESULT hr = ::D3D12CreateDevice(
      (IUnknown*)adapter, (D3D_FEATURE_LEVEL)min_fl,
      riid ? *(const IID*)riid : k_dev_iid, out_dev);
  if (SUCCEEDED(hr) && out_dev && *out_dev && !g_d3d12_device) {
    ((IUnknown*)(*out_dev))->QueryInterface(__uuidof(ID3D12Device),
                                            g_d3d12_device.put_void());
  }
  return (int)hr;
}

int TRT_GUEST_ABI host_d3d12_get_debug(const void* riid, void** out_dbg) {
  if (!riid) return (int)E_INVALIDARG;
  return (int)::D3D12GetDebugInterface(*(const IID*)riid, out_dbg);
}

int TRT_GUEST_ABI host_d3d12_ser_root(const void* desc, u32 ver,
                                      void** out_blob, void** out_err) {
  return (int)::D3D12SerializeRootSignature(
      (const D3D12_ROOT_SIGNATURE_DESC*)desc, (D3D_ROOT_SIGNATURE_VERSION)ver,
      (ID3DBlob**)out_blob, (ID3DBlob**)out_err);
}

int TRT_GUEST_ABI host_d3d12_ser_vroot(const void* desc, void** out_blob,
                                       void** out_err) {
  return (int)::D3D12SerializeVersionedRootSignature(
      (const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*)desc,
      (ID3DBlob**)out_blob, (ID3DBlob**)out_err);
}

int TRT_GUEST_ABI host_d3d12_deser_root(const void* src, u64 sz,
                                        const void* riid, void** out_des) {
  if (!riid) return (int)E_INVALIDARG;
  return (int)::D3D12CreateRootSignatureDeserializer(src, (SIZE_T)sz,
                                                   *(const IID*)riid, out_des);
}

int TRT_GUEST_ABI host_d3d12_deser_vroot(const void* src, u64 sz,
                                         const void* riid, void** out_des) {
  if (!riid) return (int)E_INVALIDARG;
  return (int)::D3D12CreateVersionedRootSignatureDeserializer(
      src, (SIZE_T)sz, *(const IID*)riid, out_des);
}

int TRT_GUEST_ABI host_d3d12_get_iface(const void* rclsid, const void* riid,
                                       void** out_iface) {
  if (!rclsid || !riid) return (int)E_INVALIDARG;
  return (int)::D3D12GetInterface(*(const IID*)rclsid, *(const IID*)riid,
                                  out_iface);
}

int TRT_GUEST_ABI host_d3d12_exp(u32 n, const void* iids, void* cfg,
                                 const u32* sizes) {
  return (int)::D3D12EnableExperimentalFeatures(
      n, (const IID*)iids, cfg, const_cast<UINT*>(sizes));
}

DXGI_FORMAT sanitize_flip_fmt(u32 fmt) {
  switch (fmt) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
      return DXGI_FORMAT_B8G8R8A8_UNORM;
  }
}

// A flip-model swapchain takes an UNORM format. D3D11 still permits an
// sRGB render-target view on that buffer when the guest requests sRGB.
int TRT_GUEST_ABI set_d3d11_swapchain_format(u32 format) {
  if (!g_swap || !g_ctx || !g_device) return E_FAIL;
  if (format == DXGI_FORMAT_UNKNOWN) return S_OK;
  const auto wanted = sanitize_flip_fmt(format);
  DXGI_SWAP_CHAIN_DESC1 desc{};
  HRESULT hr = g_swap->GetDesc1(&desc);
  if (FAILED(hr) || desc.Format == wanted) return hr;

  g_ctx->ClearState();
  g_rtv = nullptr;
  g_ctx->Flush();
  hr = g_swap->ResizeBuffers(0, desc.Width, desc.Height, wanted, desc.Flags);
  winrt::com_ptr<ID3D11Texture2D> back;
  if (SUCCEEDED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), back.put_void()))) {
    g_device->CreateRenderTargetView(back.get(), nullptr, g_rtv.put());
    d3d9_rt::g_rt.rtv = g_rtv.get();
    gl_rt::g_glrt.main_rtv = g_rtv.get();
    vk_rt::g_vkrt.main_rtv = g_rtv.get();
  }
  char message[120];
  snprintf(message, sizeof(message), "d3d: CoreWindow backbuffer format %u -> %u, hr=0x%08lx",
           static_cast<unsigned>(desc.Format), static_cast<unsigned>(wanted),
           static_cast<unsigned long>(hr));
  log_line(message);
  return hr;
}

int TRT_GUEST_ABI configure_d3d11_swapchain(u32 format, u32 buffer_count) {
  if (!g_swap || !g_device) return E_FAIL;
  const HRESULT hr = set_d3d11_swapchain_format(format);
  if (FAILED(hr)) return hr;

  const bool single_buffer = buffer_count == 1 ||
                             (buffer_count == 0 && g_guest_backbuffer);
  if (!single_buffer) {
    g_guest_backbuffer = nullptr;
    return S_OK;
  }

  // Desktop swap-effects can expose one stable buffer. UWP needs at
  // least two flip buffers. Keep the desktop render target stable and
  // copy its pixels into the CoreWindow buffer before each flip.
  DXGI_SWAP_CHAIN_DESC1 desc{};
  if (FAILED(g_swap->GetDesc1(&desc))) return E_FAIL;
  const bool srgb = format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                    format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
  DXGI_FORMAT guest_format = srgb ? static_cast<DXGI_FORMAT>(format) : desc.Format;
  if (g_guest_backbuffer) {
    D3D11_TEXTURE2D_DESC current{};
    g_guest_backbuffer->GetDesc(&current);
    if (format == DXGI_FORMAT_UNKNOWN) guest_format = current.Format;
    if (current.Width == desc.Width && current.Height == desc.Height &&
        current.Format == guest_format)
      return S_OK;
  }
  D3D11_TEXTURE2D_DESC texture{};
  texture.Width = desc.Width;
  texture.Height = desc.Height;
  texture.MipLevels = 1;
  texture.ArraySize = 1;
  texture.Format = guest_format;
  texture.SampleDesc.Count = 1;
  texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  winrt::com_ptr<ID3D11Texture2D> target;
  const HRESULT created = g_device->CreateTexture2D(&texture, nullptr, target.put());
  if (FAILED(created)) return created;
  g_guest_backbuffer = std::move(target);
  log_line("d3d: using a single guest backbuffer over the CoreWindow flip chain");
  return S_OK;
}

int TRT_GUEST_ABI get_d3d11_backbuffer(u32 index, const void* iid, void** out) {
  if (!g_guest_backbuffer) return E_NOTIMPL;
  if (out) *out = nullptr;
  if (index != 0 || !out || !iid) return DXGI_ERROR_INVALID_CALL;
  return g_guest_backbuffer->QueryInterface(*static_cast<const IID*>(iid), out);
}

// Paint a small arrow on the presentation surface. ClearView writes just
// these rectangles and leaves the guest's shaders and bindings intact.
void draw_pointer_impl(bool launcher) {
  if (!g_ctx || !g_rtv || !g_w || !g_h) return;
  if (launcher && !g_launcher_cursor_visible.load()) return;
  int x, y;
  if (!trt_cursor_snapshot(&x, &y) && !launcher) return;
  const int scale = g_h >= 900 ? 2 : 1;
  static constexpr const char* rows[] = {
      "#", "##", "#o#", "#oo#", "#ooo#", "#oooo#", "#ooooo#", "#oooooo#",
      "#ooooooo#", "#oooooooo#", "#ooooooooo#", "#ooooo#####",
      "#oo#oo#", "#o##oo#", "##..#oo#", "#...#oo#", ".....###",
  };
  D3D11_RECT dark[64], light[64];
  unsigned n_dark = 0, n_light = 0;
  for (unsigned row = 0; row < sizeof(rows) / sizeof(rows[0]); ++row) {
    const int top = y + static_cast<int>(row) * scale;
    if (top < 0 || top >= static_cast<int>(g_h)) continue;
    const char* pixels = rows[row];
    for (int col = 0; pixels[col];) {
      const int first = col;
      const char color = pixels[col++];
      while (pixels[col] == color) ++col;
      if (color == '.') continue;
      const int left = std::max(0, x + first * scale);
      const int right = std::min(static_cast<int>(g_w), x + col * scale);
      if (left >= right) continue;
      const D3D11_RECT rect{left, top, right, std::min(top + scale, (int)g_h)};
      if (color == '#') dark[n_dark++] = rect;
      else light[n_light++] = rect;
    }
  }
  const float black[4] = {0.015f, 0.015f, 0.015f, 1.f};
  const float fill[4] = {
      g_pointer_kbm_active.load() ? 0.13f : 1.f,
      g_pointer_kbm_active.load() ? 0.83f : 1.f,
      g_pointer_kbm_active.load() ? 0.93f : 1.f,
      1.f,
  };
  if (n_dark) g_ctx->ClearView(g_rtv.get(), black, dark, n_dark);
  if (n_light) g_ctx->ClearView(g_rtv.get(), fill, light, n_light);
}

void TRT_GUEST_ABI prepare_d3d11_present() {
  if (!g_swap || !g_ctx) return;
  if (g_guest_backbuffer) {
    winrt::com_ptr<ID3D11Texture2D> back;
    if (SUCCEEDED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), back.put_void())))
      g_ctx->CopyResource(back.get(), g_guest_backbuffer.get());
  }
  draw_pointer_impl(false);
}

// Host GPU submission for replayed D3D12 command lists. This issues real
// D3D11 DrawInstanced calls on the host context (the device that owns the
// CoreWindow swapchain, possibly D3D11on12). It is a genuine host draw
// submission; guest PSO/root-signature translation remains a separate step.
static int TRT_GUEST_ABI host_d3d12_submit(const trt_d3d12_submit_desc* sd) {
  if (!sd) return (int)E_INVALIDARG;
  if (!g_ctx || g_in_d3d12) return (int)E_FAIL;  // no D3D11 path while D3D12 swap is active
  for (u32 i = 0; i < sd->count && sd->draws; ++i) {
    const auto& d = sd->draws[i];
    g_ctx->DrawInstanced(d.vertices, d.instances,
                         d.start_vertex, d.start_instance);
  }
  return (int)S_OK;
}

void publish_gpu_hooks() {
  trt_gpu_hooks hooks{};
  hooks.device = g_device.get();
  hooks.context = g_ctx.get();
  hooks.swapchain = g_swap.get();
  hooks.factory = g_factory.get();
  hooks.feature_level = g_fl;
  hooks.width = g_w;
  hooks.height = g_h;
  hooks.factory6 = g_factory6.get();
  hooks.swapchain3 = g_swap3.get();
  hooks.d3d12_device = g_d3d12_device.get();
  hooks.d3d12_swapchain = g_d3d12_swap.get();
  hooks.acquire_d3d12_swapchain = [](void* queue, u32 w, u32 h, u32 fmt,
                                     u32 count, void** out_sc) -> int TRT_GUEST_ABI {
    if (!queue || !g_window || !g_factory) return (int)E_INVALIDARG;
    g_rtv = nullptr;
    g_swap3 = nullptr;
    g_swap = nullptr;
    g_d3d12_swap = nullptr;
    if (g_ctx) g_ctx->Flush();

    DXGI_SWAP_CHAIN_DESC1 sc{};
    sc.Width = w ? w : g_w;
    sc.Height = h ? h : g_h;
    sc.Format = sanitize_flip_fmt(fmt);
    sc.SampleDesc.Count = 1;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = (count < 2) ? 2 : (count > 4 ? 4 : count);
    sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    winrt::com_ptr<IDXGISwapChain1> sc1;
    const HRESULT hr = g_factory->CreateSwapChainForCoreWindow(
        (IUnknown*)queue, g_window.get(), &sc, nullptr, sc1.put());
    if (FAILED(hr)) return (int)hr;
    g_d3d12_swap = sc1.try_as<IDXGISwapChain3>();
    g_in_d3d12 = true;
    if (out_sc) *out_sc = g_d3d12_swap ? (void*)g_d3d12_swap.get() : (void*)sc1.get();
    return S_OK;
  };
  hooks.restore_d3d11_swapchain = &swapchain_restore_d3d11;
  hooks.configure_d3d11_swapchain = &configure_d3d11_swapchain;
  hooks.get_d3d11_backbuffer = &get_d3d11_backbuffer;
  hooks.present_interval = [](u32 sync) -> u32 TRT_GUEST_ABI { return game_video::present(sync); };
  hooks.frame_presented = []() TRT_GUEST_ABI { game_frames().record(); };
  hooks.prepare_d3d11_present = &prepare_d3d11_present;
  hooks.d3d11on12_create = [](void* dev, u32 flags, const void* levels,
                              u32 count, void* const* queues, u32 num_queues,
                              u32 node_mask, void** out_dev, void** out_ctx,
                              u32* out_level) -> int TRT_GUEST_ABI {
    return (int)::D3D11On12CreateDevice(
        (IUnknown*)dev, flags, (const D3D_FEATURE_LEVEL*)levels, count,
        (IUnknown* const*)queues, num_queues, node_mask,
        (ID3D11Device**)out_dev, (ID3D11DeviceContext**)out_ctx,
        (D3D_FEATURE_LEVEL*)out_level);
  };
  trt_gpu_set_hooks(&hooks);
}

HRESULT create_d3d11_corewindow_swapchain() {
  if (!g_device || !g_factory || !g_window) return E_FAIL;
  g_rtv = nullptr;
  g_swap3 = nullptr;
  g_swap = nullptr;
  DXGI_SWAP_CHAIN_DESC1 sc{};
  sc.Width = g_w;
  sc.Height = g_h;
  sc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sc.SampleDesc.Count = 1;
  sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sc.BufferCount = 2;
  sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  sc.Flags = 0;
  const HRESULT hr = g_factory->CreateSwapChainForCoreWindow(
      g_device.get(), g_window.get(), &sc, nullptr, g_swap.put());
  if (FAILED(hr)) return hr;
  g_swap3 = g_swap.try_as<IDXGISwapChain3>();
  winrt::com_ptr<ID3D11Texture2D> back;
  g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), back.put_void());
  if (back) g_device->CreateRenderTargetView(back.get(), nullptr, g_rtv.put());
  return S_OK;
}
} // namespace

void draw_pointer(bool launcher) { draw_pointer_impl(launcher); }
void set_pointer_cursor_mode(bool kbm_active) {
  g_pointer_kbm_active.store(kbm_active);
}
void set_launcher_cursor_visible(bool visible) {
  g_launcher_cursor_visible.store(visible);
}

swapchain_result swapchain_create(winrt::Windows::UI::Core::CoreWindow const& w) {
  swapchain_result r{};
  auto bounds = w.Bounds();
  // Settings knob: resolution_scale (%) renders below native size and lets
  // DXGI scale up — the main GPU headroom lever on Series S-class targets.
  const int scale = trt_settings_resolution_scale();
  g_w = (unsigned)bounds.Width;
  g_h = (unsigned)bounds.Height;
  if (scale > 0 && scale != 100) {
    g_w = (unsigned)((u64)g_w * (u64)scale / 100ull);
    g_h = (unsigned)((u64)g_h * (u64)scale / 100ull);
    if (g_w < 320) g_w = 320;
    if (g_h < 180) g_h = 180;
  }
  r.width = g_w;
  r.height = g_h;
  trt_input_display_size(static_cast<int>(g_w), static_cast<int>(g_h));
  winrt::copy_from_abi(g_window, winrt::get_abi(w));

  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  D3D_FEATURE_LEVEL levels[] = {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
  };
  D3D_FEATURE_LEVEL achieved{};
  winrt::com_ptr<ID3D11Device> dev;
  winrt::com_ptr<ID3D11DeviceContext> ctx;
  r.device_hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                  flags, levels, 2, D3D11_SDK_VERSION,
                                  dev.put(), &achieved, ctx.put());
  if (FAILED(r.device_hr)) return r;
  g_fl = (unsigned)achieved;
  r.feature_level = g_fl;
  g_device = dev.as<ID3D11Device1>();
  g_ctx = ctx.as<ID3D11DeviceContext1>();
  // Video-memory baseline at boot: is the 3.8GB video usage seen in
  // TEX-OOM-DIAG (ultrapool) a driver/system reservation, or game-
  // allocated? Logged once at device creation, before any game runs.
  // .199: uses trt_core_log (the .197/.198 log_line version never
  // logged) + a probe line + per-step failure logging.
  {
    trt_core_log("[swapchain] VIDEO-BASELINE probing...");
    IDXGIDevice* dxgi_dev0 = nullptr;
    HRESULT hr_qi = dev ? dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_dev0) : E_POINTER;
    if (SUCCEEDED(hr_qi) && dxgi_dev0) {
      IDXGIAdapter* adapter0 = nullptr;
      HRESULT hr_ad = dxgi_dev0->GetAdapter(&adapter0);
      if (SUCCEEDED(hr_ad) && adapter0) {
        IDXGIAdapter3* adapter30 = nullptr;
        HRESULT hr_a3 = adapter0->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&adapter30);
        if (SUCCEEDED(hr_a3) && adapter30) {
          DXGI_QUERY_VIDEO_MEMORY_INFO vmi{};
          HRESULT hr_vmi = adapter30->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vmi);
          if (SUCCEEDED(hr_vmi)) {
            // .222 DIAG: BOTH segments + driver reservation at
            // boot, matching VIDEO-SAMPLE -- is the ~3.7GB a
            // reservation present from boot, and in which
            // segment?
            DXGI_QUERY_VIDEO_MEMORY_INFO vmi_nl{};
            const bool ok_nl = SUCCEEDED(adapter30->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &vmi_nl));
            char vb[512];
            snprintf(vb, sizeof(vb),
                     "[swapchain] VIDEO-BASELINE local budget=%lluMB usage=%lluMB avail=%lluMB resv=%lluMB nonlocal budget=%lldMB usage=%lldMB avail=%lldMB resv=%lldMB",
                     (unsigned long long)(vmi.Budget >> 20), (unsigned long long)(vmi.CurrentUsage >> 20),
                     (unsigned long long)(vmi.AvailableForReservation >> 20),
                     (unsigned long long)(vmi.CurrentReservation >> 20),
                     ok_nl ? (long long)(vmi_nl.Budget >> 20) : -1ll,
                     ok_nl ? (long long)(vmi_nl.CurrentUsage >> 20) : -1ll,
                     ok_nl ? (long long)(vmi_nl.AvailableForReservation >> 20) : -1ll,
                     ok_nl ? (long long)(vmi_nl.CurrentReservation >> 20) : -1ll);
            trt_core_log(vb);
          } else {
            char vb[128];
            snprintf(vb, sizeof(vb), "[swapchain] VIDEO-BASELINE QueryVideoMemoryInfo failed hr=0x%08x", (unsigned)hr_vmi);
            trt_core_log(vb);
          }
          adapter30->Release();
        } else {
          char vb[128];
          snprintf(vb, sizeof(vb), "[swapchain] VIDEO-BASELINE IDXGIAdapter3 QI failed hr=0x%08x", (unsigned)hr_a3);
          trt_core_log(vb);
        }
        adapter0->Release();
      } else {
        char vb[128];
        snprintf(vb, sizeof(vb), "[swapchain] VIDEO-BASELINE GetAdapter failed hr=0x%08x", (unsigned)hr_ad);
        trt_core_log(vb);
      }
      dxgi_dev0->Release();
    } else {
      char vb[160];
      snprintf(vb, sizeof(vb), "[swapchain] VIDEO-BASELINE IDXGIDevice QI failed hr=0x%08x dev=%p", (unsigned)hr_qi, (void*)dev.get());
      trt_core_log(vb);
    }
  }

  winrt::com_ptr<IDXGIDevice1> dxgi_dev;
  g_device.as(dxgi_dev);
  winrt::com_ptr<IDXGIAdapter> adapter;
  dxgi_dev->GetAdapter(adapter.put());
  adapter->GetParent(__uuidof(IDXGIFactory2), g_factory.put_void());
  if (g_factory) g_factory6 = g_factory.try_as<IDXGIFactory6>();

  r.sc_hr = create_d3d11_corewindow_swapchain();
  if (FAILED(r.sc_hr)) return r;

  trt_d3d12_hooks d12{};
  d12.create_device = &host_d3d12_create_device;
  d12.get_debug_interface = &host_d3d12_get_debug;
  d12.serialize_root_sig = &host_d3d12_ser_root;
  d12.serialize_versioned_root_sig = &host_d3d12_ser_vroot;
  d12.create_root_deserializer = &host_d3d12_deser_root;
  d12.create_versioned_root_deserializer = &host_d3d12_deser_vroot;
  d12.get_interface = &host_d3d12_get_iface;
  d12.enable_experimental = &host_d3d12_exp;
  d12.submit = &host_d3d12_submit;
  trt_d3d12_set_hooks(&d12);

  publish_gpu_hooks();
  trt_d3dx_set_compiler((void*)&D3DCompile);
  return r;
}

void swapchain_restore_d3d11() {
  if (!g_in_d3d12) return;
  g_d3d12_swap = nullptr;
  g_in_d3d12 = false;
  create_d3d11_corewindow_swapchain();
  publish_gpu_hooks();
}

void swapchain_restore_launcher_format() {
  g_guest_backbuffer = nullptr;
  set_d3d11_swapchain_format(DXGI_FORMAT_B8G8R8A8_UNORM);
}

void swapchain_render_frame(float t) {
  if (!g_ctx || !g_rtv || g_in_d3d12) return;
  static winrt::com_ptr<ID3D11VertexShader> vs;
  static winrt::com_ptr<ID3D11PixelShader> ps;
  static winrt::com_ptr<ID3D11Buffer> cb;
  static bool compiled = false;
  if (!compiled) {
    compiled = true;
    winrt::com_ptr<ID3DBlob> vb, pb, err;
    if (SUCCEEDED(D3DCompile(k_vs, strlen(k_vs), nullptr, nullptr, nullptr,
                             "main", "vs_5_0", 0, 0, vb.put(), err.put())))
      g_device->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(),
                                   nullptr, vs.put());
    if (SUCCEEDED(D3DCompile(k_ps, strlen(k_ps), nullptr, nullptr, nullptr,
                             "main", "ps_5_0", 0, 0, pb.put(), err.put()))) {
      g_device->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(),
                                  nullptr, ps.put());
      log_line("swapchain: liquid-silk background shader compiled OK");
    } else if (err && err->GetBufferPointer()) {
      char em[256];
      snprintf(em, sizeof(em), "swapchain: bg PS compile error: %.200s",
               (const char*)err->GetBufferPointer());
      log_line(em);
    }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_device->CreateBuffer(&bd, nullptr, cb.put());
  }
  const float clear[4] = {0.026f, 0.023f, 0.034f, 1.0f};
  g_ctx->ClearRenderTargetView(g_rtv.get(), clear);
  if (vs && ps) {
    if (cb) {
      float params[4] = {t, (float)g_w, (float)g_h, 0.0f};
      g_ctx->UpdateSubresource(cb.get(), 0, nullptr, params, 0, 0);
      ID3D11Buffer* cbs[1] = {cb.get()};
      g_ctx->PSSetConstantBuffers(0, 1, cbs);
    }
    ID3D11RenderTargetView* rtv = g_rtv.get();
    D3D11_VIEWPORT vp{0.f, 0.f, (float)g_w, (float)g_h, 0.f, 1.f};
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->RSSetState(nullptr);
    g_ctx->OMSetDepthStencilState(nullptr, 0);
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(vs.get(), nullptr, 0);
    g_ctx->PSSetShader(ps.get(), nullptr, 0);
    g_ctx->Draw(3, 0);
  }
}

void swapchain_bind_d3d9() {
  d3d9_rt::bind(g_device.get(), g_ctx.get(), g_swap.get(), g_rtv.get(),
                g_w, g_h);
  gl_rt::bind(g_device.get(), g_ctx.get(), g_swap.get(), g_rtv.get(),
              g_w, g_h);
  vk_rt::bind(g_device.get(), g_ctx.get(), g_swap.get(), g_rtv.get(),
              g_w, g_h);
}

static triangle_probe probe;

static bool paint_probe() {
  winrt::com_ptr<ID3D11Texture2D> back;
  if (!g_swap || !g_ctx || FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(back.put())))) return false;
  D3D11_TEXTURE2D_DESC desc;
  back->GetDesc(&desc);
  const UINT size = triangle_probe::size;
  if (desc.Width < size || desc.Height < size || desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) return false;
  const UINT x = (desc.Width-size)/2, y = (desc.Height-size)/2;
  D3D11_BOX box{x,y,0,x+size,y+size,1};
  g_ctx->UpdateSubresource(back.get(),0,&box,probe.pixels.data(),size*4,0);
  return true;
}

std::string swapchain_triangle_test() {
  auto error = probe.render(g_factory.get());
  if (!error.empty()) return error;
  const bool shown = paint_probe() && g_swap->Present(1,0) == S_OK;
  if (shown) probe.preview_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  char result[160];
  snprintf(result, sizeof(result), "DX12 device, submit, pixels OK / DX12 Present 0x%08X / TV preview %s",
           static_cast<unsigned>(probe.present_hr), shown ? "OK" : "unavailable");
  return result;
}

void swapchain_present() {
  if (!g_swap || g_in_d3d12) return;
  // Settings knobs (community wave): vsync off -> Present(0), frame-rate cap
  // -> minimum frame time enforced on the UI thread's present cadence.
  const bool vsync = trt_settings_vsync();
  const int cap = trt_settings_frame_rate_cap();
  static std::chrono::steady_clock::time_point last_present{};
  auto now = std::chrono::steady_clock::now();
  if (cap > 0) {
    const auto min_dt = std::chrono::milliseconds(1000 / cap);
    auto since = now - last_present;
    if (since < min_dt) {
      const auto wait = min_dt - since;
      Sleep((unsigned)std::chrono::duration_cast<std::chrono::milliseconds>(
                wait)
                .count());
      now = std::chrono::steady_clock::now();
    }
  }
  last_present = now;
  if (now < probe.preview_until) paint_probe();
  draw_pointer_impl(true);
  g_swap->Present(vsync ? 1 : 0, 0);
}

void* swapchain_dxgi() { return g_swap.get(); }
void* swapchain_d3d() { return g_device.get(); }

void swapchain_shutdown() {
  d3d9_rt::shutdown();
  gl_rt::shutdown();
  vk_rt::shutdown();
  g_d3d12_swap = nullptr;
  g_d3d12_device = nullptr;
  g_guest_backbuffer = nullptr;
  g_rtv = nullptr;
  g_swap3 = nullptr;
  g_swap = nullptr;
  g_ctx = nullptr;
  g_device = nullptr;
  g_factory6 = nullptr;
  g_factory = nullptr;
  g_window = nullptr;
}

} // namespace longan
