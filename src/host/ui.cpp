// tiramisu/src/host/ui.cpp
// DirectWrite/D2D overlay. Pixel-consistent: one text scale, fixed margins,
// everything positioned from the 1920x1080 design grid scaled to the
// swapchain size.
#include "ui.h"
#include <algorithm>
#include "settings.h"

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
std::vector<std::wstring> g_setup_steps;
int g_setup_done_count = 0;
bool g_setup_finished = false;
bool g_first_run = true;
std::wstring g_transfer_url;
std::wstring g_transfer_status;
std::wstring g_control_hint;
std::vector<ui_game> g_games;
int g_selected = 0;
std::string g_qr_text;
uint8_t g_qr[qrcodegen_BUFFER_LEN_MAX];
int g_qr_size = 0;

// dashboard screens
ui_screen g_screen = UI_SHELF;
bool g_quick_menu = false;
float g_fps = 0.0f;
std::wstring g_sysinfo;
std::vector<ui_setting> g_set_rows;
int g_set_selected = 0;
std::vector<std::wstring> g_log_tail;

int g_steam_selected = 0;
std::vector<ui_steam_item> g_steam_items;
bool g_steam_signed_in = false;
uint64_t g_steam_id64 = 0;
std::wstring g_steam_persona;
std::wstring g_steam_status_line = L"Sign in with Steam QR (Y) to load purchases";
std::vector<ui_steam_friend> g_steam_friends;
std::vector<ui_steam_ach> g_steam_achs;
int g_qm_selected = 0;
}

void ui_steam_nav_up() {
  if (g_steam_selected > 0) --g_steam_selected;
}

void ui_steam_nav_down() {
  if (g_steam_selected + 1 < static_cast<int>(g_steam_items.size()))
    ++g_steam_selected;
}

int ui_steam_selected_index() { return g_steam_selected; }

void ui_set_steam_account(bool signed_in, uint64_t steamid64,
                          std::wstring persona, std::wstring status_line) {
  g_steam_signed_in = signed_in;
  g_steam_id64 = steamid64;
  g_steam_persona = std::move(persona);
  g_steam_status_line = std::move(status_line);
}

void ui_set_steam_items(const std::vector<ui_steam_item>& items) {
  g_steam_items = items;
  if (g_steam_selected >= static_cast<int>(g_steam_items.size())) {
    g_steam_selected = items.empty() ? 0 : static_cast<int>(items.size()) - 1;
  }
}

void ui_set_steam_friends(const std::vector<ui_steam_friend>& friends) {
  g_steam_friends = friends;
}

void ui_set_steam_achievements(const std::vector<ui_steam_ach>& achs) {
  g_steam_achs = achs;
}

void ui_quick_menu_nav_up() {
  if (g_qm_selected > 0) --g_qm_selected;
}

void ui_quick_menu_nav_down() {
  if (g_qm_selected < 5) ++g_qm_selected;
}

int ui_quick_menu_selected() { return g_qm_selected; }

void ui_init(unsigned width, unsigned height, void* d2d_factory_v,
             void* dwrite_factory_v, void* d2d_device_v, void* swapchain_v) {
  g_w = (float)width;
  g_h = (float)height;
  ID2D1Factory1* f = (ID2D1Factory1*)d2d_factory_v;
  g_d2d_device = (ID2D1Device*)d2d_device_v;
  g_dw = (IDWriteFactory*)dwrite_factory_v;
  g_swap = (IDXGISwapChain1*)swapchain_v;
  f->AddRef();
  g_dw->AddRef();
  g_d2d_device->AddRef();
  if (g_swap) g_swap->AddRef();

  g_d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_ctx);

  D2D1_COLOR_F white{0.98f, 0.92f, 0.83f, 1.0f};
  D2D1_COLOR_F teal{0.87f, 0.59f, 0.34f, 1.0f};
  D2D1_COLOR_F dim{0.73f, 0.64f, 0.56f, 1.0f};
  g_ctx->CreateSolidColorBrush(white, &g_brush);
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
  if (g_swap) g_swap->Release();
  g_ctx = nullptr;
  g_brush = g_teal = g_dim = nullptr;
  g_title = g_text = g_small = nullptr;
  g_dw = nullptr;
  g_d2d_device = nullptr;
  g_swap = nullptr;
}

void ui_set_games(const std::vector<ui_game>& games, int selected) {
  g_games = games;
  g_selected = selected;
}

void ui_set_lines(const std::vector<std::wstring>& lines) { g_lines = lines; }

void ui_set_setup(const std::vector<std::wstring>& steps, int done_count,
                  bool finished) {
  g_setup_steps = steps;
  g_setup_done_count = done_count;
  g_setup_finished = finished;
}

void ui_dismiss_first_run() {
  g_first_run = false;
  if (g_screen == UI_SETUP) g_screen = UI_SHELF;
}

void ui_set_control_hint(std::wstring hint) { g_control_hint.swap(hint); }

void ui_set_transfer(std::wstring url, std::wstring status) {
  g_transfer_url.swap(url);
  g_transfer_status.swap(status);
}

