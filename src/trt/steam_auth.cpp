// tiramisu/src/trt/steam_auth.cpp
// Steam OpenID 2.0 sign-in. See steam_auth.h for the security model.
//
// No secret is ever handled here. The OpenID assertion is validated against
// Steam before anything is written, and is then discarded -- only the public
// SteamID survives.
#include "steam_auth.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#ifndef TIRAMISU_UWP_HOST
#include <winhttp.h>
#endif
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

const char* const TRT_STEAM_OPENID_ENDPOINT =
    "https://steamcommunity.com/openid/login";

namespace {

trt_steam_http_hook_t g_http_hook = nullptr;

bool starts_with(const std::string& s, const char* prefix) {
  const size_t n = strlen(prefix);
  return s.size() >= n && memcmp(s.data(), prefix, n) == 0;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool all_digits(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s)
    if (!is_digit(c)) return false;
  return true;
}

std::string json_escape_light(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if ((unsigned char)c < 0x20) {
      char b[8];
      snprintf(b, sizeof(b), "\\u%04x", (unsigned)(unsigned char)c);
      out += b;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

// Minimal JSON string/number reader for our own state file (no dependency).
bool json_field(const std::string& text, const char* key, std::string* out) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = text.find(needle);
  if (p == std::string::npos) return false;
  p = text.find(':', p + needle.size());
  if (p == std::string::npos) return false;
  ++p;
  while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
  if (p >= text.size()) return false;
  if (text[p] == '"') {
    ++p;
    std::string v;
    while (p < text.size() && text[p] != '"') {
      if (text[p] == '\\' && p + 1 < text.size()) {
        ++p;
        if (text[p] == 'n') v.push_back('\n');
        else v.push_back(text[p]);
        ++p;
        continue;
      }
      v.push_back(text[p++]);
    }
    *out = std::move(v);
    return true;
  }
  const size_t start = p;
  while (p < text.size() && text[p] != ',' && text[p] != '}' && text[p] != '\n')
    ++p;
  std::string v = text.substr(start, p - start);
  while (!v.empty() && (v.back() == ' ' || v.back() == '\r')) v.pop_back();
  *out = std::move(v);
  return true;
}

bool ensure_dir(const std::string& dir) {
  if (dir.empty()) return false;
#if defined(_WIN32) && !defined(TIRAMISU_UWP_HOST)
  CreateDirectoryA(dir.c_str(), nullptr);
  DWORD a = GetFileAttributesA(dir.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#elif defined(_WIN32)
  _mkdir(dir.c_str());
  struct _stat64 st{};
  return _stat64(dir.c_str(), &st) == 0 && (st.st_mode & _S_IFDIR);
#else
  mkdir(dir.c_str(), 0700);
  struct stat st;
  return stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

std::string join_path(const std::string& a, const char* b) {
  if (a.empty()) return b;
  const char last = a[a.size() - 1];
  if (last == '/' || last == '\\') return a + b;
#ifdef _WIN32
  return a + "\\" + b;
#else
  return a + "/" + b;
#endif
}

std::string read_small(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return {};
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  fclose(f);
  return out;
}

// 0600, atomically: write a sibling temp file, then rename over the target.
bool write_private(const std::string& path, const std::string& data) {
  const std::string tmp = path + ".tmp";
#ifdef _WIN32
  FILE* f = fopen(tmp.c_str(), "wb");
#else
  const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  FILE* f = (fd >= 0) ? fdopen(fd, "wb") : nullptr;
  if (!f && fd >= 0) close(fd);
#endif
  if (!f) return false;
  const size_t n = fwrite(data.data(), 1, data.size(), f);
  fflush(f);
#ifdef _WIN32
  _commit(_fileno(f));
#else
  fsync(fileno(f));
#endif
  fclose(f);
  if (n != data.size()) {
    remove(tmp.c_str());
    return false;
  }
#if defined(_WIN32) && !defined(TIRAMISU_UWP_HOST)
  return MoveFileExA(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#elif defined(_WIN32)
  remove(path.c_str());
  return rename(tmp.c_str(), path.c_str()) == 0;
#else
  return rename(tmp.c_str(), path.c_str()) == 0;
#endif
}

// ---- transport ------------------------------------------------------------

#if defined(_WIN32) && !defined(TIRAMISU_UWP_HOST)
bool winhttp_post(const std::string& url, const std::string& body,
                  std::string* out, std::string* err) {
  // Split https://host/path?query. The caller already restricted the URL to
  // Steam's endpoint, so a tiny parser is enough (and cannot be abused).
  if (!starts_with(url, "https://")) {
    if (err) *err = "refusing non-https url";
    return false;
  }
  const size_t host_begin = 8;
  const size_t slash = url.find('/', host_begin);
  std::string host = url.substr(host_begin, slash - host_begin);
  std::string path = slash == std::string::npos ? "/" : url.substr(slash);

  std::wstring whost(host.begin(), host.end());
  std::wstring wpath(path.begin(), path.end());

  HINTERNET ses = WinHttpOpen(L"Tiramisu/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!ses) {
    if (err) *err = "WinHttpOpen failed";
    return false;
  }
  WinHttpSetTimeouts(ses, 15000, 15000, 20000, 20000);
  bool ok = false;
  HINTERNET con = WinHttpConnect(ses, whost.c_str(),
                                 INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!con) {
    if (err) *err = "WinHttpConnect failed";
    WinHttpCloseHandle(ses);
    return false;
  }
  HINTERNET req = WinHttpOpenRequest(con, L"POST", wpath.c_str(), nullptr,
                                     WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     WINHTTP_FLAG_SECURE);
  if (req) {
    const wchar_t* hdr = L"Content-Type: application/x-www-form-urlencoded";
    if (WinHttpSendRequest(req, hdr, (DWORD)-1L, (LPVOID)body.data(),
                           (DWORD)body.size(), (DWORD)body.size(), 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
      std::string resp;
      DWORD avail = 0;
      do {
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(req, &chunk[0], avail, &read) || read == 0) break;
        resp.append(chunk.data(), read);
        if (resp.size() > (1u << 20)) break;  // a validation reply is ~40 bytes
      } while (true);
      *out = std::move(resp);
      ok = true;
    } else if (err) {
      *err = "WinHTTP request failed";
    }
    WinHttpCloseHandle(req);
  } else if (err) {
    *err = "WinHttpOpenRequest failed";
  }
  WinHttpCloseHandle(con);
  WinHttpCloseHandle(ses);
  return ok;
}
#elif defined(_WIN32)
bool winhttp_post(const std::string&, const std::string&, std::string*, std::string* err) {
  if (err) *err = "OpenID HTTP POST unavailable in UWP container (use QR session)";
  return false;
}
#else
// POSIX fallback: curl, body handed over on stdin via a private temp file so
// nothing user-visible (or shell-relevant) ever reaches a command line.
bool curl_post(const std::string& url, const std::string& body, std::string* out,
               std::string* err) {
  char tmpl[] = "/tmp/tiramisu-openid-XXXXXX";
  const int fd = mkstemp(tmpl);
  if (fd < 0) {
    if (err) *err = "mkstemp failed";
    return false;
  }
  const size_t n = write(fd, body.data(), body.size());
  close(fd);
  if (n != body.size()) {
    unlink(tmpl);
    if (err) *err = "temp write failed";
    return false;
  }
  const std::string cmd = std::string(
      "curl -sS --max-time 20 -H 'Content-Type: "
      "application/x-www-form-urlencoded' --data-binary @") +
      tmpl + " '" + url + "' 2>/dev/null";
  FILE* p = popen(cmd.c_str(), "r");
  bool ok = false;
  if (p) {
    std::string resp;
    char buf[2048];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), p)) > 0) resp.append(buf, got);
    const int rc = pclose(p);
    if (rc == 0) {
      *out = std::move(resp);
      ok = true;
    } else if (err) {
      *err = "curl exited non-zero";
    }
  } else if (err) {
    *err = "curl unavailable (install curl or use the Windows build)";
  }
  unlink(tmpl);
  return ok;
}
#endif

}  // namespace

