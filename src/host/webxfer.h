// tiramisu/src/host/webxfer.h
// Phone -> console game transfer: a tiny HTTP server inside the UWP app.
// Launch the app, scan the TV QR (or type the URL), upload a game ZIP from
// the phone, hit LAUNCH. File transfer is the primary game pipeline.
#ifndef TIRAMISU_WEBXFER_H
#define TIRAMISU_WEBXFER_H

#include <string>

namespace longan {

struct webxfer_state {
  bool running = false;
  unsigned short port = 9127;
  std::string url;            // http://<ip>:9127
  // launch handoff: worker thread sets name+flag; host loop consumes
  std::string launch_name;
  bool launch_requested = false;
  // status for the TV overlay
  std::string last_upload;    // "name (123 MB) -> 4 exe candidates"
  std::string last_error;
};

// start the listener; returns the URL for the TV/QR. Safe to call once.
std::string webxfer_start(const std::string& local_state, int display_w);

// poll from the host loop: consumes launch requests, refreshes status
bool webxfer_poll_launch(std::string* out_name);

// one-line status for the TV overlay ("transfer: http://…", last upload, err)
std::string webxfer_status_line();

// the URL shown in the QR (empty until started)
std::string webxfer_url();

void webxfer_shutdown();

} // namespace longan

#endif
