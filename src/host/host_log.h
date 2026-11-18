// tiramisu/src/host/host_log.h
// LocalState logger — on console there is no ETW and no stderr; a log file
// in LocalState (plus Device Portal file pull) is the ONLY diagnosis path.
#ifndef TIRAMISU_HOST_LOG_H
#define TIRAMISU_HOST_LOG_H

#include <cstddef>
#include <string>

namespace longan {

// resolve LocalState once and open the log file (append). Returns the
// LocalState directory (UTF-8) for later use (drive_c mount), empty on fail.
std::string log_open(const std::string& package_path);
void log_line(const char* line);
// Read the last bytes of the on-disk log (tiramisu.log) — evidence that
// survives a crash. Returns the byte count written to out (0 = no file).
size_t log_read_disk_tail(char* out, size_t cap);
void log_close();

// In-memory ring of recent log lines (UI dashboard + web /api/logs use it).
// Lines longer than 220 chars are truncated; oldest fall off at the cap.
void log_get_recent(char* out, size_t cap);
unsigned log_recent_count();

} // namespace longan

#endif