// ---------------------------------------------------------------------------

void trt_steam_set_http_hook(trt_steam_http_hook_t hook) { g_http_hook = hook; }

std::string trt_steam_url_encode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back((char)c);
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 15]);
    }
  }
  return out;
}

std::string trt_steam_url_decode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') {
      out.push_back(' ');
    } else if (s[i] == '%' && i + 2 < s.size()) {
      auto hv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int hi = hv(s[i + 1]), lo = hv(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back((char)((hi << 4) | lo));
        i += 2;
      } else {
        out.push_back(s[i]);
      }
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

bool trt_steam_parse_query(const std::string& query,
                           std::map<std::string, std::string>* out,
                           std::string* err) {
  size_t i = 0;
  while (i < query.size()) {
    size_t amp = query.find('&', i);
    const std::string pair =
        query.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
    if (!pair.empty()) {
      const size_t eq = pair.find('=');
      const std::string k = trt_steam_url_decode(pair.substr(0, eq));
      const std::string v = (eq == std::string::npos)
                                ? std::string()
                                : trt_steam_url_decode(pair.substr(eq + 1));
      // A repeated openid.* field means the assertion was tampered with.
      if (out->count(k)) {
        if (err) *err = "duplicate parameter: " + k;
        return false;
      }
      (*out)[k] = v;
    }
    if (amp == std::string::npos) break;
    i = amp + 1;
  }
  return true;
}

std::string trt_steam_login_url(const std::string& return_to,
                                const std::string& realm) {
  const std::string id_select =
      "http://specs.openid.net/auth/2.0/identifier_select";
  std::string url = std::string(TRT_STEAM_OPENID_ENDPOINT) +
                    "?openid.ns=" +
                    trt_steam_url_encode("http://specs.openid.net/auth/2.0") +
                    "&openid.mode=checkid_setup" +
                    "&openid.return_to=" + trt_steam_url_encode(return_to) +
                    "&openid.realm=" + trt_steam_url_encode(realm) +
                    "&openid.identity=" + trt_steam_url_encode(id_select) +
                    "&openid.claimed_id=" + trt_steam_url_encode(id_select);
  return url;
}

int trt_steam_parse_assertion(const std::string& query,
                              std::map<std::string, std::string>* params,
                              std::string* err) {
  static const char* required[] = {
      "openid.ns",          "openid.mode",          "openid.op_endpoint",
      "openid.claimed_id",  "openid.identity",      "openid.return_to",
      "openid.response_nonce", "openid.assoc_handle", "openid.signed",
      "openid.sig"};
  if (!trt_steam_parse_query(query, params, err)) return -1;
  for (const char* k : required) {
    if (!params->count(k) || (*params)[k].empty()) {
      if (err) *err = std::string("assertion missing ") + k;
      return -1;
    }
  }
  if ((*params)["openid.mode"] != "id_res") {
    if (err) *err = "assertion mode is not id_res";
    return -1;
  }
  if ((*params)["openid.op_endpoint"] != TRT_STEAM_OPENID_ENDPOINT) {
    if (err) *err = "assertion op_endpoint is not Steam's";
    return -1;
  }
  if ((*params)["openid.identity"] != (*params)["openid.claimed_id"]) {
    if (err) *err = "assertion identity/claimed_id disagree";
    return -1;
  }
  std::string steamid;
  if (!trt_steam_claimed_id_to_steam64((*params)["openid.claimed_id"], &steamid)) {
    if (err) *err = "assertion claimed_id is not a SteamID";
    return -1;
  }
  return 0;
}

bool trt_steam_claimed_id_to_steam64(const std::string& claimed_id,
                                     std::string* out) {
  static const char* kPrefix = "https://steamcommunity.com/openid/id/";
  if (!starts_with(claimed_id, kPrefix)) return false;
  const std::string id = claimed_id.substr(strlen(kPrefix));
  if (id.size() != 17 || !all_digits(id)) return false;
  const unsigned long long v = strtoull(id.c_str(), nullptr, 10);
  // SteamID64 layout: universe (bits 56-63), type (52-55), instance (32-51),
  // account id (0-31). Steam only issues universe 1, type 1 (individual).
  if (((v >> 56) & 0xFFull) != 1) return false;
  if (((v >> 52) & 0xFull) != 1) return false;
  *out = id;
  return true;
}

std::string trt_steam_validation_body(
    const std::map<std::string, std::string>& params) {
  std::string body;
  for (const auto& kv : params) {
    if (kv.first.rfind("openid.", 0) != 0) continue;
    const char* value = kv.first == "openid.mode" ? "check_authentication"
                                                  : kv.second.c_str();
    if (!body.empty()) body.push_back('&');
    body += trt_steam_url_encode(kv.first);
    body.push_back('=');
    body += trt_steam_url_encode(value);
  }
  // Never echo an assertion we did not fully see (defensive, not reachable
  // through parse_assertion which requires all fields).
  if (body.find("openid.mode=check_authentication") == std::string::npos) {
    if (!body.empty()) body.push_back('&');
    body += "openid.mode=check_authentication";
  }
  return body;
}

bool trt_steam_validation_ok(const std::string& response) {
  size_t p = 0;
  while ((p = response.find("is_valid:", p)) != std::string::npos) {
    p += strlen("is_valid:");
    while (p < response.size() && (response[p] == ' ' || response[p] == '\t'))
      ++p;
    if (response.compare(p, 4, "true") == 0) return true;
  }
  return false;
}

bool trt_steam_http_post(const std::string& url, const std::string& body,
                         std::string* out, std::string* err) {
  if (!starts_with(url, TRT_STEAM_OPENID_ENDPOINT)) {
    if (err) *err = "refusing to post to a non-Steam endpoint";
    return false;
  }
  if (g_http_hook) return g_http_hook(url, body, out, err);
#ifdef _WIN32
  return winhttp_post(url, body, out, err);
#else
  return curl_post(url, body, out, err);
#endif
}

// ---- local identity store -------------------------------------------------

std::string trt_steam_state_path(const std::string& state_dir) {
  return join_path(state_dir, "steam_identity.json");
}

bool trt_steam_auth_load(const std::string& state_dir, trt_steam_identity* out) {
  if (!out) return false;
  trt_steam_identity id;
  const std::string text = read_small(trt_steam_state_path(state_dir));
  if (text.empty()) return false;
  std::string steamid, persona, method, when;
  if (!json_field(text, "steamid64", &steamid) || steamid.empty()) return false;
  std::string check;
  if (!trt_steam_claimed_id_to_steam64(
          "https://steamcommunity.com/openid/id/" + steamid, &check))
    return false;  // a hand-edited file must not put a bogus id in play
  json_field(text, "persona", &persona);
  json_field(text, "method", &method);
  json_field(text, "signed_in_at", &when);
  id.steamid64 = steamid;
  id.persona = persona;
  id.method = method.empty() ? "openid2" : method;
  id.signed_in_at = when.empty() ? 0 : atoll(when.c_str());
  id.signed_in = true;
  *out = std::move(id);
  return true;
}

bool trt_steam_auth_save(const std::string& state_dir,
                         const trt_steam_identity& id) {
  if (id.steamid64.empty()) return false;
  if (!ensure_dir(state_dir)) return false;
  if (id.signed_in_at == 0) {
    // Caller may leave it unset; stamp it here rather than persisting zero.
    trt_steam_identity tmp = id;
    tmp.signed_in_at = (long long)time(nullptr);
    return trt_steam_auth_save(state_dir, tmp);
  }
  char buf[512];
  snprintf(buf, sizeof(buf),
           "{\n  \"steamid64\": \"%s\",\n  \"persona\": \"%s\",\n"
           "  \"method\": \"%s\",\n  \"signed_in_at\": %lld\n}\n",
           json_escape_light(id.steamid64).c_str(),
           json_escape_light(id.persona).c_str(),
           json_escape_light(id.method.empty() ? "openid2" : id.method).c_str(),
           (long long)id.signed_in_at);
  return write_private(trt_steam_state_path(state_dir), buf);
}

bool trt_steam_auth_sign_out(const std::string& state_dir) {
  return remove(trt_steam_state_path(state_dir).c_str()) == 0;
}

int trt_steam_auth_complete(const std::string& state_dir,
                            const std::string& query, trt_steam_identity* out,
                            std::string* err) {
  std::map<std::string, std::string> params;
  if (trt_steam_parse_assertion(query, &params, err) != 0) return -1;

  const std::string body = trt_steam_validation_body(params);
  std::string response;
  std::string terr;
  if (!trt_steam_http_post(TRT_STEAM_OPENID_ENDPOINT, body, &response, &terr)) {
    if (err) *err = "Steam validation failed: " + terr;
    return -1;
  }
  if (!trt_steam_validation_ok(response)) {
    if (err) *err = "Steam rejected the assertion (is_valid was not true)";
    return -1;
  }
  std::string steamid;
  if (!trt_steam_claimed_id_to_steam64(params["openid.claimed_id"], &steamid)) {
    if (err) *err = "validated assertion has no SteamID";
    return -1;
  }
  trt_steam_identity id;
  id.steamid64 = steamid;
  id.method = "openid2";
  id.signed_in_at = (long long)time(nullptr);
  id.signed_in = true;
  if (!trt_steam_auth_save(state_dir, id)) {
    if (err) *err = "could not persist the identity (state dir unwritable)";
    return -1;
  }
  if (out) *out = id;
  return 0;
}
