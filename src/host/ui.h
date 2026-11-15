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

void ui_init(unsigned width, unsigned height, void* d2d_factory_v,
             void* dwrite_factory_v, void* d2d_device_v, void* swapchain_v);
void ui_shutdown();
void ui_set_games(const std::vector<ui_game>& games, int selected);
void ui_set_lines(const std::vector<std::wstring>& lines);
void ui_set_qr(const std::string& qr_text);  // empty = hide QR
void ui_set_setup(const std::vector<std::wstring>& steps, int done_count,
                  bool finished);  // non-empty steps = setup screen on
void ui_render();

} // namespace longan

#endif
