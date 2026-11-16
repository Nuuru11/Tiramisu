// tiramisu/src/host/ui.h
#ifndef TIRAMISU_UI_H
#define TIRAMISU_UI_H

#include <string>
#include <vector>
#include "vendor/qrcodegen.h"

namespace longan {

struct ui_game {
  std::wstring name;
  std::wstring exe;
  std::wstring meta;
};

// which dashboard screen is up (host loop drives; controller/keyboard nav)
enum ui_screen {
  UI_SHELF = 0,    // game shelf + status
  UI_SETTINGS = 1, // settings editor
  UI_LOG = 2,      // live log tail
  UI_QUICKMENU = 3 // in-game quick menu (RetroArch-style)
};

// one editable setting row on the settings screen
struct ui_setting {
  wchar_t label[64];      // left column
  wchar_t value[64];      // editable value (utf-16, host keeps the buffer)
  bool is_bool;           // rendered as ON/OFF, A toggles
  bool is_choice;         // value is one of choices (LR cycles)
  bool is_int;            // numeric, up/down edits
};

void ui_init(unsigned width, unsigned height, void* d2d_factory_v,
             void* dwrite_factory_v, void* d2d_device_v, void* swapchain_v);
void ui_shutdown();
void ui_set_games(const std::vector<ui_game>& games, int selected);
void ui_set_lines(const std::vector<std::wstring>& lines);
void ui_set_qr(const std::string& qr_text);  // empty = hide QR
void ui_set_setup(const std::vector<std::wstring>& steps, int done_count,
                  bool finished);  // non-empty steps = setup screen on
void ui_set_screen(ui_screen s);                 // shelf | settings | log
void ui_set_settings(const ui_setting* rows, int count, int selected);
void ui_set_quick_menu(bool visible);            // RetroArch-style in-game menu
void ui_set_fps(float fps);                      // FPS counter for overlay
void ui_set_sysinfo(const std::wstring& info);   // CPU/GPU/RAM string
void ui_set_log_tail(const std::vector<std::wstring>& lines);
void ui_render();

} // namespace longan

#endif
