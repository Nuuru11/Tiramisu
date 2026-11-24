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
  std::wstring engine_type;  // UNIVERSAL | Custom Native | Unity (IL2CPP) | ...
};

// which dashboard screen is up (host loop drives; controller/keyboard nav)
enum ui_screen {
  UI_SHELF = 0,    // game shelf + status
  UI_SETTINGS = 1, // settings editor
  UI_LOG = 2,      // live log tail
  UI_SETUP = 3,    // pairing and system checks
  UI_STEAM = 4,    // on-console Steam library & downloader
  UI_TAB_COUNT = 5,
  UI_QUICKMENU = UI_TAB_COUNT // in-game Steam social & quick overlay
};

struct ui_steam_item {
  std::wstring title;
  unsigned app_id;
  std::wstring size_str;
  bool installed;
  bool downloading;
  float download_pct;
  std::wstring status_str;
  std::wstring install_dir;
};

struct ui_steam_friend {
  std::wstring persona;
  int state;  // 0=Offline, 1=Online, 3=Away, 6=In-Game
  std::wstring game_name;
};

struct ui_steam_ach {
  std::wstring name;
  bool achieved;
  unsigned cur_progress;
  unsigned max_progress;
};

void ui_steam_nav_up();
void ui_steam_nav_down();
int ui_steam_selected_index();
void ui_set_steam_account(bool signed_in, uint64_t steamid64,
                          std::wstring persona, std::wstring status_line);
void ui_set_steam_items(const std::vector<ui_steam_item>& items);
void ui_set_steam_friends(const std::vector<ui_steam_friend>& friends);
void ui_set_steam_achievements(const std::vector<ui_steam_ach>& achs);
void ui_quick_menu_nav_up();
void ui_quick_menu_nav_down();
int ui_quick_menu_selected();

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
                  bool finished);
void ui_dismiss_first_run();
void ui_set_transfer(std::wstring url, std::wstring status);
void ui_set_control_hint(std::wstring hint);
void ui_set_screen(ui_screen s);                 // shelf | settings | log
void ui_set_settings(const ui_setting* rows, int count, int selected);
void ui_set_quick_menu(bool visible);            // RetroArch-style in-game menu
void ui_set_fps(float fps);                      // FPS counter for overlay
void ui_set_sysinfo(const std::wstring& info);   // CPU/GPU/RAM string
void ui_set_log_tail(const std::vector<std::wstring>& lines);
enum class ui_click_type { none, tab, open_home, play, select_game, select_setting };
struct ui_click { ui_click_type type; int index; };
ui_click ui_hit_test(int x, int y);
void ui_render();

} // namespace longan

#endif
