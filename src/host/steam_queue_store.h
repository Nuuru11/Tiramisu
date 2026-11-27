#pragma once
#include "steam_client.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>

namespace longan::steam_queue_store {

// Called with the Steam state mutex held. Account IDs and queue order are
// sufficient to restore jobs. Tickets and CDN credentials are session-local.
inline std::string restart_state(const std::string& state) {
  if (state == "running") return "queued";
  if (state == "holding") return "paused";
  return state;
}

inline bool save(const std::filesystem::path& file, const std::vector<steam_queue_entry>& jobs) {
  if (jobs.size() > 65536) return false;
  auto temp = file;
  temp += ".tmp";
  std::ofstream out(temp, std::ios::binary | std::ios::trunc);
  out << "TQ1 " << jobs.size() << '\n';
  for (const auto& job : jobs)
    out << job.app_id << ' ' << job.family << ' ' << restart_state(job.state) << ' '
        << std::quoted(job.title) << ' ' << std::quoted(job.detail) << '\n';
  const auto bytes = out.tellp();
  out.close();
  std::error_code ec;
  if (out && bytes >= 0 && bytes <= 16 * 1024 * 1024) {
    std::filesystem::rename(temp, file, ec);
    if (!ec) return true;
  }
  std::filesystem::remove(temp, ec);
  return false;
}

inline bool load(const std::filesystem::path& file, std::vector<steam_queue_entry>& jobs) {
  std::error_code ec;
  if (!std::filesystem::exists(file, ec)) return !ec;
  if (std::filesystem::file_size(file, ec) > 16 * 1024 * 1024 || ec) return false;
  std::ifstream in(file, std::ios::binary);
  std::string version;
  size_t count;
  if (!(in >> version >> count) || version != "TQ1" || count > 65536) return false;
  std::vector<steam_queue_entry> loaded;
  std::set<uint32_t> ids;
  for (size_t i = 0; i < count; ++i) {
    steam_queue_entry job;
    if (!(in >> job.app_id >> job.family >> job.state >> std::quoted(job.title) >> std::quoted(job.detail)) ||
        !job.app_id || !ids.insert(job.app_id).second ||
        (job.state != "queued" && job.state != "paused" && job.state != "confirm" && job.state != "failed"))
      return false;
    // A previous size confirmation cannot authorize a new manifest/space plan.
    if (job.state == "confirm") {
      job.state = "paused";
      job.detail = "Resume to check current storage requirements";
    }
    loaded.push_back(std::move(job));
  }
  in >> std::ws;
  if (!in.eof()) return false;
  jobs.swap(loaded);
  return true;
}
}
