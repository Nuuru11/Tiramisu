// tiramisu/src/host/ui.cpp
// DirectWrite/D2D overlay. Pixel-consistent: one text scale, fixed margins,
// everything positioned from the 1920x1080 design grid scaled to the
// swapchain size.
#include "ui.h"

#include <d2d1_1.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <stdint.h>

#include "vendor/qrcodegen.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace longan {

namespace {
float g_w = 1920, g_h = 1080;
ID2D1DeviceContext* g_ctx = nullptr;
ID2D1Device* g_d2d_device = nullptr;
IDXGISwapChain1* g_swap = nullptr;
ID2D1SolidColorBrush* g_brush = nullptr;
ID2D1SolidColorBrush* g_teal = nullptr;
ID2D1SolidColorBrush* g_dim = nullptr;
IDWriteFactory* g_dw = nullptr;
IDWriteTextFormat* g_title = nullptr;
IDWriteTextFormat* g_text = nullptr;
IDWriteTextFormat* g_small = nullptr;

std::vector<std::wstring> g_lines;
std::vector<ui_game> g_games;
int g_selected = 0;
std::string g_qr_text;
uint8_t g_qr[qrcodegen_BUFFER_LEN_MAX];
int g_qr_size = 0;
}

void ui_init(unsigned width, unsigned height, void* d2d_factory_v,
             void* dwrite_factory_v, void* d2d_device_v) {
  g_w = (float)width;
  g_h = (float)height;
  ID2D1Factory1* f = (ID2D1Factory1*)d2d_factory_v;
  g_d2d_device = (ID2D1Device*)d2d_device_v;
  g_dw = (IDWriteFactory*)dwrite_factory_v;
  f->AddRef();
  g_dw->AddRef();
  g_d2d_device->AddRef();

  g_d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_ctx);

  D2D1_COLOR_F canvas{0.055f, 0.067f, 0.086f, 1.0f};
  D2D1_COLOR_F teal{0.125f, 0.502f, 0.553f, 1.0f};
  D2D1_COLOR_F dim{0.45f, 0.52f, 0.55f, 1.0f};
  g_ctx->CreateSolidColorBrush(canvas, &g_brush);
  g_ctx->CreateSolidColorBrush(teal, &g_teal);
  g_ctx->CreateSolidColorBrush(dim, &g_dim);

  g_dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                         72.0f, L"en-US", &g_title);
  g_dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                         34.0f, L"en-US", &g_text);
  g_dw->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                         26.0f, L"en-US", &g_small);
}

void ui_shutdown() {
  if (g_ctx) g_ctx->Release();
  if (g_brush) g_brush->Release();
  if (g_teal) g_teal->Release();
  if (g_dim) g_dim->Release();
  if (g_title) g_title->Release();
  if (g_text) g_text->Release();
  if (g_small) g_small->Release();
  if (g_dw) g_dw->Release();
  if (g_d2d_device) g_d2d_device->Release();
  g_ctx = nullptr;
}

void ui_set_games(const std::vector<ui_game>& games, int selected) {
  g_games = games;
  g_selected = selected;
}

void ui_set_lines(const std::vector<std::wstring>& lines) { g_lines = lines; }

void ui_set_qr(const std::string& qr_text) {
  g_qr_text = qr_text;
  g_qr_size = 0;
  if (!qr_text.empty()) {
    qrcodegen_encodeText(qr_text.c_str(), g_qr, g_qr, qrcodegen_Ecc_MEDIUM,
                         qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                         qrcodegen_Mask_AUTO, true);
    g_qr_size = qrcodegen_getSize(g_qr);
  }
}

void text_out(IDWriteTextFormat* fmt, float x, float y, float w, float h,
              const wchar_t* s, ID2D1SolidColorBrush* br) {
  D2D1_RECT_F r{x, y, x + w, y + h};
  g_ctx->DrawText(s, (UINT32)wcslen(s), fmt, r, br);
}

