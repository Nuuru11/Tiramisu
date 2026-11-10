// tiramisu/src/host/host_log.h
// LocalState logger — on console there is no ETW and no stderr; a log file
// in LocalState (plus Device Portal file pull) is the ONLY diagnosis path.
#ifndef TIRAMISU_HOST_LOG_H
#define TIRAMISU_HOST_LOG_H

#include <string>

namespace tiramisu {

// resolve LocalState once and open the log file (append). Returns the
// LocalState directory (UTF-8) for later use (drive_c mount), empty on fail.
std::string log_open(const std::string& package_path);
void log_line(const char* line);
void log_close();

} // namespace tiramisu

#endif
