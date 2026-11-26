// Local Epic Games Launcher install metadata.
#pragma once
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#ifdef TIRAMISU_UWP_HOST
#include <winrt/Windows.Data.Json.h>
#endif

namespace longan {
struct epic_install {
  bool detected = false;
  std::string executable;
};

inline epic_install read_epic_install(const std::string& directory) {
  namespace fs = std::filesystem;
  epic_install result;
  std::error_code ec;
  const auto root = fs::u8path(directory);
  result.detected = fs::is_directory(root / ".egstore", ec);
#ifdef TIRAMISU_UWP_HOST
  // An exported Launcher .item file may be placed alongside the game.
  fs::directory_iterator it(root, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    if (it->path().extension() != L".item" || !it->is_regular_file(ec)) continue;
    const auto size = it->file_size(ec);
    if (ec || size == 0 || size > 1024 * 1024) continue;
    std::string bytes(static_cast<size_t>(size), '\0');
    std::ifstream file(it->path(), std::ios::binary);
    if (!file.read(bytes.data(), bytes.size())) continue;
    try {
      if (bytes.compare(0, 3, "\xef\xbb\xbf") == 0) bytes.erase(0, 3);
      auto item = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(bytes));
      if (item.GetNamedString(L"CatalogItemId", L"").empty() ||
          item.GetNamedString(L"AppName", L"").empty()) continue;
      result.detected = true;
      auto exe = winrt::to_string(item.GetNamedString(L"LaunchExecutable", L""));
      if (exe.empty()) continue;
      if (!result.executable.empty() && result.executable != exe) {
        result.executable.clear();
        break;
      }
      result.executable = exe;
    } catch (const winrt::hresult_error&) { continue; }
  }
#endif
  return result;
}

// Only an executable already found by the regular game scanner can be selected.
inline size_t epic_exe_index(const std::vector<std::string>& exes, std::string wanted) {
  auto normalize = [](std::string path) {
    for (auto& c : path) {
      if (c == '\\') c = '/';
      if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    return path;
  };
  wanted = normalize(wanted);
  for (size_t i = 0; i < exes.size(); ++i)
    if (normalize(exes[i]) == wanted) return i;
  return 0;
}
}