void ui_render() {
  if (!g_ctx || !g_swap) return;
  // bind this frame's backbuffer as the D2D target
  ID2D1Image* old_target = nullptr;
  g_ctx->GetTarget(&old_target);
  ID2D1Bitmap1* bmp = nullptr;
  IDXGISurface* surf = nullptr;
  if (SUCCEEDED(g_swap->GetBuffer(0, __uuidof(IDXGISurface), (void**)&surf))) {
    D2D1_BITMAP_PROPERTIES1 bp{};
    bp.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE};
    g_ctx->CreateBitmapFromDxgiSurface(surf, &bp, &bmp);
    g_ctx->SetTarget(bmp);
    g_ctx->BeginDraw();

    const float m = g_h * 0.06f;

    // title
    text_out(g_title, m, m * 0.55f, g_w - 2 * m, g_h * 0.1f, L"Tiramisu", g_teal);

    // transfer URL + status lines
    float y = m + g_h * 0.1f;
    for (const auto& l : g_lines) {
      text_out(g_small, m, y, g_w - 2 * m, g_h * 0.045f, l.c_str(), g_dim);
      y += g_h * 0.042f;
    }

    // game shelf
    y += g_h * 0.02f;
    const float row_h = g_h * 0.072f;
    int shown = 0;
    for (int i = 0; i < (int)g_games.size() && shown < 6; ++i) {
      if (i < g_selected - 2) continue;  // keep selection visible
      const float a = i == g_selected ? 1.0f : 0.0f;
      if (a > 0.0f) {
        D2D1_RECT_F sel{m - 12, y - 4, g_w - m + 12, y + row_h};
        g_ctx->FillRectangle(&sel, g_teal);
      }
      D2D1_COLOR_F white{0.92f, 0.94f, 0.96f, i == g_selected ? 0.0f : 0.75f};
      // selection row inverts: dark text on teal
      ID2D1SolidColorBrush* br = g_dim;
      D2D1_COLOR_F onsel{0.05f, 0.07f, 0.086f, 1.0f};
      ID2D1SolidColorBrush* fg = g_dim;
      if (i == g_selected) {
        g_ctx->CreateSolidColorBrush(onsel, &fg);
      }
      const std::wstring row = g_games[i].name + L"   —   " + g_games[i].exe;
      text_out(g_text, m, y, g_w - 2 * m, row_h, row.c_str(),
               i == g_selected ? fg : br);
      if (i == g_selected) fg->Release();
      y += row_h;
      ++shown;
    }
    if (g_games.empty()) {
      text_out(g_text, m, y, g_w - 2 * m, row_h,
               L"No games yet — scan the QR, upload a .zip of a game folder", g_dim);
      y += row_h;
    }

    // QR (transfer URL or Steam login) bottom-right
    if (g_qr_size > 0) {
      const float q = g_h * 0.30f;
      const float qx = g_w - m - q, qy = g_h - m - q - g_h * 0.02f;
      D2D1_RECT_F bg{qx - 14, qy - 14, qx + q + 14, qy + q + 14};
      D2D1_COLOR_F white{0.94f, 0.96f, 0.97f, 1.0f};
      ID2D1SolidColorBrush* wbr = nullptr;
      g_ctx->CreateSolidColorBrush(white, &wbr);
      g_ctx->FillRectangle(&bg, wbr);
      const float cell = q / (float)g_qr_size;
      D2D1_COLOR_F black{0.05f, 0.07f, 0.086f, 1.0f};
      ID2D1SolidColorBrush* bbr = nullptr;
      g_ctx->CreateSolidColorBrush(black, &bbr);
      for (int yy = 0; yy < g_qr_size; ++yy)
        for (int xx = 0; xx < g_qr_size; ++xx)
          if (qrcodegen_getModule(g_qr, xx, yy)) {
            D2D1_RECT_F c{qx + xx * cell, qy + yy * cell, qx + (xx + 1) * cell,
                          qy + (yy + 1) * cell};
            g_ctx->FillRectangle(&c, bbr);
          }
      bbr->Release();
      wbr->Release();
    }

    g_ctx->EndDraw();
    if (bmp) bmp->Release();
    surf->Release();
    g_ctx->SetTarget(old_target);
    if (old_target) old_target->Release();
  }
}

} // namespace longan