void ui_set_screen(ui_screen s) { g_screen = s; }

void ui_set_quick_menu(bool visible) {
  g_quick_menu = visible;
  if (visible) g_screen = UI_QUICKMENU;
  else g_screen = UI_SHELF;
}

void ui_set_fps(float fps) { g_fps = fps; }
void ui_set_sysinfo(const std::wstring& info) { g_sysinfo = info; }

void ui_set_settings(const ui_setting* rows, int count, int selected) {
  g_set_rows.assign(rows, rows + (count > 0 ? count : 0));
  g_set_selected = selected;
}

void ui_set_log_tail(const std::vector<std::wstring>& lines) {
  g_log_tail = lines;
}

void ui_set_qr(const std::string& qr_text) {
  if (g_qr_text == qr_text) return;
  g_qr_text = qr_text;
  g_qr_size = 0;
  if (!qr_text.empty()) {
    uint8_t work[qrcodegen_BUFFER_LEN_MAX];
    if (qrcodegen_encodeText(qr_text.c_str(), work, g_qr, qrcodegen_Ecc_MEDIUM,
                            qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                            qrcodegen_Mask_AUTO, true))
      g_qr_size = qrcodegen_getSize(g_qr);
  }
}

void text_out(IDWriteTextFormat* fmt, float x, float y, float w, float h,
              const wchar_t* s, ID2D1SolidColorBrush* br) {
  D2D1_RECT_F r{x, y, x + w, y + h};
  g_ctx->DrawText(s, (UINT32)wcslen(s), fmt, r, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void draw_panel(float x, float y, float w, float h, bool focused = false);

void draw_qr_overlay(float m) {
  if (g_qr_size <= 0) return;
  const float cell = (float)(int)(g_h * 0.25f / (g_qr_size + 8));
  if (cell < 1) return;
  const float q = cell * g_qr_size, quiet = 4 * cell;
  const float qx = (float)(int)(g_w - m - q - quiet);
  const float qy = (float)(int)(g_h - m - q - quiet - g_h * 0.06f);
  D2D1_RECT_F bg{qx - quiet, qy - quiet, qx + q + quiet, qy + q + quiet};
  D2D1_COLOR_F white{0.94f, 0.96f, 0.97f, 1.0f};
  ID2D1SolidColorBrush* wbr = nullptr;
  g_ctx->CreateSolidColorBrush(white, &wbr);
  g_ctx->FillRectangle(&bg, wbr);
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
  text_out(g_small, qx - quiet, qy + q + quiet + 8, q + 2 * quiet, g_h * 0.04f,
           L"SCAN OR OPEN LAN URL", g_teal);
}

void draw_quick_menu(float m) {
  // Steam Community In-Game Social Overlay (activated via Select+Start or Steam API)
  const float panel_w = g_w * 0.72f;
  const float panel_h = g_h * 0.78f;
  const float px = (g_w - panel_w) * 0.5f;
  const float py = (g_h - panel_h) * 0.5f;

  // Dark Steam obsidian translucent backdrop
  D2D1_COLOR_F bg{0.07f, 0.09f, 0.12f, 0.94f};
  ID2D1SolidColorBrush* bbr = nullptr;
  g_ctx->CreateSolidColorBrush(bg, &bbr);
  D2D1_ROUNDED_RECT panel_box{{px, py, px + panel_w, py + panel_h}, 12.0f, 12.0f};
  g_ctx->FillRoundedRectangle(&panel_box, bbr);
  bbr->Release();

  // Steam subtle cyan border
  D2D1_COLOR_F border{0.10f, 0.62f, 0.96f, 0.75f};
  ID2D1SolidColorBrush* bor = nullptr;
  g_ctx->CreateSolidColorBrush(border, &bor);
  g_ctx->DrawRoundedRectangle(&panel_box, bor, 2.5f);
  bor->Release();

  // Top header banner
  D2D1_RECT_F header_rect{px, py, px + panel_w, py + g_h * 0.08f};
  D2D1_COLOR_F header_bg{0.10f, 0.15f, 0.22f, 0.95f};
  ID2D1SolidColorBrush* hbr = nullptr;
  g_ctx->CreateSolidColorBrush(header_bg, &hbr);
  g_ctx->FillRectangle(&header_rect, hbr);
  hbr->Release();

  text_out(g_title, px + 24, py + 8, panel_w * 0.5f, g_h * 0.06f,
           L"STEAM COMMUNITY OVERLAY", g_teal);

  // Active User / Rich Presence status (live Steam identity)
  wchar_t acct_buf[160];
  if (g_steam_signed_in) {
    swprintf(acct_buf, 160, L"%s (SteamID %llu)  •  In-Game",
             g_steam_persona.empty() ? L"Steam User" : g_steam_persona.c_str(),
             static_cast<unsigned long long>(g_steam_id64));
  } else {
    swprintf(acct_buf, 160, L"Unlinked Guest Session  •  Sign in on Steam Tab");
  }
  text_out(g_small, px + 24, py + g_h * 0.052f, panel_w * 0.56f, g_h * 0.025f,
           acct_buf, g_brush);

  // Top Right Stats (live FPS & frame time)
  const float fps_val = g_fps > 0.5f ? g_fps : 60.0f;
  wchar_t perfbuf[128];
  swprintf(perfbuf, 128, L"%.1f FPS  •  %.1fms  •  Xbox Dev Mode UWP",
           fps_val, 1000.0f / fps_val);
  text_out(g_small, px + panel_w * 0.45f, py + 14, panel_w * 0.52f, g_h * 0.04f,
           perfbuf, g_teal);

  const float col_y = py + g_h * 0.10f;
  const float col_h = panel_h - g_h * 0.16f;
  const float left_w = panel_w * 0.38f;
  const float right_x = px + left_w + 20.0f;
  const float right_w = panel_w - left_w - 40.0f;

  // Left Column: Quick Navigation Menu
  draw_panel(px + 16, col_y, left_w, col_h);
  text_out(g_small, px + 30, col_y + 14, left_w - 40, g_h * 0.03f,
           L"NAVIGATION", g_teal);

  int online_friends = 0;
  for (const auto& f : g_steam_friends) {
    if (f.state > 0 || !f.game_name.empty()) ++online_friends;
  }
  int ach_unlocked = 0;
  for (const auto& a : g_steam_achs) {
    if (a.achieved) ++ach_unlocked;
  }
  wchar_t fr_label[64], ach_label[64];
  swprintf(fr_label, 64, L"👥  Friends (%d Online)", online_friends);
  swprintf(ach_label, 64, L"🏆  Achievements (%d/%zu)", ach_unlocked,
           g_steam_achs.size());
  const wchar_t* nav_items[] = {
    L"▶  Resume Game",
    fr_label,
    ach_label,
    L"🎮  Toggle Pad Mouse Mode",
    L"🔄  Refresh Steam Profile",
    L"⏏  Exit to Tiramisu Shelf",
  };
  float nav_y = col_y + g_h * 0.06f;
  const float row_h = g_h * 0.052f;
  for (int i = 0; i < 6; ++i) {
    const bool sel = (i == g_qm_selected);
    draw_panel(px + 24, nav_y, left_w - 16, row_h - 4, sel);
    text_out(g_text, px + 36, nav_y + 4, left_w - 40, row_h - 8,
             nav_items[i], sel ? g_teal : g_brush);
    nav_y += row_h;
  }

  // Right Column: Live Steam Friends & Activity
  draw_panel(right_x, col_y, right_w, col_h);
  text_out(g_small, right_x + 18, col_y + 14, right_w - 36, g_h * 0.03f,
           L"STEAM FRIENDS & COMMUNITY", g_teal);

  float social_y = col_y + g_h * 0.055f;
  if (g_steam_friends.empty()) {
    text_out(g_text, right_x + 18, social_y, right_w - 36, g_h * 0.06f,
             g_steam_signed_in ? L"No Steam friends online or private profile"
                               : L"Sign in with Steam QR on the Steam tab to view friends",
             g_dim);
    social_y += g_h * 0.065f;
  } else {
    size_t show_n = std::min<size_t>(g_steam_friends.size(), 5);
    for (size_t i = 0; i < show_n; ++i) {
      const auto& fr = g_steam_friends[i];
      wchar_t fbuf[160];
      if (!fr.game_name.empty()) {
        swprintf(fbuf, 160, L"●  %s — Playing %s", fr.persona.c_str(),
                 fr.game_name.c_str());
      } else if (fr.state > 0) {
        swprintf(fbuf, 160, L"●  %s — Online", fr.persona.c_str());
      } else {
        swprintf(fbuf, 160, L"○  %s — Offline", fr.persona.c_str());
      }
      text_out(g_text, right_x + 18, social_y, right_w - 36, g_h * 0.04f,
               fbuf, (!fr.game_name.empty() || fr.state > 0) ? g_brush : g_dim);
      social_y += g_h * 0.045f;
    }
  }

  // In-Game Achievement progress card
  social_y += g_h * 0.02f;
  D2D1_ROUNDED_RECT ach_box{{right_x + 18, social_y, right_x + right_w - 18,
                             std::min(col_y + col_h - 14.0f, social_y + g_h * 0.19f)},
                            8.0f, 8.0f};
  D2D1_COLOR_F ach_bg{0.12f, 0.18f, 0.25f, 0.9f};
  ID2D1SolidColorBrush* abr = nullptr;
  g_ctx->CreateSolidColorBrush(ach_bg, &abr);
  g_ctx->FillRoundedRectangle(&ach_box, abr);
  abr->Release();

  const float ach_ratio = g_steam_achs.empty()
                              ? 0.0f
                              : static_cast<float>(ach_unlocked) /
                                    static_cast<float>(g_steam_achs.size());
  wchar_t ach_hdr[128];
  if (g_steam_achs.empty()) {
    swprintf(ach_hdr, 128, L"ACHIEVEMENTS (NONE RECORDED FOR ACTIVE SESSION)");
  } else {
    swprintf(ach_hdr, 128, L"ACHIEVEMENTS (%d / %zu UNLOCKED — %d%%)",
             ach_unlocked, g_steam_achs.size(),
             static_cast<int>(ach_ratio * 100.0f));
  }
  text_out(g_small, right_x + 30, social_y + 10, right_w - 60, g_h * 0.03f,
           ach_hdr, g_teal);

  // Progress Bar
  const float bar_w = right_w - 60.0f;
  const float bar_h = 10.0f;
  const float bar_y = social_y + g_h * 0.045f;
  D2D1_RECT_F pbg{right_x + 30, bar_y, right_x + 30 + bar_w, bar_y + bar_h};
  D2D1_COLOR_F pbg_col{0.05f, 0.07f, 0.09f, 1.0f};
  ID2D1SolidColorBrush* pbbr = nullptr;
  g_ctx->CreateSolidColorBrush(pbg_col, &pbbr);
  g_ctx->FillRectangle(&pbg, pbbr);
  pbbr->Release();

  if (ach_ratio > 0.0f) {
    D2D1_RECT_F pfill{right_x + 30, bar_y, right_x + 30 + bar_w * ach_ratio,
                      bar_y + bar_h};
    g_ctx->FillRectangle(&pfill, g_teal);
  }

  float ay = social_y + g_h * 0.065f;
  for (size_t i = 0; i < g_steam_achs.size() && i < 2; ++i) {
    const auto& a = g_steam_achs[i];
    wchar_t abuf[192];
    if (a.achieved) {
      swprintf(abuf, 192, L"★ %s (Unlocked)", a.name.c_str());
    } else if (a.max_progress > 0) {
      swprintf(abuf, 192, L"☆ %s (%u/%u)", a.name.c_str(),
               a.cur_progress, a.max_progress);
    } else {
      swprintf(abuf, 192, L"☆ %s (Locked)", a.name.c_str());
    }
    text_out(g_small, right_x + 30, ay, right_w - 60, g_h * 0.035f, abuf,
             a.achieved ? g_brush : g_dim);
    ay += g_h * 0.035f;
  }

  // Footer bar
  text_out(g_small, px + 24, py + panel_h - g_h * 0.042f, panel_w - 48,
           g_h * 0.03f, L"Back+Start: Close Steam Overlay   |   (A) Select   |   (B) Resume Game", g_dim);
}

void draw_fps_overlay() {
  if (g_fps <= 0) return;
  wchar_t buf[32];
  swprintf(buf, 32, L"%.0f FPS", g_fps);
  // top-right corner
  D2D1_COLOR_F bg{0.0f, 0.0f, 0.0f, 0.6f};
  ID2D1SolidColorBrush* bbr = nullptr;
  g_ctx->CreateSolidColorBrush(bg, &bbr);
  D2D1_RECT_F r{g_w - 120, 8, g_w - 8, 36};
  g_ctx->FillRectangle(&r, bbr);
  bbr->Release();
  text_out(g_small, g_w - 112, 10, 100, 24, buf, g_teal);
}

void draw_sysinfo_overlay() {
  if (g_sysinfo.empty()) return;
  D2D1_COLOR_F bg{0.0f, 0.0f, 0.0f, 0.6f};
  ID2D1SolidColorBrush* bbr = nullptr;
  g_ctx->CreateSolidColorBrush(bg, &bbr);
  D2D1_RECT_F r{8, 8, g_w * 0.35f, 28};
  g_ctx->FillRectangle(&r, bbr);
  bbr->Release();
  text_out(g_small, 12, 10, g_w * 0.35f - 8, 20, g_sysinfo.c_str(), g_dim);
}

void draw_panel(float x, float y, float w, float h, bool focused) {
  D2D1_ROUNDED_RECT box{{x, y, x + w, y + h}, 14.0f, 14.0f};
  ID2D1SolidColorBrush* surface = nullptr;
  const D2D1_COLOR_F color{0.18f, 0.12f, 0.09f, 1.0f};
  if (SUCCEEDED(g_ctx->CreateSolidColorBrush(color, &surface))) {
    g_ctx->FillRoundedRectangle(&box, surface);
    surface->Release();
  }
  if (focused) g_ctx->DrawRoundedRectangle(&box, g_teal, 3.0f);
}

void draw_tabs(float m) {
  const wchar_t* names[UI_TAB_COUNT] = {L"Home", L"Settings", L"Live Log", L"Setup", L"Steam"};
  const float w = g_w * 0.10f, h = g_h * 0.05f, y = g_h * 0.045f;
  for (int i = 0; i < UI_TAB_COUNT; ++i) {
    const float x = m + i * (w + g_w * 0.010f);
    const bool focused = (g_first_run ? UI_SETUP : g_screen) == i;
    draw_panel(x, y, w, h, focused);
    text_out(g_text, x + 16, y + 4, w - 24, h - 4, names[i],
             focused ? g_teal : g_dim);
  }
  if (!g_control_hint.empty())
    text_out(g_small, g_w * 0.58f, y + 4, g_w * 0.40f, h,
             g_control_hint.c_str(), g_teal);
}

void draw_settings_screen(float m, float y0) {
  if (g_set_rows.empty()) {
    text_out(g_text, m, y0, g_w - 2 * m, g_h * 0.06f,
             L"No settings rows (host not ready)", g_dim);
    return;
  }
  const float row_h = g_h * 0.058f;
  const float lab_w = g_w * 0.38f;
  const float val_w = g_w * 0.34f;
  wchar_t hint[128];
  const ui_setting& sel = g_set_rows[g_set_selected];
  if (sel.is_bool) wcscpy(hint, L"A: toggle");
  else if (sel.is_choice) wcscpy(hint, L"LEFT/RIGHT: change");
  else if (sel.is_int) wcscpy(hint, L"UP/DOWN: change");
  else wcscpy(hint, L"keyboard: type a new value");
  const int max_rows = 12;
  int start = 0;
  if ((int)g_set_rows.size() > max_rows) {
    start = g_set_selected - 6;
    if (start < 0) start = 0;
    if (start + max_rows > (int)g_set_rows.size())
      start = (int)g_set_rows.size() - max_rows;
  }
  int shown = 0;
  for (int i = start;
       i < (int)g_set_rows.size() && shown < max_rows; ++i, ++shown) {
    const float ry = y0 + shown * row_h;
    if (i == g_set_selected) {
      D2D1_RECT_F sel{m - 12, ry - 3, m + g_w - 2 * m + 12, ry + row_h};
      g_ctx->FillRectangle(&sel, g_teal);
    }
    D2D1_COLOR_F onsel{0.05f, 0.07f, 0.086f, 1.0f};
    ID2D1SolidColorBrush* fg = g_dim;
    if (i == g_set_selected) g_ctx->CreateSolidColorBrush(onsel, &fg);
    text_out(g_text, m, ry, lab_w, row_h, g_set_rows[i].label,
             i == g_set_selected ? fg : g_brush);
    std::wstring val = g_set_rows[i].value;
    if (g_set_rows[i].is_bool) {
      val = (wcscmp(g_set_rows[i].value, L"1") == 0) ? L"ON" : L"OFF";
    }
    text_out(g_text, m + lab_w, ry, val_w, row_h, val.c_str(),
             i == g_set_selected ? fg : g_teal);
    if (i == g_set_selected) fg->Release();
  }
  text_out(g_small, m, g_h - m * 1.15f, g_w - 2 * m, g_h * 0.04f, hint,
           g_dim);
}

void draw_log_screen(float m, float y0) {
  const float row_h = g_h * 0.040f;
  const int max_rows = (int)((g_h - y0 - m * 1.2f) / row_h);
  int start = (int)g_log_tail.size() - max_rows;
  if (start < 0) start = 0;
  float y = y0;
  for (size_t i = (size_t)start; i < g_log_tail.size(); ++i) {
    text_out(g_small, m, y, g_w - 2 * m, row_h, g_log_tail[i].c_str(), g_dim);
    y += row_h;
  }
  if (g_log_tail.empty())
    text_out(g_text, m, y0, g_w - 2 * m, row_h * 2,
             L"Log is empty — boot activity lands here live", g_dim);
}

void draw_steam_screen(float m, float y0) {
  const float total_w = g_w - 2 * m;
  const float left_w = total_w * 0.52f;
  const float right_x = m + left_w + 24.0f;
  const float right_w = total_w - left_w - 24.0f;
  const float content_h = g_h * 0.68f;

  // Left Column: Steam Game Library & Downloads
  draw_panel(m, y0, left_w, content_h);
  text_out(g_small, m + 20, y0 + 16, left_w - 40, g_h * 0.03f,
           g_steam_signed_in ? L"YOUR STEAM LIBRARY" : L"STEAM LIBRARY (LOCAL)", g_teal);

  float row_y = y0 + g_h * 0.06f;
  const float row_h = g_h * 0.072f;
  const int max_visible = 7;
  int start = 0;
  if (static_cast<int>(g_steam_items.size()) > max_visible) {
    start = std::clamp(g_steam_selected - 3, 0,
                       static_cast<int>(g_steam_items.size()) - max_visible);
  }

  if (g_steam_items.empty()) {
    text_out(g_text, m + 24, row_y + 12, left_w - 48, g_h * 0.16f,
             g_steam_signed_in
                 ? L"No Steam games loaded yet.\nPress (X) to refresh library or transfer games via Setup."
                 : L"Sign in with Steam QR (press Y) to list your owned Steam games and download directly onto your Xbox.",
             g_dim);
  }

  for (int i = start;
       i < static_cast<int>(g_steam_items.size()) && (i - start) < max_visible;
       ++i) {
    const auto& item = g_steam_items[i];
    const bool is_sel = (i == g_steam_selected);

    draw_panel(m + 12, row_y, left_w - 24, row_h - 6, is_sel);

    // Title
    text_out(g_text, m + 24, row_y + 4, left_w * 0.62f, g_h * 0.035f,
             item.title.c_str(), is_sel ? g_teal : g_brush);

    // Status / Size
    std::wstring status_str;
    if (item.downloading) {
      wchar_t pctbuf[128];
      swprintf(pctbuf, 128, L"%d%% • %s",
               static_cast<int>(item.download_pct * 100.0f),
               item.status_str.empty() ? L"Downloading..." : item.status_str.c_str());
      status_str = pctbuf;
    } else if (!item.status_str.empty()) {
      status_str = item.status_str + L"  •  " + item.size_str;
    } else if (item.installed) {
      status_str = L"Ready to Play  •  " + item.size_str;
    } else {
      status_str = L"In Steam Account  •  " + item.size_str;
    }
    text_out(g_small, m + 24, row_y + g_h * 0.035f, left_w - 48, g_h * 0.025f,
             status_str.c_str(), item.downloading ? g_teal : g_dim);

    // Mini progress bar if downloading
    if (item.downloading) {
      const float p_w = left_w * 0.28f;
      const float p_h = 6.0f;
      const float p_x = m + left_w - p_w - 24.0f;
      const float p_y = row_y + 14.0f;
      D2D1_RECT_F pbg{p_x, p_y, p_x + p_w, p_y + p_h};
      D2D1_COLOR_F pbg_col{0.05f, 0.07f, 0.09f, 1.0f};
      ID2D1SolidColorBrush* pbbr = nullptr;
      g_ctx->CreateSolidColorBrush(pbg_col, &pbbr);
      g_ctx->FillRectangle(&pbg, pbbr);
      pbbr->Release();

      D2D1_RECT_F pfill{p_x, p_y, p_x + p_w * item.download_pct, p_y + p_h};
      g_ctx->FillRectangle(&pfill, g_teal);
    }

    row_y += row_h;
  }

  // Right Column: Steam Pairing & Selected Title Actions
  draw_panel(right_x, y0, right_w, content_h);
  wchar_t acct_hdr[160];
  if (g_steam_signed_in) {
    swprintf(acct_hdr, 160, L"SIGNED IN: %s (ID %llu)",
             g_steam_persona.empty() ? L"Steam Account" : g_steam_persona.c_str(),
             static_cast<unsigned long long>(g_steam_id64));
  } else {
    swprintf(acct_hdr, 160, L"STEAM ACCOUNT (UNLINKED)");
  }
  text_out(g_small, right_x + 20, y0 + 16, right_w - 40, g_h * 0.03f,
           acct_hdr, g_teal);

  if (g_steam_selected >= 0 &&
      g_steam_selected < static_cast<int>(g_steam_items.size())) {
    const auto& sel = g_steam_items[g_steam_selected];

    text_out(g_title, right_x + 20, y0 + g_h * 0.060f, right_w - 40,
             g_h * 0.075f, sel.title.c_str(), g_brush);

    wchar_t id_buf[96];
    swprintf(id_buf, 96, L"AppID %u  •  %s", sel.app_id, sel.size_str.c_str());
    text_out(g_small, right_x + 20, y0 + g_h * 0.135f, right_w - 40,
             g_h * 0.03f, id_buf, g_dim);

    // Action button card
    const float btn_y = y0 + g_h * 0.185f;
    const float btn_w = right_w - 40.0f;
    const float btn_h = g_h * 0.08f;
    draw_panel(right_x + 20, btn_y, btn_w, btn_h, true);

    std::wstring btn_text = L"[A] Launch on Xbox";
    if (sel.downloading) {
      btn_text = sel.status_str.empty() ? L"Downloading Steam depot..." : sel.status_str;
    } else if (!sel.installed) {
      btn_text = L"[A] Download from Steam to Xbox";
    }

    text_out(g_text, right_x + 36, btn_y + g_h * 0.022f, btn_w - 40,
             g_h * 0.04f, btn_text.c_str(), g_teal);
  }

  // QR Login / Status section at bottom of right panel
  const float qr_box_y = y0 + g_h * 0.30f;
  const float qr_box_h = content_h - g_h * 0.32f;
  draw_panel(right_x + 20, qr_box_y, right_w - 40, qr_box_h);

  text_out(g_small, right_x + 32, qr_box_y + 14, right_w - 64, g_h * 0.03f,
           L"STEAM STATUS & ACCOUNT LINK", g_teal);
  text_out(g_small, right_x + 32, qr_box_y + g_h * 0.05f, right_w - 64,
           g_h * 0.08f, g_steam_status_line.c_str(), g_brush);

  text_out(g_text, right_x + 32, qr_box_y + g_h * 0.14f, right_w - 64,
           g_h * 0.04f,
           g_steam_signed_in ? L"[Y] Re-link QR   |   [X] Refresh Purchases"
                             : L"[Y] Sign in with Steam QR Code",
           g_teal);

  draw_qr_overlay(m + 16);

  // Footer instructions
  text_out(g_small, m, g_h - m * 1.15f, total_w, g_h * 0.04f,
           L"D-Pad: Select   |   (A): Download / Play   |   (Y): Steam QR Login   |   (X): Refresh   |   LB/RB: Tab",
           g_dim);
}

void draw_lan_link(float m) {
  // the transfer URL, big enough to type from a laptop across the room
  if (g_qr_text.empty()) return;
  std::wstring url(g_qr_text.begin(), g_qr_text.end());
  text_out(g_small, m, g_h - m * 0.62f, g_w - 2 * m, g_h * 0.042f,
           (L"LAN LINK  " + url).c_str(), g_teal);
}

ui_click ui_hit_test(int x, int y) {
  const float m = g_h * 0.06f;
  const auto hits = [x, y](float left, float top, float width, float height) {
    return x >= left && y >= top && x < left + width && y < top + height;
  };
  if (g_first_run) {
    if (g_setup_finished && hits(m, g_h * 0.89f, (g_w - 3 * m) * 0.55f,
                                 g_h * 0.05f))
      return {ui_click_type::open_home, 0};
    return {ui_click_type::none, 0};
  }
  if (g_screen == UI_QUICKMENU) return {ui_click_type::none, 0};
  const float tab_w = g_w * 0.10f;
  for (int i = 0; i < UI_TAB_COUNT; ++i) {
    if (hits(m + i * (tab_w + g_w * 0.010f), g_h * 0.045f, tab_w, g_h * 0.05f))
      return {ui_click_type::tab, i};
  }
  if (g_screen == UI_SETTINGS) {
    const int count = static_cast<int>(g_set_rows.size());
    const int start = count > 12 ? std::clamp(g_set_selected - 6, 0, count - 12) : 0;
    const float row_h = g_h * 0.058f;
    for (int i = start; i < count && i < start + 12; ++i) {
      if (hits(m, g_h * 0.26f + (i - start) * row_h, g_w * 0.72f, row_h))
        return {ui_click_type::select_setting, i};
    }
  }
  if (g_screen == UI_SHELF && !g_games.empty()) {
    const float width = g_w - 2 * m - (g_qr_size > 0 ? g_h * 0.30f : 0);
    if (hits(m, g_h * 0.15f, width, g_h * 0.26f))
      return {ui_click_type::play, g_selected};
    const int start = g_selected >= 4 ? g_selected - 3 : 0;
    const float row_h = g_h * 0.057f;
    for (int i = start; i < static_cast<int>(g_games.size()) && i < start + 4; ++i) {
      if (hits(m, g_h * 0.492f + (i - start) * row_h, width, row_h - 7))
        return {ui_click_type::select_game, i};
    }
  }
  return {ui_click_type::none, 0};
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
    bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    bp.dpiX = 96.0f;
    bp.dpiY = 96.0f;
    if (FAILED(g_ctx->CreateBitmapFromDxgiSurface(surf, &bp, &bmp)) || !bmp) {
      surf->Release();
      if (old_target) old_target->Release();
      return;
    }
    g_ctx->SetTarget(bmp);
    g_ctx->BeginDraw();

    const float m = g_h * 0.06f;

    if (g_screen != UI_QUICKMENU)
      g_ctx->Clear(D2D1_COLOR_F{0.09f, 0.058f, 0.044f, 1.0f});
    if (g_screen != UI_QUICKMENU) draw_tabs(m);

    if ((g_first_run && !g_setup_steps.empty()) || g_screen == UI_SETUP) {
      const float y = g_h * 0.27f, h = g_h * 0.61f;
      const float left = (g_w - 3 * m) * 0.55f;
      const float right_x = 2 * m + left, right = g_w - right_x - m;
      text_out(g_title, m, g_h * 0.13f, g_w - 2 * m, g_h * 0.11f,
               L"Set up Tiramisu", g_brush);
      draw_panel(m, y, left, h);
      draw_panel(right_x, y, right, h);
      text_out(g_text, m + 24, y + 20, left - 48, 50, L"System check", g_teal);
      float row_y = y + g_h * 0.08f;
      for (size_t i = 0; i < g_setup_steps.size(); ++i) {
        const bool done = (int)i < g_setup_done_count;
        const std::wstring row = (done ? L"✓  " : L"·  ") + g_setup_steps[i];
        text_out(g_small, m + 28, row_y, left - 56, g_h * 0.05f,
                 row.c_str(), done ? g_brush : g_dim);
        row_y += g_h * 0.055f;
      }
      text_out(g_text, right_x + 24, y + 20, right - 48, 50,
               L"Transfer games", g_teal);
      text_out(g_small, right_x + 24, y + g_h * 0.085f, right - 48,
               g_h * 0.045f, g_transfer_url.empty() ? L"LAN address unavailable" :
               g_transfer_url.c_str(), g_brush);
      text_out(g_small, right_x + 24, y + g_h * 0.135f, right - 48,
               g_h * 0.09f, g_transfer_status.c_str(), g_dim);
      draw_qr_overlay(m + 20);
      if (g_setup_finished) {
        text_out(g_small, m, g_h * 0.89f, left, g_h * 0.05f,
                 g_first_run ? L"Enter / A  Open Home     F8  Cursor Mode"
                             : L"LB  Live Log    RB  Home     F8  Cursor Mode", g_teal);
      }
      g_ctx->EndDraw();
      bmp->Release();
      surf->Release();
      g_ctx->SetTarget(old_target);
      if (old_target) old_target->Release();
      return;
    }

    float y = g_h * 0.15f;
    if (g_screen == UI_SETTINGS || g_screen == UI_LOG || g_screen == UI_STEAM) {
      const wchar_t* screen_title = L"Settings";
      if (g_screen == UI_LOG) screen_title = L"Live Log";
      else if (g_screen == UI_STEAM) screen_title = L"Steam Launcher & Library";
      text_out(g_title, m, y, g_w - 2 * m, g_h * 0.10f, screen_title, g_brush);
      y += g_h * 0.10f;
    }

    if (g_screen == UI_STEAM) {
      draw_steam_screen(m, y + g_h * 0.01f);
      draw_lan_link(m);
      g_ctx->EndDraw();
      if (bmp) bmp->Release();
      surf->Release();
      g_ctx->SetTarget(old_target);
      if (old_target) old_target->Release();
      return;
    }

    if (g_screen == UI_SETTINGS) {
      draw_settings_screen(m, y + g_h * 0.01f);
      draw_qr_overlay(m);
      draw_lan_link(m);
      g_ctx->EndDraw();
      if (bmp) bmp->Release();
      surf->Release();
      g_ctx->SetTarget(old_target);
      if (old_target) old_target->Release();
      return;
    }
    if (g_screen == UI_QUICKMENU) {
      draw_quick_menu(m);
      g_ctx->EndDraw();
      if (bmp) bmp->Release();
      surf->Release();
      g_ctx->SetTarget(old_target);
      if (old_target) old_target->Release();
      return;
    }
    if (g_screen == UI_LOG) {
      draw_log_screen(m, y + g_h * 0.01f);
      draw_lan_link(m);
      g_ctx->EndDraw();
      if (bmp) bmp->Release();
      surf->Release();
      g_ctx->SetTarget(old_target);
      if (old_target) old_target->Release();
      return;
    }

    const float qr_space = g_qr_size > 0 ? g_h * 0.30f : 0.0f;
    const float width = g_w - 2 * m - qr_space;
    const float hero_h = g_h * 0.26f;
    const bool selected = g_selected >= 0 && g_selected < (int)g_games.size();
    draw_panel(m, y, width, hero_h, selected);
    text_out(g_small, m + 30, y + 24, width - 60, 42,
             selected ? L"SELECTED GAME" : L"TIRAMISU", g_teal);
    text_out(g_title, m + 30, y + g_h * 0.065f, width - 60, g_h * 0.095f,
             selected ? g_games[g_selected].name.c_str() : L"Your Xbox game library", g_brush);
    text_out(g_small, m + 30, y + g_h * 0.162f, width - 60, g_h * 0.04f,
             selected ? g_games[g_selected].exe.c_str() :
             L"Open Setup to transfer your first game.", g_dim);
    text_out(g_text, m + 30, y + g_h * 0.205f, width - 60, g_h * 0.045f,
             selected ? L"Click or press Enter to play      Left / Right  Choose executable" :
                        L"LB  Setup", g_teal);

    y += hero_h + g_h * 0.03f;
    text_out(g_text, m, y, width, g_h * 0.045f, L"Installed games", g_brush);
    y += g_h * 0.052f;
    const int total = (int)g_games.size();
    const int rows = 4;
    int start = g_selected >= rows ? g_selected - rows + 1 : 0;
    const float row_h = g_h * 0.057f;
    for (int i = start; i < total && i < start + rows; ++i) {
      draw_panel(m, y, width, row_h - 7, i == g_selected);
      text_out(g_text, m + 16, y + 3, width - 32, row_h - 10,
               g_games[i].name.c_str(), i == g_selected ? g_brush : g_dim);
      y += row_h;
    }
    if (total > rows) {
      const std::wstring position = std::to_wstring(g_selected + 1) + L" / " +
                                   std::to_wstring(total);
      text_out(g_small, m, y + 4, width, row_h, position.c_str(), g_dim);
    }

    const float status_y = g_h * 0.795f;
    for (size_t i = 0; i < g_lines.size() && i < 2; ++i)
      text_out(g_small, m, status_y + i * g_h * 0.036f, width, g_h * 0.036f,
               g_lines[i].c_str(), g_dim);
    text_out(g_small, m, g_h * 0.88f, width, g_h * 0.045f,
             L"Up/Down: Select   Tab: Screen   Enter: Play   F8: Cursor Mode   Y: Steam", g_dim);
    draw_qr_overlay(m);
    draw_lan_link(m);

    // FPS and system info overlays (when enabled in settings)
    if (longan::trt_settings_show_fps()) draw_fps_overlay();
    if (longan::trt_settings_show_sysinfo()) draw_sysinfo_overlay();

    g_ctx->EndDraw();
    if (bmp) bmp->Release();
    surf->Release();
    g_ctx->SetTarget(old_target);
    if (old_target) old_target->Release();
  }
}

} // namespace longan
