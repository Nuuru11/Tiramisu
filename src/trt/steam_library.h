// tiramisu/src/trt/steam_library.h
// Read the user's REAL Steam installation: libraryfolders.vdf -> library
// roots, then each root's steamapps/appmanifest_*.acf -> installed apps.
//
// Why parse instead of asking Steam: Tiramisu never talks to a Steam client
// or account for library data. The files Steam itself maintains are the
// source of truth, they are plain text, and reading them needs no secret.
// Nothing here authenticates or claims ownership; ownership comes from
// steam_auth (OpenID) and the app manifests Steam wrote for installed titles.
#ifndef TIRAMISU_STEAM_LIBRARY_H
#define TIRAMISU_STEAM_LIBRARY_H

#include <cstdint>
#include <string>
#include <vector>

// One installed (or partially installed) Steam app.
struct trt_steam_app {
  unsigned appid = 0;
  std::string name;
  std::string installdir;
  std::string library_path;  // the library root that owns it
  unsigned long long size_on_disk = 0;
  unsigned state_flags = 0;  // Steam's EAppState bits; 4 = fully installed
  bool fully_installed = false;
};

// A Steam library root plus every app manifest found under it.
struct trt_steam_library {
  std::vector<std::string> roots;
  std::vector<trt_steam_app> apps;
  // Named gaps: a root or manifest that could not be read/parsed. Never a
  // silent skip -- the setup page shows these verbatim.
  std::vector<std::string> notes;
};

// Candidate Steam install roots for this host. Missing ones are simply not
// reported; every candidate that exists is returned.
std::vector<std::string> trt_steam_default_roots();

// Path to a library's libraryfolders.vdf ("" if absent).
std::string trt_steam_libraryfolders_path(const std::string& root);

// ---- VDF text parsing (exposed so it can be tested without a Steam install)
// Parse libraryfolders.vdf text into the library paths it lists. Accepts both
// the modern form ("0" { "path" "D:\\SteamLibrary" }) and the legacy form
// ("1" "D:\\SteamLibrary"), and ignores non-path keys such as "contentstatsid".
// Returns the number of paths appended, or -1 when the text is not VDF.
int trt_steam_parse_library_paths(const std::string& text,
                                  std::vector<std::string>* out);

struct trt_steam_depot {
  uint32_t id;
  uint64_t manifest;
  uint64_t size;
};

// Public, base-game depots for Windows with neutral or English content.
std::vector<trt_steam_depot> trt_steam_family_depots(const std::string& appinfo);

// Parse one appmanifest_<id>.acf. Returns 0 on success, -1 when the text has no
// AppState block or no usable appid.
int trt_steam_parse_appmanifest(const std::string& text, trt_steam_app* out);

// Pick the launcher exe inside an install dir: the largest *.exe in the top
// level of installdir (Steam games keep their launcher there; shipping
// binaries live in subdirectories). Returns "" when none is found.
std::string trt_steam_find_exe(const std::string& installdir);

// Scan every existing default root. Returns apps found. `lib` receives roots,
// apps and notes; `lib` may be null.
int trt_steam_library_scan(trt_steam_library* lib);

// Scan an explicit list of roots (tests, and a user-supplied extra root).
int trt_steam_library_scan_roots(const std::vector<std::string>& roots,
                                 trt_steam_library* lib);

#endif  // TIRAMISU_STEAM_LIBRARY_H
