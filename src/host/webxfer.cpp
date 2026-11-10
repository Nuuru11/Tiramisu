// tiramisu/src/host/webxfer.cpp
// HTTP game-transfer server on raw ws2_32 (real DLL on console, SYSTEM route).
// Protocol kept deliberately simple so phone browsers need nothing special:
//   GET  /              -> phone upload page (dark, Tiramisu-styled)
//   PUT  /upload/<name> -> raw body streamed to LocalState\incoming\<name>;
//                          .zip gets extracted into games\<stem>\
//   GET  /games         -> JSON list (name, exes[])
//   POST /launch/<name> -> set launch flag; the host loop boots the guest
// Streams bodies straight to disk (games are GBs; nothing buffered in RAM).
#include "webxfer.h"
#include "host_log.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include "vendor/miniz.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace longan {

namespace {

webxfer_state g;
std::atomic<bool> g_stop{false};
SOCKET g_listener = INVALID_SOCKET;

std::string g_games_dir;    // LocalState\games
std::string g_incoming;     // LocalState\incoming

// ---------- filesystem helpers (CRT on the LocalState paths) ----------
bool dir_exists(const std::string& p) {
  DWORD a = GetFileAttributesA(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

void mkdirs(const std::string& p) {
  std::string cur;
  for (size_t i = 0; i <= p.size(); ++i) {
    if (i == p.size() || p[i] == '\\') {
      cur = p.substr(0, i);
      if (!cur.empty() && !dir_exists(cur)) CreateDirectoryA(cur.c_str(), nullptr);
    }
  }
}

bool read_file(const std::string& p, std::string* out) {
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return false;
  char buf[8192];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
  fclose(f);
  return true;
}

void list_dir(const std::string& dir, std::vector<std::string>* names) {
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    const std::string n = fd.cFileName;
    if (n != "." && n != "..") names->push_back(n);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
}

// primary exe = largest exe, folder-name match wins
std::string pick_exe(const std::string& game_dir, const std::string& game_name) {
  std::vector<std::string> files;
  list_dir(game_dir, &files);
  std::string best;
  uint64_t best_size = 0;
  for (const auto& f : files) {
    const size_t dot = f.rfind('.');
    if (dot == std::string::npos ||
        _stricmp(f.c_str() + dot, ".exe") != 0) continue;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    uint64_t sz = 0;
    if (GetFileAttributesExA((game_dir + "\\" + f).c_str(),
                             GetFileExInfoStandard, &fa))
      sz = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    const bool name_match = _stricmp(f.substr(0, dot).c_str(), game_name.c_str()) == 0;
    if (name_match || best_size == 0 || sz > best_size) {
      best = f;
      best_size = sz;
      if (name_match) break;
    }
  }
  return best;
}

// ---------- zip extraction (miniz) ----------
bool extract_zip(const std::string& zip_path, const std::string& dest,
                 std::string* note) {
  mz_zip_archive zip{};
  memset(&zip, 0, sizeof(zip));
  if (!mz_zip_reader_init_file(&zip, zip_path.c_str(), 0)) {
    *note = "not a valid zip";
    return false;
  }
  const int n = (int)mz_zip_reader_get_num_files(&zip);
  int extracted = 0;
  char safe_name[256];
  for (int i = 0; i < n; ++i) {
    mz_zip_archive_file_stat st{};
    if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
    if (st.m_is_directory) continue;
    // sanitize: no absolute parts, no ..
    const char* base = st.m_filename;
    while (*base == '/' || *base == '\\') ++base;
    if (strstr(base, "..")) continue;
    snprintf(safe_name, sizeof(safe_name), "%s", base);
    for (char* q = safe_name; *q; ++q) if (*q == '/') *q = '\\';
    const std::string out_path = dest + "\\" + safe_name;
    const size_t slash = out_path.rfind('\\');
    if (slash != std::string::npos) mkdirs(out_path.substr(0, slash));
    mz_zip_reader_extract_to_file(&zip, i, out_path.c_str(), 0);
    ++extracted;
  }
  mz_zip_reader_end(&zip);
  char tmp[128];
  snprintf(tmp, sizeof(tmp), "%d files", extracted);
  *note = tmp;
  return extracted > 0;
}

// ---------- game list + http plumbing ----------
std::string json_escape(const std::string& s) {
  std::string r;
  for (char c : s) {
    if (c == '"' || c == '\\') { r += '\\'; r += c; }
    else if ((uint8_t)c < 0x20) r += ' ';
    else r += c;
  }
  return r;
}

std::string games_json() {
  std::vector<std::string> dirs;
  list_dir(g_games_dir, &dirs);
  std::string j = "[";
  bool first = true;
  for (const auto& d : dirs) {
    if (!dir_exists(g_games_dir + "\\" + d)) continue;
    const std::string exe = pick_exe(g_games_dir + "\\" + d, d);
    if (exe.empty()) continue;
    if (!first) j += ",";
    first = false;
    j += "{\"name\":\"" + json_escape(d) + "\",\"exe\":\"" + json_escape(exe) + "\"}";
  }
  return j + "]";
}

const char* k_upload_page =
  "<!doctype html><html><head><meta charset=utf-8>"
  "<meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>Tiramisu</title><style>"
  "body{background:#0e1116;color:#e6e9ef;font-family:-apple-system,sans-serif;"
  "max-width:560px;margin:0 auto;padding:24px}"
  "h1{color:#20808d;font-weight:700;margin:0 0 4px}h2{font-size:15px;color:#8aa;}"
  "input[type=file]{width:100%;padding:14px;border:1px dashed #20808d;border-radius:10px;"
  "background:#131922;color:#9fb3b8;margin:14px 0}"
  "button{width:100%;padding:14px;border:0;border-radius:10px;background:#20808d;"
  "color:#fff;font-size:16px;font-weight:700}"
  "button:disabled{background:#2a3a3e}"
  ".g{display:flex;justify-content:space-between;align-items:center;"
  "padding:12px;border:1px solid #1d2733;border-radius:10px;margin:8px 0}"
  ".g b{color:#e6e9ef}.g span{color:#7d8c96;font-size:13px}"
  ".l{background:#1d4f57;color:#bfe9ef;font-size:13px;padding:8px 12px;"
  "border-radius:8px;font-weight:700}"
  "#bar{height:6px;background:#1d2733;border-radius:3px;overflow:hidden;margin:10px 0}"
  "#fill{height:100%;width:0;background:#20808d}"
  "#log{font-size:13px;color:#7d8c96;min-height:20px}</style></head><body>"
  "<h1>Tiramisu</h1><h2>Windows games on Xbox — phone transfer</h2>"
  "<input type=file id=f>"
  "<div id=bar><div id=fill></div></div>"
  "<button id=up onclick=go()>Upload game (.zip of the game folder)</button>"
  "<div id=log></div><hr style='border-color:#1d2733'>"
  "<h2 style=margin-top:18px>Games on this console</h2><div id=list>"
  "<span style=color:#556>loading…</span></div>"
  "<script>"
  "function L(){fetch('/games').then(r=>r.json()).then(g=>{const e=document.getElementById('list');"
  "e.innerHTML=g.length?'':'<span style=color:#556>none yet — upload a zip</span>';"
  "g.forEach(x=>{const d=document.createElement('div');d.className='g';"
  "d.innerHTML='<b>'+x.name+'</b><span class=l onclick=\"go2(\\''+x.name+'\\')\">LAUNCH ▶</span>';"
  "e.appendChild(d);});});}"
  "function go2(n){document.getElementById('log').textContent='launching '+n+' — look at the TV';"
  "fetch('/launch/'+encodeURIComponent(n),{method:'POST'}).then(L);}"
  "function go(){const f=document.getElementById('f').files[0];if(!f)return;"
  "const b=document.getElementById('up');b.disabled=1;"
  "const t=setInterval(()=>{fetch('/progress/'+encodeURIComponent(f.name)).then(r=>r.text()).then(t=>{"
  "document.getElementById('fill').style.width=Math.min(99,parseFloat(t)*100)+'%';});},800);"
  "fetch('/upload/'+encodeURIComponent(f.name),{method:'PUT',body:f})"
  ".then(r=>r.text()).then(s=>{clearInterval(t);"
  "document.getElementById('fill').style.width='100%';b.disabled=0;"
  "document.getElementById('log').textContent=s;L();});}"
  "L();</script></body></html>";

uint64_t g_upload_total = 0;   // bytes expected (current upload)
uint64_t g_upload_done = 0;    // bytes written so far

// stream N bytes from the socket to a file; returns bytes written
uint64_t stream_to_file(SOCKET s, const char* path, uint64_t len) {
  FILE* f = fopen(path, "wb");
  if (!f) return 0;
  char buf[65536];
  uint64_t got = 0;
  while (got < len && !g_stop) {
    const int want = (int)((len - got) < sizeof(buf) ? (len - got) : sizeof(buf));
    const int n = recv(s, buf, want, 0);
    if (n <= 0) break;
    fwrite(buf, 1, (size_t)n, f);
    got += (uint64_t)n;
    g_upload_done = got;
  }
  fclose(f);
  return got;
}

void respond(SOCKET s, const char* code, const char* ctype,
             const std::string& body) {
  char hdr[256];
  snprintf(hdr, sizeof(hdr),
           "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
           "Connection: close\r\n\r\n", code, ctype, body.size());
  send(s, hdr, (int)strlen(hdr), 0);
  send(s, body.c_str(), (int)body.size(), 0);
}


void handle(SOCKET s) {
  char req[8192];
  int rl = 0;
  // read until \r\n\r\n
  while (rl < (int)sizeof(req) - 1) {
    const int n = recv(s, req + rl, 1, 0);
    if (n <= 0) return;
    rl += n;
    if (rl > 4 && strncmp(req + rl - 4, "\r\n\r\n", 4) == 0) break;
  }
  req[rl] = 0;

  char method[16] = {}, path[2048] = {};
  uint64_t content_len = 0;
  sscanf(req, "%15s %2047s", method, path);
  for (const char* p = req; *p; ++p) {  // case-insensitive Content-Length
    if ((p[0] == 'C' || p[0] == 'c') &&
        _strnicmp(p, "content-length:", 15) == 0) {
      content_len = strtoull(p + 15, nullptr, 10);
      break;
    }
  }

  char line[256];
  std::string url_path = path;

  if (strcmp(method, "GET") == 0 && url_path.rfind("/progress/", 0) == 0) {
    snprintf(line, sizeof(line), "%f",
             g_upload_total ? (double)g_upload_done / (double)g_upload_total
                            : 0.0);
    respond(s, "200 OK", "text/plain", line);
    return;
  }
  if (strcmp(method, "GET") == 0 && url_path == "/games") {
    respond(s, "200 OK", "application/json", games_json());
    return;
  }
  if (strcmp(method, "GET") == 0 &&
      (url_path == "/" || url_path.rfind("/?", 0) == 0)) {
    respond(s, "200 OK", "text/html", k_upload_page);
    return;
  }
  if (strcmp(method, "POST") == 0 && url_path.rfind("/launch/", 0) == 0) {
    const std::string name = url_path.substr(8);
    g.launch_name = name;
    g.launch_requested = true;
    respond(s, "200 OK", "text/plain", "launching " + name);
    return;
  }
  if (strcmp(method, "PUT") == 0 && url_path.rfind("/upload/", 0) == 0) {
    std::string fname = url_path.substr(8);
    // sanitize
    for (auto& c : fname)
      if (c == '/' || c == '\\' || c == ':') c = '_';
    if (fname.find("..") != std::string::npos || fname.empty()) {
      respond(s, "400 Bad Request", "text/plain", "bad name");
      return;
    }
    const std::string tmp = g_incoming + "\\" + fname;
    g_upload_total = content_len;
    const uint64_t got = stream_to_file(s, tmp.c_str(), content_len);
    if (got != content_len) {
      remove(tmp.c_str());
      g.last_error = "upload incomplete";
      respond(s, "500 Server Error", "text/plain", "incomplete");
      return;
    }
    char mb[32];
    snprintf(mb, sizeof(mb), "%.1f MB", (double)got / 1048576.0);
    if (fname.size() > 4 && _stricmp(fname.c_str() + fname.size() - 4, ".zip") == 0) {
      const size_t stem = fname.rfind('.');
      std::string game_name = fname.substr(0, stem);
      const std::string dest = g_games_dir + "\\" + game_name;
      mkdirs(dest);
      std::string note;
      const bool ok = extract_zip(tmp, dest, &note);
      remove(tmp.c_str());
      if (ok) {
        g.last_upload = game_name + " (" + mb + ", " + note + ")";
        respond(s, "200 OK", "text/plain",
                "extracted: " + game_name + " — " + note +
                ". Hit LAUNCH, then look at the TV.");
      } else {
        g.last_error = "zip extract failed: " + note;
        respond(s, "500 Server Error", "text/plain", "extract failed");
      }
    } else {
      g.last_error = "only .zip supported (zip the game folder)";
      respond(s, "400 Bad Request", "text/plain",
              "zip the game folder and upload the .zip");
    }
    g_upload_total = 0;
    g_upload_done = 0;
    return;
  }
  respond(s, "404 Not Found", "text/plain", "nope");
}

void listener_thread() {
  while (!g_stop) {
    SOCKET c = accept(g_listener, nullptr, nullptr);
    if (c == INVALID_SOCKET) continue;
    handle(c);
    closesocket(c);
  }
}

} // namespace

std::string webxfer_start(const std::string& local_state, int display_w) {
  (void)display_w;
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    g.last_error = "WSAStartup failed";
    return {};
  }
  g_games_dir = local_state + "\\games";
  g_incoming = local_state + "\\incoming";
  mkdirs(g_games_dir);
  mkdirs(g_incoming);

  g_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (g_listener == INVALID_SOCKET) {
    g.last_error = "socket() failed";
    return {};
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(g.port);
  if (bind(g_listener, (sockaddr*)&addr, sizeof(addr)) != 0 ||
      listen(g_listener, 4) != 0) {
    g.last_error = "bind/listen failed (port taken?)";
    return {};
  }
  g_stop = false;
  std::thread(listener_thread).detach();
  g.running = true;

  // resolve a LAN IP for display (best-effort)
  char host[64] = "?";
  gethostname(host, sizeof(host));
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_INET;
  if (getaddrinfo(host, nullptr, &hints, &res) == 0 && res) {
    sockaddr_in* a = (sockaddr_in*)res->ai_addr;
    inet_ntop(AF_INET, &a->sin_addr, host, sizeof(host));
    freeaddrinfo(res);
  }
  g.url = std::string("http://") + host + ":" + std::to_string(g.port);
  char line[192];
  snprintf(line, sizeof(line), "webxfer: serving %s -> %s", g.url.c_str(),
           g_games_dir.c_str());
  log_line(line);
  return g.url;
}

bool webxfer_poll_launch(std::string* out_name) {
  if (!g.launch_requested) return false;
  g.launch_requested = false;
  if (out_name) *out_name = g.launch_name;
  return true;
}

void webxfer_shutdown() {
  g_stop = true;
  if (g_listener != INVALID_SOCKET) {
    closesocket(g_listener);
    g_listener = INVALID_SOCKET;
  }
  WSACleanup();
}

// expose upload status for the TV overlay
std::string webxfer_status_line() {
  if (!g.last_upload.empty()) return "xfer: " + g.last_upload;
  if (!g.last_error.empty()) return "xfer ERR: " + g.last_error;
  return g.running ? ("transfer: " + g.url) : "transfer: offline";
}

std::string webxfer_url() { return g.url; }

} // namespace longan
