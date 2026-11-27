// tiramisu/src/host/crash_report.cpp
// One-file crash bundles (see crash_report.h).
#include "crash_report.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace longan {
namespace {

#if defined(_WIN32)
constexpr const char* k_sep = "\\";
#else
constexpr const char* k_sep = "/";
#endif

bool make_dirs(const std::string& path) {
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  return !ec;
}

// Report file names are report-<n>.txt with n starting at 1; -1 for anything
// else so unrelated files in the folder are left alone.
int report_index(const std::string& name) {
  if (name.size() < 12) return -1;
  if (name.compare(0, 7, "report-") != 0) return -1;
  if (name.compare(name.size() - 4, 4, ".txt") != 0) return -1;
  if (name[7] == '0') return -1;
  int n = 0;
  for (size_t i = 7; i + 4 < name.size(); ++i) {
    if (name[i] < '0' || name[i] > '9') return -1;
    n = n * 10 + (name[i] - '0');
    if (n > 100000) return -1;
  }
  return n;
}

std::vector<int> report_indices(const std::string& dir) {
  std::vector<int> out;
#if defined(_WIN32)
  const std::string pattern = dir + k_sep + "report-*.txt";
  struct _finddata_t fd;
  intptr_t h = _findfirst(pattern.c_str(), &fd);
  if (h == -1) return out;
  do {
    if (fd.attrib & _A_SUBDIR) continue;
    const int n = report_index(fd.name);
    if (n > 0) out.push_back(n);
  } while (_findnext(h, &fd) == 0);
  _findclose(h);
#else
  DIR* d = opendir(dir.c_str());
  if (!d) return out;
  while (struct dirent* e = readdir(d)) {
    const int n = report_index(e->d_name);
    if (n <= 0) continue;
    struct stat info{};
    if (lstat((dir + k_sep + e->d_name).c_str(), &info) == 0 && S_ISREG(info.st_mode))
      out.push_back(n);
  }
  closedir(d);
#endif
  std::sort(out.begin(), out.end());
  return out;
}

std::string report_path(const std::string& local_state, int index) {
  char name[32];
  snprintf(name, sizeof(name), "report-%d.txt", index);
  return crash_report_dir(local_state) + k_sep + name;
}

// Sections are appended in a fixed order so two reports from different builds
// can be diffed; empty sections say so instead of vanishing.
void section(std::string* out, const char* title, const std::string& body,
             const char* empty) {
  *out += "---- ";
  *out += title;
  *out += " ----\n";
  if (body.empty()) {
    *out += empty;
    *out += "\n";
  } else {
    *out += body;
    if (body[body.size() - 1] != '\n') *out += '\n';
  }
}

}  // namespace

std::string crash_report_dir(const std::string& local_state) {
  if (local_state.empty()) return std::string();
  return local_state + k_sep + "crash";
}

std::string crash_report_write(const std::string& local_state,
                               const crash_report_input& in, int keep) {
  // A second fault must not truncate a report while it is being published.
  static std::mutex writer;
  std::unique_lock<std::mutex> lock(writer, std::try_to_lock);
  if (!lock.owns_lock()) return {};
  const std::string dir = crash_report_dir(local_state);
  if (dir.empty()) return std::string();
  if (!make_dirs(dir)) return std::string();

  const std::vector<int> existing = report_indices(dir);
  const int index = existing.empty() ? 1 : existing.back() + 1;
  const std::string path = report_path(local_state, index);

  std::string text = "tiramisu crash report\n";
  section(&text, "fault", in.reason, "(no reason recorded)");
  section(&text, "registers and stack", in.detail, "(no context captured)");
  section(&text, "settings in force", in.settings_ini, "(settings unavailable)");
  section(&text, "shader census", in.census_json, "(no shaders translated)");
  section(&text, "log tail", in.log_tail, "(log unavailable)");

  const std::string tmp = path + ".tmp";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) return std::string();
  const size_t wrote = fwrite(text.data(), 1, text.size(), f);
  const bool closed = fclose(f) == 0;
  if (!closed || wrote != text.size() || rename(tmp.c_str(), path.c_str()) != 0) {
    remove(tmp.c_str());
    return std::string();
  }
  crash_report_trim(local_state, keep);
  return path;
}

std::vector<int> crash_report_ids(const std::string& local_state) {
  if (local_state.empty()) return {};
  auto ids = report_indices(crash_report_dir(local_state));
  std::reverse(ids.begin(), ids.end());
  return ids;
}

int crash_report_count(const std::string& local_state) {
  const std::string dir = crash_report_dir(local_state);
  if (dir.empty()) return 0;
  return (int)report_indices(dir).size();
}

int crash_report_trim(const std::string& local_state, int keep) {
  const std::string dir = crash_report_dir(local_state);
  if (dir.empty()) return 0;
  if (keep < 1) keep = 1;
  const std::vector<int> all = report_indices(dir);
  if ((int)all.size() <= keep) return 0;
  int removed = 0;
  const size_t drop = all.size() - (size_t)keep;
  for (size_t i = 0; i < drop; ++i) {
    const std::string p = report_path(local_state, all[i]);
    if (remove(p.c_str()) == 0) ++removed;
  }
  return removed;
}

}  // namespace longan
