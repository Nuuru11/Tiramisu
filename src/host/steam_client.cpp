// tiramisu/src/host/steam_client.cpp
// Native Xbox UWP Steam Client + CM/CDN Depot Downloader.
#include "steam_client.h"
#include "steam_file_verify.h"
#include "host_log.h"
#include "../trt/bridge_steam.h"
#include "../trt/steam_auth.h"
#include "../trt/steam_library.h"
#include "../trt/engine_synthesizer.h"
#include "../trt/pc2xb_translator.h"
#include "vendor/miniz.h"
#include "../../third_party/zstd.h"

#include <windows.h>
#include <bcrypt.h>

#include <winrt/base.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/windows.networking.sockets.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace longan {

namespace {

std::mutex g_mu;
std::string g_state_dir;
std::string g_games_dir;
steam_account_snapshot g_acct;
std::string g_refresh_token;
std::string g_access_token;
std::vector<steam_library_item> g_items;
std::vector<steam_library_item> g_family_items;
std::atomic<bool> g_auth_busy{false};
std::atomic<bool> g_dl_busy{false};

// Accessed only by the worker holding g_dl_busy.
std::string pending_space_plan;
std::filesystem::path pending_removal;
std::chrono::steady_clock::time_point removal_deadline;

// ---- UTF helpers ----
std::wstring u8to16(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  if (n <= 0) return std::wstring(s.begin(), s.end());
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
  return w;
}

std::string u16to8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0,
                              nullptr, nullptr);
  if (n <= 0) {
    std::string out;
    for (wchar_t c : w) out.push_back((c >= 0x20 && c < 0x7F) ? (char)c : '?');
    return out;
  }
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr,
                      nullptr);
  return s;
}

std::string sanitize_dirname(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.') {
      out.push_back((char)c);
    }
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  return out.empty() ? "SteamApp" : out;
}

std::string format_bytes(uint64_t b) {
  char buf[64];
  if (b >= (1ULL << 30)) {
    snprintf(buf, sizeof(buf), "%.1f GB", (double)b / (double)(1ULL << 30));
  } else if (b >= (1ULL << 20)) {
    snprintf(buf, sizeof(buf), "%.1f MB", (double)b / (double)(1ULL << 20));
  } else if (b >= 1024) {
    snprintf(buf, sizeof(buf), "%.0f KB", (double)b / 1024.0);
  } else if (b > 0) {
    snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)b);
  } else {
    return "Steam";
  }
  return buf;
}

void ensure_dir_recursive(const std::string& path) {
  std::string cur;
  for (size_t i = 0; i < path.size(); ++i) {
    char c = path[i];
    cur.push_back(c);
    if ((c == '\\' || c == '/') && cur.size() > 3) {
      CreateDirectoryA(cur.c_str(), nullptr);
    }
  }
  if (!cur.empty()) CreateDirectoryA(cur.c_str(), nullptr);
}

// Steam depot filenames are relative Windows paths. A family depot must
// stay inside its staging directory, including when opened through the CRT.
bool valid_family_depot_path(const std::string& name) {
  size_t start = 0;
  do {
    const size_t end = name.find_first_of("\\/", start);
    const auto part = name.substr(start, end - start);
    if (part.empty() || part.back() == '.' || part.back() == ' ') return false;
    for (unsigned char ch : part) {
      if (ch < 32 || strchr(":*?\"<>|", ch)) return false;
    }
    std::string base = part.substr(0, part.find('.'));
    while (!base.empty() && base.back() == ' ') base.pop_back();
    for (char& ch : base) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
        (base.size() == 4 && (base.compare(0, 3, "COM") == 0 ||
                              base.compare(0, 3, "LPT") == 0) &&
         base[3] >= '0' && base[3] <= '9'))
      return false;
    if (end == std::string::npos) return true;
    start = end + 1;
  } while (start <= name.size());
  return false;
}

// ---- Base64 + Base64URL ----
int b64_val(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}

std::string b64_encode(const uint8_t* data, size_t len) {
  static const char k_tbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  for (size_t i = 0; i < len; i += 3) {
    const uint32_t a = data[i];
    const uint32_t b = (i + 1 < len) ? data[i + 1] : 0;
    const uint32_t c = (i + 2 < len) ? data[i + 2] : 0;
    const uint32_t tri = (a << 16) | (b << 8) | c;
    out.push_back(k_tbl[(tri >> 18) & 0x3F]);
    out.push_back(k_tbl[(tri >> 12) & 0x3F]);
    out.push_back((i + 1 < len) ? k_tbl[(tri >> 6) & 0x3F] : '=');
    out.push_back((i + 2 < len) ? k_tbl[tri & 0x3F] : '=');
  }
  return out;
}

std::vector<uint8_t> b64_decode(const std::string& in) {
  std::vector<uint8_t> out;
  uint32_t val = 0;
  int valb = -8;
  for (unsigned char c : in) {
    if (c == '=' || std::isspace(c)) continue;
    int d = b64_val(c);
    if (d < 0) continue;
    val = (val << 6) + (uint32_t)d;
    valb += 6;
    if (valb >= 0) {
      out.push_back((uint8_t)((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  return out;
}

uint64_t extract_steamid_from_jwt(const std::string& jwt) {
  const size_t p1 = jwt.find('.');
  if (p1 == std::string::npos) return 0;
  const size_t p2 = jwt.find('.', p1 + 1);
  if (p2 == std::string::npos) return 0;
  auto raw = b64_decode(jwt.substr(p1 + 1, p2 - p1 - 1));
  std::string payload(raw.begin(), raw.end());
  try {
    auto obj = winrt::Windows::Data::Json::JsonObject::Parse(
        winrt::hstring(u8to16(payload)));
    if (obj.HasKey(L"sub")) {
      std::string sub = u16to8(std::wstring(obj.GetNamedString(L"sub").c_str()));
      return strtoull(sub.c_str(), nullptr, 10);
    }
  } catch (...) {
  }
  return 0;
}

// ---- Token file persistence ----
std::string token_file_path(const std::string& state_dir) {
  return state_dir + "\\steam.token";
}

void save_credentials(const std::string& state_dir, uint64_t steamid64,
                      const std::string& acct_name, const std::string& persona,
                      const std::string& refresh_tok,
                      const std::string& access_tok) {
  winrt::Windows::Data::Json::JsonObject obj;
  obj.Insert(L"steamid64", winrt::Windows::Data::Json::JsonValue::CreateStringValue(
                               u8to16(std::to_string(steamid64))));
  obj.Insert(L"account_name", winrt::Windows::Data::Json::JsonValue::CreateStringValue(
                                  u8to16(acct_name)));
  obj.Insert(L"persona", winrt::Windows::Data::Json::JsonValue::CreateStringValue(
                             u8to16(persona.empty() ? acct_name : persona)));
  obj.Insert(L"refresh_token", winrt::Windows::Data::Json::JsonValue::CreateStringValue(
                                   u8to16(refresh_tok)));
  obj.Insert(L"access_token", winrt::Windows::Data::Json::JsonValue::CreateStringValue(
                                  u8to16(access_tok)));
  std::string json_u8 = u16to8(std::wstring(obj.Stringify().c_str()));
  FILE* fp = fopen(token_file_path(state_dir).c_str(), "wb");
  if (fp) {
    fwrite(json_u8.data(), 1, json_u8.size(), fp);
    fclose(fp);
  }
  trt_steam_identity id;
  id.steamid64 = std::to_string(steamid64);
  id.persona = persona.empty() ? acct_name : persona;
  id.method = "steam_qr";
  id.signed_in = (steamid64 != 0);
  trt_steam_auth_save(state_dir, id);
  if (id.signed_in) {
    trt_steam_set_identity(id.steamid64.c_str(), id.persona.c_str());
  }
}

// ---- WinRT HTTP helpers ----
std::wstring uri_escape(const std::wstring& s) {
  try {
    return std::wstring(
        winrt::Windows::Foundation::Uri::EscapeComponent(s).c_str());
  } catch (...) {
    return s;
  }
}

bool http_get_text(const std::string& url, std::string* out, std::string* err) {
  try {
    using namespace winrt::Windows::Web::Http;
    HttpClient http;
    auto resp = http.GetAsync(winrt::Windows::Foundation::Uri(u8to16(url))).get();
    if (!resp.IsSuccessStatusCode()) {
      if (err) {
        char b[64];
        snprintf(b, sizeof(b), "HTTP %d", (int)resp.StatusCode());
        *err = b;
      }
      return false;
    }
    *out = u16to8(std::wstring(resp.Content().ReadAsStringAsync().get().c_str()));
    return true;
  } catch (winrt::hresult_error const& e) {
    if (err) *err = u16to8(std::wstring(e.message().c_str()));
    return false;
  } catch (...) {
    if (err) *err = "HTTP GET failed";
    return false;
  }
}

bool http_get_bytes(const std::string& url, std::vector<uint8_t>* out,
                    std::string* err, bool check_steam_result = false) {
  try {
    using namespace winrt::Windows::Web::Http;
    using namespace winrt::Windows::Storage::Streams;
    HttpClient http;
    auto resp = http.GetAsync(winrt::Windows::Foundation::Uri(u8to16(url))).get();
    if (!resp.IsSuccessStatusCode()) {
      if (err) {
        char b[64];
        snprintf(b, sizeof(b), "HTTP %d", (int)resp.StatusCode());
        *err = b;
      }
      return false;
    }
    if (check_steam_result && resp.Headers().HasKey(L"x-eresult") &&
        resp.Headers().Lookup(L"x-eresult") != L"1") {
      return false;
    }
    IBuffer buf = resp.Content().ReadAsBufferAsync().get();
    DataReader dr = DataReader::FromBuffer(buf);
    out->resize(buf.Length());
    if (!out->empty()) dr.ReadBytes(*out);
    return true;
  } catch (winrt::hresult_error const& e) {
    if (err) *err = "HTTP transport error " + std::to_string(static_cast<int32_t>(e.code()));
    return false;
  } catch (...) {
    if (err) *err = "HTTP download failed";
    return false;
  }
}

bool http_post_protobuf_steam(const std::string& endpoint,
                              const std::vector<uint8_t>& pb_in,
                              std::vector<uint8_t>* pb_out,
                              std::string* err) {
  try {
    using namespace winrt::Windows::Web::Http;
    using namespace winrt::Windows::Storage::Streams;
    HttpClient http;
    const std::string b64 = b64_encode(pb_in.data(), pb_in.size());
    const std::wstring body = L"input_protobuf_encoded=" + uri_escape(u8to16(b64));
    HttpStringContent content(body, UnicodeEncoding::Utf8,
                              L"application/x-www-form-urlencoded");
    auto resp =
        http.PostAsync(winrt::Windows::Foundation::Uri(u8to16(endpoint)), content)
            .get();
    if (!resp.IsSuccessStatusCode()) {
      if (err) {
        char b[64];
        snprintf(b, sizeof(b), "HTTP %d", (int)resp.StatusCode());
        *err = b;
      }
      return false;
    }
    IBuffer buf = resp.Content().ReadAsBufferAsync().get();
    DataReader dr = DataReader::FromBuffer(buf);
    pb_out->resize(buf.Length());
    if (!pb_out->empty()) dr.ReadBytes(*pb_out);
    return true;
  } catch (winrt::hresult_error const& e) {
    if (err) *err = u16to8(std::wstring(e.message().c_str()));
    return false;
  } catch (...) {
    if (err) *err = "HTTP POST protobuf failed";
    return false;
  }
}

bool http_post_form_steam(const std::string& endpoint,
                          const std::string& input_json, std::string* out,
                          std::string* err) {
  try {
    using namespace winrt::Windows::Web::Http;
    HttpClient http;
    const std::wstring body =
        L"input_protobuf_encoded=false&input=" + uri_escape(u8to16(input_json));
    HttpStringContent content(
        body, winrt::Windows::Storage::Streams::UnicodeEncoding::Utf8,
        L"application/x-www-form-urlencoded");
    auto resp =
        http.PostAsync(winrt::Windows::Foundation::Uri(u8to16(endpoint)), content)
            .get();
    if (!resp.IsSuccessStatusCode()) {
      if (err) {
        char b[64];
        snprintf(b, sizeof(b), "HTTP %d", (int)resp.StatusCode());
        *err = b;
      }
      return false;
    }
    *out = u16to8(std::wstring(resp.Content().ReadAsStringAsync().get().c_str()));
    return true;
  } catch (winrt::hresult_error const& e) {
    if (err) *err = u16to8(std::wstring(e.message().c_str()));
    return false;
  } catch (...) {
    if (err) *err = "HTTP POST failed";
    return false;
  }
}

// ---- Tiny Protobuf Wire Codec ----
struct pb_writer {
  std::vector<uint8_t> buf;
  void write_varint(uint64_t v) {
    while (v >= 0x80) {
      buf.push_back((uint8_t)((v & 0x7F) | 0x80));
      v >>= 7;
    }
    buf.push_back((uint8_t)v);
  }
  void write_tag(uint32_t field, uint32_t wt) {
    write_varint(((uint64_t)field << 3) | wt);
  }
  void write_uint32(uint32_t field, uint32_t v) {
    write_tag(field, 0);
    write_varint(v);
  }
  void write_uint64(uint32_t field, uint64_t v) {
    write_tag(field, 0);
    write_varint(v);
  }
  void write_fixed64(uint32_t field, uint64_t v) {
    write_tag(field, 1);
    for (int i = 0; i < 8; ++i) buf.push_back((uint8_t)((v >> (i * 8)) & 0xFF));
  }
  void write_fixed32(uint32_t field, uint32_t v) {
    write_tag(field, 5);
    for (int i = 0; i < 4; ++i) buf.push_back((uint8_t)((v >> (i * 8)) & 0xFF));
  }
  void write_bytes(uint32_t field, const void* data, size_t len) {
    write_tag(field, 2);
    write_varint(len);
    const uint8_t* p = (const uint8_t*)data;
    buf.insert(buf.end(), p, p + len);
  }
  void write_string(uint32_t field, const std::string& s) {
    write_bytes(field, s.data(), s.size());
  }
};

struct pb_reader {
  const uint8_t* p;
  size_t len;
  size_t pos = 0;
  bool read_varint(uint64_t* out) {
    uint64_t v = 0;
    int shift = 0;
    while (pos < len) {
      uint8_t b = p[pos++];
      if (shift == 63 && b > 1) return false;
      v |= (uint64_t)(b & 0x7F) << shift;
      if ((b & 0x80) == 0) {
        *out = v;
        return true;
      }
      shift += 7;
      if (shift >= 70) return false;
    }
    return false;
  }
  bool next(uint32_t* field, uint32_t* wt) {
    if (pos >= len) return false;
    uint64_t tag = 0;
    if (!read_varint(&tag)) return false;
    *field = (uint32_t)(tag >> 3);
    *wt = (uint32_t)(tag & 7);
    return true;
  }
  bool read_fixed64(uint64_t* out) {
    if (pos + 8 > len) return false;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)p[pos + i] << (i * 8);
    pos += 8;
    *out = v;
    return true;
  }
  bool read_fixed32(uint32_t* out) {
    if (pos + 4 > len) return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= (uint32_t)p[pos + i] << (i * 8);
    pos += 4;
    *out = v;
    return true;
  }
  bool read_bytes(const uint8_t** out_p, size_t* out_len) {
    uint64_t l = 0;
    if (!read_varint(&l) || l > len - pos) return false;
    *out_p = p + pos;
    *out_len = (size_t)l;
    pos += (size_t)l;
    return true;
  }
  bool skip(uint32_t wt) {
    uint64_t dummy = 0;
    const uint8_t* dp = nullptr;
    size_t dl = 0;
    if (wt == 0) return read_varint(&dummy);
    if (wt == 1) return read_fixed64(&dummy);
    if (wt == 2) return read_bytes(&dp, &dl);
    if (wt == 5) {
      uint32_t d32 = 0;
      return read_fixed32(&d32);
    }
    return false;
  }
};

// Family catalog reads use the account's existing access token. They do
// not reserve a copy or grant a license to the guest Steamworks bridge.
bool get_family_protobuf(const char* method, const std::string& access_token,
                         const pb_writer& request, std::vector<uint8_t>* response) {
  const auto encode = [](const std::string& value) {
    return u16to8(uri_escape(u8to16(value)));
  };
  const std::string url =
      std::string("https://api.steampowered.com/IFamilyGroupsService/") + method +
      "/v1?access_token=" + encode(access_token) +
      "&input_protobuf_encoded=" + encode(b64_encode(request.buf.data(), request.buf.size()));
  return http_get_bytes(url, response, nullptr, true);
}

bool parse_family_membership(const std::vector<uint8_t>& response, uint64_t* group) {
  pb_reader reader{response.data(), response.size()};
  uint64_t group_id = 0;
  uint64_t not_a_member = 0;
  while (reader.pos < reader.len) {
    uint32_t field, wire;
    if (!reader.next(&field, &wire) || field == 0) return false;
    if (wire == 0 && (field == 1 || field == 2)) {
      if (!reader.read_varint(field == 1 ? &group_id : &not_a_member)) return false;
    } else if (!reader.skip(wire)) {
      return false;
    }
  }
  *group = not_a_member ? 0 : group_id;
  return not_a_member || group_id != 0;
}

bool parse_family_apps(const std::vector<uint8_t>& response,
                       std::vector<steam_library_item>* items) {
  pb_reader reader{response.data(), response.size()};
  std::vector<steam_library_item> catalog;
  while (reader.pos < reader.len) {
    uint32_t field, wire;
    if (!reader.next(&field, &wire) || field == 0) return false;
    if (field != 1 || wire != 2) {
      if (!reader.skip(wire)) return false;
      continue;
    }
    const uint8_t* data;
    size_t size;
    if (!reader.read_bytes(&data, &size)) return false;
    pb_reader app{data, size};
    uint64_t id = 0, excluded = 0;
    steam_library_item item;
    while (app.pos < app.len) {
      if (!app.next(&field, &wire) || field == 0) return false;
      if ((field == 1 || field == 10) && wire == 0) {
        if (!app.read_varint(field == 1 ? &id : &excluded)) return false;
      } else if (field == 6 && wire == 2) {
        if (!app.read_bytes(&data, &size)) return false;
        item.title.assign(reinterpret_cast<const char*>(data), size);
      } else if (!app.skip(wire)) {
        return false;
      }
    }
    if (!id || id > UINT32_MAX || excluded) continue;
    item.app_id = static_cast<uint32_t>(id);
    if (item.title.empty()) item.title = "App " + std::to_string(id);
    item.family_shared = true;
    item.size_str = "Steam Families";
    item.status_text = "Shared library";
    catalog.push_back(std::move(item));
  }
  std::sort(catalog.begin(), catalog.end(),
            [](const steam_library_item& a, const steam_library_item& b) {
              return a.title < b.title || (a.title == b.title && a.app_id < b.app_id);
            });
  *items = std::move(catalog);
  return true;
}

bool fetch_family_apps(uint64_t sid, const std::string& access_token,
                       std::vector<steam_library_item>* items) {
  pb_writer membership;
  membership.write_uint64(1, sid);
  std::vector<uint8_t> response;
  uint64_t group = 0;
  if (!get_family_protobuf("GetFamilyGroupForUser", access_token, membership, &response) ||
      !parse_family_membership(response, &group)) return false;
  if (group == 0) return true;

  pb_writer request;
  request.write_fixed64(1, group);
  request.write_uint32(2, 0);  // exclude titles owned by this account
  request.write_uint32(3, 0);  // exclude titles Steam does not share
  request.write_fixed64(8, sid);
  return get_family_protobuf("GetSharedLibraryApps", access_token, request, &response) &&
         parse_family_apps(response, items);
}

// ---- BCrypt AES-256 ----
bool bcrypt_aes256_decrypt_steam(const uint8_t* cipher, size_t cipher_len,
                                 const uint8_t key[32],
                                 std::vector<uint8_t>* plain) {
  if (cipher_len < 32 || (cipher_len % 16) != 0) return false;
  BCRYPT_ALG_HANDLE hEcb = nullptr;
  BCRYPT_ALG_HANDLE hCbc = nullptr;
  BCRYPT_KEY_HANDLE kEcb = nullptr;
  BCRYPT_KEY_HANDLE kCbc = nullptr;
  if (BCryptOpenAlgorithmProvider(&hEcb, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0)
    return false;
  if (BCryptOpenAlgorithmProvider(&hCbc, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) {
    BCryptCloseAlgorithmProvider(hEcb, 0);
    return false;
  }
  bool ok = false;
  BCryptSetProperty(hEcb, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB,
                    sizeof(BCRYPT_CHAIN_MODE_ECB), 0);
  BCryptSetProperty(hCbc, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
                    sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
  if (BCryptGenerateSymmetricKey(hEcb, &kEcb, nullptr, 0, (PUCHAR)key, 32, 0) >=
          0 &&
      BCryptGenerateSymmetricKey(hCbc, &kCbc, nullptr, 0, (PUCHAR)key, 32, 0) >=
          0) {
    uint8_t iv[16] = {};
    ULONG cbOut = 0;
    if (BCryptDecrypt(kEcb, (PUCHAR)cipher, 16, nullptr, nullptr, 0, iv, 16,
                      &cbOut, 0) >= 0 &&
        cbOut == 16) {
      const uint8_t* body = cipher + 16;
      const ULONG body_len = (ULONG)(cipher_len - 16);
      plain->resize(body_len);
      if (BCryptDecrypt(kCbc, (PUCHAR)body, body_len, nullptr, iv, 16,
                        plain->data(), body_len, &cbOut,
                        BCRYPT_BLOCK_PADDING) >= 0) {
        plain->resize(cbOut);
        ok = true;
      }
    }
  }
  if (kEcb) BCryptDestroyKey(kEcb);
  if (kCbc) BCryptDestroyKey(kCbc);
  BCryptCloseAlgorithmProvider(hEcb, 0);
  BCryptCloseAlgorithmProvider(hCbc, 0);
  return ok;
}

// ---- Compression helpers (gzip raw, PK zip, compact LZMA1 for VZ) ----
bool gzip_decompress(const uint8_t* in, size_t in_len,
                     std::vector<uint8_t>* out) {
  if (in_len < 18 || in[0] != 0x1F || in[1] != 0x8B) return false;
  size_t off = 10;
  uint8_t flg = in[3];
  if (flg & 0x04) {
    if (off + 2 > in_len) return false;
    uint16_t xlen = (uint16_t)in[off] | ((uint16_t)in[off + 1] << 8);
    off += 2 + xlen;
  }
  if (flg & 0x08) {
    while (off < in_len && in[off] != 0) ++off;
    ++off;
  }
  if (flg & 0x10) {
    while (off < in_len && in[off] != 0) ++off;
    ++off;
  }
  if (flg & 0x02) off += 2;
  if (off + 8 >= in_len) return false;
  size_t raw_len = in_len - off - 8;
  mz_stream s{};
  if (mz_inflateInit2(&s, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) return false;
  s.next_in = in + off;
  s.avail_in = (unsigned int)raw_len;
  out->clear();
  uint8_t tmp[16384];
  int rc = MZ_OK;
  do {
    s.next_out = tmp;
    s.avail_out = sizeof(tmp);
    rc = mz_inflate(&s, MZ_NO_FLUSH);
    if (rc != MZ_OK && rc != MZ_STREAM_END) {
      mz_inflateEnd(&s);
      return false;
    }
    size_t got = sizeof(tmp) - s.avail_out;
    out->insert(out->end(), tmp, tmp + got);
  } while (rc != MZ_STREAM_END);
  mz_inflateEnd(&s);
  return true;
}

// Steam's depot manifest stores an Adler-32 of the decompressed chunk with a
// zero initial seed (zlib's default seed is 1). SteamKit's DepotChunk uses the
// same zero seed, so a chunk Steam considers valid checksums to this value.
uint32_t adler32_steam(const uint8_t* data, size_t len) {
  constexpr uint32_t kBase = 65521;
  constexpr size_t kNmax = 5552;  // largest n before the sums need reducing
  uint32_t a = 0, b = 0;
  while (len > 0) {
    const size_t n = len < kNmax ? len : kNmax;
    len -= n;
    for (size_t i = 0; i < n; ++i) {
      a += data[i];
      b += a;
    }
    data += n;
    a %= kBase;
    b %= kBase;
  }
  return (b << 16) | a;
}

// Minimal LZMA1 decoder for Valve VZ depot chunks.
// Header: 'V' 'Z' 'a' [4B CRC/timestamp] [5B LZMA props (lc/lp/pb + dict)]
// Body: compressed LZMA1 stream
// Footer: [4B CRC32 of decompressed data] [4B size LE] 'z' 'v'
bool lzma1_decode_raw(const uint8_t props[5], const uint8_t* src, size_t src_len,
                      uint32_t out_size, std::vector<uint8_t>* out) {
  uint8_t p0 = props[0];
  if (p0 >= 9 * 5 * 5) return false;
  int lc = p0 % 9;
  p0 /= 9;
  int lp = p0 % 5;
  int pb = p0 / 5;
  uint32_t dict_size = (uint32_t)props[1] | ((uint32_t)props[2] << 8) |
                       ((uint32_t)props[3] << 16) | ((uint32_t)props[4] << 24);
  if (dict_size < 4096) dict_size = 4096;
  if (src_len < 5) return false;

  struct rc_t {
    const uint8_t* buf;
    size_t len;
    size_t pos;
    uint32_t range;
    uint32_t code;
    void norm() {
      if (range < (1u << 24)) {
        range <<= 8;
        code = (code << 8) | (pos < len ? buf[pos++] : 0);
      }
    }
    int bit(uint16_t* prob) {
      norm();
      uint32_t bound = (range >> 11) * (*prob);
      if (code < bound) {
        range = bound;
        *prob += (uint16_t)((2048 - *prob) >> 5);
        return 0;
      } else {
        range -= bound;
        code -= bound;
        *prob -= (uint16_t)(*prob >> 5);
        return 1;
      }
    }
    uint32_t direct(int n) {
      uint32_t res = 0;
      for (int i = n; i > 0; --i) {
        norm();
        range >>= 1;
        uint32_t t = (uint32_t)(code - range) >> 31;
        code -= range & (t - 1);
        res = (res << 1) | (1 - t);
      }
      return res;
    }
    uint32_t bittree(uint16_t* probs, int n) {
      uint32_t m = 1;
      for (int i = 0; i < n; ++i) m = (m << 1) | bit(&probs[m]);
      return m - (1u << n);
    }
    uint32_t rev_bittree(uint16_t* probs, int n) {
      uint32_t m = 1, sym = 0;
      for (int i = 0; i < n; ++i) {
        int b = bit(&probs[m]);
        m = (m << 1) | b;
        sym |= (uint32_t)b << i;
      }
      return sym;
    }
  } rc{src, src_len, 5, 0xFFFFFFFFu, 0};
  for (int i = 0; i < 5; ++i) rc.code = (rc.code << 8) | src[i];

  struct len_dec {
    uint16_t choice = 1024;
    uint16_t choice2 = 1024;
    uint16_t low[16][8];
    uint16_t mid[16][8];
    uint16_t high[256];
    len_dec() {
      std::fill(&low[0][0], &low[0][0] + 16 * 8, 1024);
      std::fill(&mid[0][0], &mid[0][0] + 16 * 8, 1024);
      std::fill(high, high + 256, 1024);
    }
    uint32_t dec(rc_t& r, int pos_state) {
      if (r.bit(&choice) == 0) return r.bittree(low[pos_state], 3);
      if (r.bit(&choice2) == 0) return 8 + r.bittree(mid[pos_state], 3);
      return 16 + r.bittree(high, 8);
    }
  };

  std::vector<uint16_t> lit_probs((1u << (lc + lp)) * 0x300, 1024);
  uint16_t is_match[192], is_rep[12], is_rep_g0[12], is_rep_g1[12], is_rep_g2[12],
      is_rep0_long[192];
  std::fill(is_match, is_match + 192, 1024);
  std::fill(is_rep, is_rep + 12, 1024);
  std::fill(is_rep_g0, is_rep_g0 + 12, 1024);
  std::fill(is_rep_g1, is_rep_g1 + 12, 1024);
  std::fill(is_rep_g2, is_rep_g2 + 12, 1024);
  std::fill(is_rep0_long, is_rep0_long + 192, 1024);
  uint16_t pos_slot[4][64], pos_decoders[115], align_probs[16];
  std::fill(&pos_slot[0][0], &pos_slot[0][0] + 4 * 64, 1024);
  std::fill(pos_decoders, pos_decoders + 115, 1024);
  std::fill(align_probs, align_probs + 16, 1024);
  len_dec match_len, rep_len;

  out->clear();
  out->reserve(out_size);
  uint32_t rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;
  int state = 0;
  const uint32_t pb_mask = (1u << pb) - 1;
  const uint32_t lp_mask = (1u << lp) - 1;

  while (out->size() < out_size) {
    int pos_state = (int)(out->size() & pb_mask);
    if (rc.bit(&is_match[(state << 4) + pos_state]) == 0) {
      uint8_t prev = out->empty() ? 0 : out->back();
      uint32_t lidx = ((out->size() & lp_mask) << lc) + (prev >> (8 - lc));
      uint16_t* probs = &lit_probs[lidx * 0x300];
      uint32_t sym = 1;
      if (state >= 7) {
        uint8_t match_b =
            (out->size() >= rep0 + 1) ? (*out)[out->size() - rep0 - 1] : 0;
        do {
          int mb = (match_b >> 7) & 1;
          match_b <<= 1;
          int b = rc.bit(&probs[((1 + mb) << 8) + sym]);
          sym = (sym << 1) | b;
          if (mb != b) break;
        } while (sym < 0x100);
      }
      while (sym < 0x100) sym = (sym << 1) | rc.bit(&probs[sym]);
      out->push_back((uint8_t)sym);
      state = (state < 4) ? 0 : (state < 10 ? state - 3 : state - 6);
    } else {
      uint32_t len = 0;
      if (rc.bit(&is_rep[state]) == 1) {
        if (rc.bit(&is_rep_g0[state]) == 0) {
          if (rc.bit(&is_rep0_long[(state << 4) + pos_state]) == 0) {
            state = state < 7 ? 9 : 11;
            if (out->size() < rep0 + 1) return false;
            out->push_back((*out)[out->size() - rep0 - 1]);
            continue;
          }
        } else {
          uint32_t dist = 0;
          if (rc.bit(&is_rep_g1[state]) == 0) {
            dist = rep1;
          } else {
            if (rc.bit(&is_rep_g2[state]) == 0) {
              dist = rep2;
            } else {
              dist = rep3;
              rep3 = rep2;
            }
            rep2 = rep1;
          }
          rep1 = rep0;
          rep0 = dist;
        }
        len = rep_len.dec(rc, pos_state) + 2;
        state = state < 7 ? 8 : 11;
      } else {
        rep3 = rep2;
        rep2 = rep1;
        rep1 = rep0;
        len = match_len.dec(rc, pos_state) + 2;
        state = state < 7 ? 7 : 10;
        int len_st = len < 6 ? (int)(len - 2) : 3;
        uint32_t slot = rc.bittree(pos_slot[len_st], 6);
        if (slot < 4) {
          rep0 = slot;
        } else {
          int num_direct = (int)((slot >> 1) - 1);
          rep0 = (2u | (slot & 1u)) << num_direct;
          if (slot < 14) {
            rep0 += rc.rev_bittree(pos_decoders + rep0 - slot, num_direct);
          } else {
            rep0 += rc.direct(num_direct - 4) << 4;
            rep0 += rc.rev_bittree(align_probs, 4);
            if (rep0 == 0xFFFFFFFFu) break;
          }
        }
      }
      if (out->size() < rep0 + 1) return false;
      for (uint32_t i = 0; i < len && out->size() < out_size; ++i) {
        out->push_back((*out)[out->size() - rep0 - 1]);
      }
    }
  }
  return out->size() == out_size;
}

bool decompress_steam_payload(const uint8_t* raw, size_t len,
                              uint32_t expected_size,
                              std::vector<uint8_t>* out, std::string* err) {
  if (len >= 23 && raw[0] == 'V' && raw[1] == 'S' && raw[2] == 'Z' &&
      raw[3] == 'a') {
    // Valve VZstd container (SteamKit VZstdUtil):
    //   header: "VSZa" + CRC32 (4 bytes)
    //   body:   standard zstd frame
    //   footer: CRC32 (4), decompressed size (4), 4 unknown bytes, "zsv" (3)
    // The body is NOT just the bytes after the magic; the header CRC and the
    // 15-byte footer must be stripped before calling zstd.
    if (raw[len - 3] != 'z' || raw[len - 2] != 's' || raw[len - 1] != 'v') {
      if (err) *err = "VSZa footer missing zsv";
      return false;
    }
    const uint32_t footer_size =
        (uint32_t)raw[len - 11] | ((uint32_t)raw[len - 10] << 8) |
        ((uint32_t)raw[len - 9] << 16) | ((uint32_t)raw[len - 8] << 24);
    if (expected_size != 0 && footer_size != expected_size) {
      if (err) *err = "VSZa size " + std::to_string(footer_size) +
                      " does not match manifest " +
                      std::to_string(expected_size);
      return false;
    }

    unsigned long long need = expected_size ? expected_size : footer_size;
    const uint8_t* frame = raw + 8;
    const size_t frame_len = len - 8 - 15;
    if (need == 0) {
      need = ZSTD_getFrameContentSize(frame, frame_len);
      if (need == ZSTD_CONTENTSIZE_ERROR || need == ZSTD_CONTENTSIZE_UNKNOWN) {
        if (err) *err = "VSZa frame size unknown";
        return false;
      }
    }
    if (need > (unsigned long long)64 * 1024 * 1024) {
      if (err) *err = "VSZa frame too large";
      return false;
    }
    out->resize((size_t)need);
    size_t written = ZSTD_decompress(out->data(), out->size(), frame, frame_len);
    if (ZSTD_isError(written)) {
      if (err) *err = std::string("VSZa zstd error: ") +
                      ZSTD_getErrorName(written);
      out->clear();
      return false;
    }
    if (footer_size != 0 && written != footer_size) {
      if (err) *err = "VSZa decompressed to " + std::to_string(written) +
                      ", footer says " + std::to_string(footer_size);
      out->clear();
      return false;
    }
    out->resize(written);
    return true;
  }
  if (len >= 2 && raw[0] == 'P' && raw[1] == 'K') {
    mz_zip_archive za{};
    if (!mz_zip_reader_init_mem(&za, raw, len, 0)) {
      if (err) *err = "zip reader init failed";
      return false;
    }
    bool ok = false;
    if (mz_zip_reader_get_num_files(&za) > 0) {
      size_t sz = 0;
      void* p = mz_zip_reader_extract_to_heap(&za, 0, &sz, 0);
      if (p) {
        out->assign((const uint8_t*)p, (const uint8_t*)p + sz);
        mz_free(p);
        ok = true;
      }
    }
    mz_zip_reader_end(&za);
    if (!ok && err) *err = "zip extract failed";
    return ok;
  }
  if (len >= 22 && raw[0] == 'V' && raw[1] == 'Z' && raw[2] == 'a' &&
      raw[len - 2] == 'z' && raw[len - 1] == 'v') {
    // Valve VZ: 'V','Z','a',timestamp/crc(4),props(5),data...,
    // crc32(4),size(4),'z','v'. The decompressed size is the four bytes at
    // len-6, not the last four bytes (those are size high bytes + 'z','v').
    uint32_t sz = (uint32_t)raw[len - 6] | ((uint32_t)raw[len - 5] << 8) |
                  ((uint32_t)raw[len - 4] << 16) |
                  ((uint32_t)raw[len - 3] << 24);
    if (sz == 0) sz = expected_size;
    if (expected_size > 0 && sz != expected_size) {
      if (err) *err = "VZ size " + std::to_string(sz) +
                      " does not match manifest " +
                      std::to_string(expected_size);
      return false;
    }
    // Header is 12 bytes (7-byte VZ prefix + 5 LZMA props); footer is 10.
    if (!lzma1_decode_raw(raw + 7, raw + 12, len - 22, sz, out)) {
      if (err) *err = "VZ LZMA1 decode failed";
      return false;
    }
    return true;
  }
  if (expected_size > 0 && len == expected_size) {
    out->assign(raw, raw + len);
    return true;
  }
  if (err) *err = "unknown Steam compression signature";
  return false;
}

// The WinRT MessageWebSocket callbacks capture `this`. A stack-allocated
// session died before the Closed/MessageReceived delegates did on the console
// (HoloCure 2420510: FAIL then HOST UNHANDLED 0xC0000005). Keep the session
// shared and have callbacks hold a strong reference so the delegate cannot
// outlive the object it touches.
class cm_session : public std::enable_shared_from_this<cm_session> {
 public:
  winrt::Windows::Networking::Sockets::MessageWebSocket ws{nullptr};
  std::mutex rx_mu;
  std::condition_variable rx_cv;
  std::deque<std::vector<uint8_t>> rx_msgs;
  bool closed = false;
  uint32_t session_id = 0;
  uint64_t steamid = 0;
  uint64_t job_seq = 1;
  int32_t last_eresult = 2;

  ~cm_session() {
    if (ws) {
      try {
        ws.Close(1000, L"done");
      } catch (...) {
      }
    }
  }

  bool connect(const std::string& host, std::string* err) {
    try {
      using namespace winrt::Windows::Networking::Sockets;
      ws = MessageWebSocket();
      ws.Control().MessageType(SocketMessageType::Binary);
      auto self = shared_from_this();
      ws.MessageReceived([self](MessageWebSocket const&,
                                MessageWebSocketMessageReceivedEventArgs const& a) {
        try {
          auto dr = a.GetDataReader();
          uint32_t n = dr.UnconsumedBufferLength();
          std::vector<uint8_t> pkt(n);
          if (n > 0) dr.ReadBytes(pkt);
          std::lock_guard<std::mutex> lk(self->rx_mu);
          self->rx_msgs.push_back(std::move(pkt));
          self->rx_cv.notify_all();
        } catch (...) {
        }
      });
      ws.Closed([self](auto const&, auto const&) {
        std::lock_guard<std::mutex> lk(self->rx_mu);
        self->closed = true;
        self->rx_cv.notify_all();
      });
      std::string url = "wss://" + host + "/cmsocket/";
      ws.ConnectAsync(winrt::Windows::Foundation::Uri(u8to16(url))).get();
      return true;
    } catch (winrt::hresult_error const& e) {
      if (err) *err = u16to8(std::wstring(e.message().c_str()));
      return false;
    } catch (...) {
      if (err) *err = "CM WebSocket connect failed";
      return false;
    }
  }

  bool send_proto(uint32_t emsg, const pb_writer& header_fields,
                  const pb_writer& body, uint64_t job_id = 0,
                  const std::string& target_job_name = std::string()) {
    try {
      pb_writer hdr;
      if (steamid != 0) hdr.write_fixed64(1, steamid);
      if (session_id != 0) hdr.write_uint32(2, session_id);
      if (job_id != 0) hdr.write_fixed64(10, job_id);
      if (!target_job_name.empty()) hdr.write_string(12, target_job_name);
      hdr.buf.insert(hdr.buf.end(), header_fields.buf.begin(),
                     header_fields.buf.end());

      std::vector<uint8_t> pkt;
      uint32_t raw_emsg = emsg | 0x80000000u;
      uint32_t hlen = (uint32_t)hdr.buf.size();
      pkt.resize(8 + hlen + body.buf.size());
      std::memcpy(pkt.data(), &raw_emsg, 4);
      std::memcpy(pkt.data() + 4, &hlen, 4);
      if (hlen > 0) std::memcpy(pkt.data() + 8, hdr.buf.data(), hlen);
      if (!body.buf.empty())
        std::memcpy(pkt.data() + 8 + hlen, body.buf.data(), body.buf.size());

      auto dw = winrt::Windows::Storage::Streams::DataWriter(ws.OutputStream());
      dw.WriteBytes(pkt);
      dw.StoreAsync().get();
      dw.DetachStream();
      return true;
    } catch (...) {
      return false;
    }
  }

  // Wait for a specific EMsg (automatically unpacks EMsgMulti = 1)
  bool recv_emsg(uint32_t want_emsg, std::vector<uint8_t>* out_body,
                 int timeout_ms = 12000, uint64_t expected_job = 0) {
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
      std::vector<uint8_t> pkt;
      {
        std::unique_lock<std::mutex> lk(rx_mu);
        if (rx_msgs.empty()) {
          if (closed) return false;
          if (rx_cv.wait_until(lk, deadline) == std::cv_status::timeout) {
            return false;
          }
          if (rx_msgs.empty()) {
            if (closed) return false;
            continue;
          }
        }
        pkt = std::move(rx_msgs.front());
        rx_msgs.pop_front();
      }
      if (dispatch_pkt(pkt, want_emsg, out_body, expected_job)) return true;
    }
  }

  // Steam's modern service RPCs (e.g. ContentServerDirectory) are sent as
  // EMsg.ServiceMethodCallFromClient (151) with target_job_name set to the
  // method, and answered with EMsg.ServiceMethodResponse (147). The response
  // body is CMsgClientServiceMethodLegacyResponse, whose field 2 contains the
  // serialized service response.
  bool unified_call(const std::string& method, const pb_writer& request,
                    std::vector<uint8_t>* response, std::string* err,
                    bool require_success = false) {
    last_eresult = 2;
    const uint64_t job = job_seq++;
    if (!send_proto(151 /* ServiceMethodCallFromClient */, {}, request, job,
                    method)) {
      if (err) *err = "service request send failed";
      return false;
    }
    std::vector<uint8_t> pkt_body;
    if (!recv_emsg(147 /* ServiceMethodResponse */, &pkt_body, 12000,
                   require_success ? job : 0)) {
      if (err) *err = "service response timeout";
      return false;
    }

    if (require_success && last_eresult != 1) {
      if (err) *err = "Steam service denied the request (eresult=" +
                      std::to_string(last_eresult) + ")";
      return false;
    }

    pb_reader r{pkt_body.data(), pkt_body.size()};
    uint32_t f = 0, wt = 0;
    bool got = false, saw_method_name = false;
    while (r.next(&f, &wt)) {
      if (f == 1 && wt == 2) {
        saw_method_name = true;
        r.skip(wt);
      } else if (f == 2 && wt == 2) {
        const uint8_t* p = nullptr;
        size_t n = 0;
        if (r.read_bytes(&p, &n)) {
          response->assign(p, p + n);
          got = true;
        }
      } else {
        r.skip(wt);
      }
    }
    // Older CM builds send the service response directly instead of wrapping
    // it in CMsgClientServiceMethodLegacyResponse.
    if (!got && !saw_method_name && !pkt_body.empty()) {
      response->assign(pkt_body.begin(), pkt_body.end());
      got = true;
    }
    if (!got) {
      if (err) {
        *err = "service response missing (eresult=" +
               std::to_string(last_eresult) + ")";
      }
      return false;
    }
    return true;
  }

  bool dispatch_pkt(const std::vector<uint8_t>& pkt, uint32_t want_emsg,
                    std::vector<uint8_t>* out_body, uint64_t expected_job) {
    if (pkt.size() < 8) return false;
    uint32_t raw_emsg = 0, hlen = 0;
    std::memcpy(&raw_emsg, pkt.data(), 4);
    std::memcpy(&hlen, pkt.data() + 4, 4);
    if (8 + (size_t)hlen > pkt.size()) return false;
    uint32_t emsg = raw_emsg & 0x7FFFFFFFu;
    pb_reader hr{pkt.data() + 8, hlen};
    uint64_t response_job = 0;
    int response_result = 2;
    uint32_t f, wt;
    while (hr.next(&f, &wt)) {
      if (f == 1 && wt == 1) {
        uint64_t sid = 0;
        hr.read_fixed64(&sid);
        if (sid != 0) steamid = sid;
      } else if (f == 2 && wt == 0) {
        uint64_t s = 0;
        hr.read_varint(&s);
        if (s != 0) session_id = (uint32_t)s;
      } else if (f == 11 && wt == 1) {
        if (!hr.read_fixed64(&response_job)) return false;
      } else if (f == 13 && wt == 0) {
        uint64_t e = 0;
        if (!hr.read_varint(&e)) return false;
        response_result = (int32_t)e;
        if (!expected_job) last_eresult = response_result;
      } else if (f == 12 && wt == 2) {
        const uint8_t* np = nullptr;
        size_t nl = 0;
        if (hr.read_bytes(&np, &nl)) {
          (void)np;
          (void)nl;
        }
      } else {
        hr.skip(wt);
      }
    }
    const uint8_t* b = pkt.data() + 8 + hlen;
    size_t blen = pkt.size() - 8 - hlen;
    if (emsg == 1 /* EMsgMulti */) {
      pb_reader mr{b, blen};
      uint32_t uncomp_sz = 0;
      const uint8_t* payload = nullptr;
      size_t payload_len = 0;
      while (mr.next(&f, &wt)) {
        if (f == 1 && wt == 0) {
          uint64_t u = 0;
          mr.read_varint(&u);
          uncomp_sz = (uint32_t)u;
        } else if (f == 2 && wt == 2) {
          mr.read_bytes(&payload, &payload_len);
        } else {
          mr.skip(wt);
        }
      }
      std::vector<uint8_t> flat;
      if (uncomp_sz > 0 && payload) {
        if (!gzip_decompress(payload, payload_len, &flat)) return false;
      } else if (payload) {
        flat.assign(payload, payload + payload_len);
      }
      size_t off = 0;
      while (off + 4 <= flat.size()) {
        uint32_t sub_len = 0;
        std::memcpy(&sub_len, flat.data() + off, 4);
        off += 4;
        if (off + sub_len > flat.size()) break;
        std::vector<uint8_t> sub(flat.begin() + off, flat.begin() + off + sub_len);
        off += sub_len;
        if (dispatch_pkt(sub, want_emsg, out_body, expected_job)) return true;
      }
      return false;
    }
    if (emsg == want_emsg && (!expected_job || response_job == expected_job)) {
      if (expected_job) last_eresult = response_result;
      out_body->assign(b, b + blen);
      return true;
    }
    return false;
  }
};

// Parse Steam VDF text sections for depot_id, manifest_id, launch executable.
// The PICS appinfo blob is a binary KeyValue tree: it has a small type/name
// prefix before the first '"', and "depots" contains many numeric child
// blocks. Scan the depots block by brace depth instead of doing a global
// substring search, which picked the first redist manifest for every title.
void parse_appinfo_vdf(const std::string& vdf, uint32_t* out_depot,
                       uint64_t* out_manifest, uint64_t* out_size,
                       std::string* out_exe) {
  // Find executable in config -> launch -> * -> executable
  size_t lpos = vdf.find("\"launch\"");
  if (lpos != std::string::npos) {
    size_t ep = vdf.find("\"executable\"", lpos);
    if (ep != std::string::npos) {
      size_t q1 = vdf.find('"', ep + 12);
      size_t q2 = (q1 != std::string::npos) ? vdf.find('"', q1 + 1) : std::string::npos;
      if (q1 != std::string::npos && q2 != std::string::npos)
        *out_exe = vdf.substr(q1 + 1, q2 - q1 - 1);
    }
  }

  size_t dpos = vdf.find("\"depots\"");
  if (dpos == std::string::npos) dpos = vdf.find("\"Depots\"");
  if (dpos == std::string::npos) return;
  size_t brace = vdf.find('{', dpos);
  if (brace == std::string::npos) return;

  // Matching brace for the depots section.
  auto matching = [&](size_t open) -> size_t {
    int depth = 0;
    for (size_t i = open; i < vdf.size(); ++i) {
      if (vdf[i] == '{') ++depth;
      else if (vdf[i] == '}') {
        if (--depth == 0) return i;
      }
    }
    return std::string::npos;
  };
  size_t depots_end = matching(brace);
  if (depots_end == std::string::npos) return;

  uint32_t best_depot = 0;
  uint64_t best_manifest = 0;
  uint64_t best_sz = 0;
  size_t pos = brace + 1;
  while (pos < depots_end) {
    size_t q1 = vdf.find('"', pos);
    if (q1 == std::string::npos || q1 >= depots_end) break;
    size_t q2 = vdf.find('"', q1 + 1);
    if (q2 == std::string::npos || q2 >= depots_end) break;
    std::string key = vdf.substr(q1 + 1, q2 - q1 - 1);
    pos = q2 + 1;

    bool all_digits = !key.empty();
    for (char c : key)
      if (!std::isdigit((unsigned char)c)) all_digits = false;
    if (!all_digits) continue;

    // The value block must follow the key (allow whitespace only).
    size_t b = pos;
    while (b < depots_end && std::isspace((unsigned char)vdf[b])) ++b;
    if (b >= depots_end || vdf[b] != '{') continue;
    size_t e = matching(b);
    if (e == std::string::npos || e > depots_end) continue;
    pos = e + 1;

    uint32_t did = (uint32_t)strtoul(key.c_str(), nullptr, 10);
    if (did < 10 || did == 228980 || did == 228990) continue; // shared redist

    std::string blk = vdf.substr(b, e - b + 1);
    // oslist is a quoted CSV; accept only blocks that allow Windows.
    size_t ol = blk.find("\"oslist\"");
    if (ol != std::string::npos) {
      size_t value_start = blk.find('"', ol + 8);
      size_t value_end = (value_start != std::string::npos)
                             ? blk.find('"', value_start + 1)
                             : std::string::npos;
      std::string oslist = (value_start != std::string::npos &&
                            value_end != std::string::npos)
                               ? blk.substr(value_start + 1, value_end - value_start - 1)
                               : "";
      if (oslist.find("windows") == std::string::npos) continue;
    }
    size_t mp = blk.find("\"public\"");
    if (mp == std::string::npos) continue;
    size_t gp = blk.find("\"gid\"", mp);
    if (gp == std::string::npos) continue;
    size_t gq1 = blk.find('"', gp + 5);
    size_t gq2 = (gq1 != std::string::npos) ? blk.find('"', gq1 + 1) : std::string::npos;
    if (gq1 == std::string::npos || gq2 == std::string::npos) continue;
    uint64_t mid = strtoull(blk.substr(gq1 + 1, gq2 - gq1 - 1).c_str(), nullptr, 10);
    uint64_t msz = 0;
    size_t sp = blk.find("\"size\"", mp);
    if (sp != std::string::npos) {
      size_t sq1 = blk.find('"', sp + 6);
      size_t sq2 = (sq1 != std::string::npos) ? blk.find('"', sq1 + 1) : std::string::npos;
      if (sq1 != std::string::npos && sq2 != std::string::npos)
        msz = strtoull(blk.substr(sq1 + 1, sq2 - sq1 - 1).c_str(), nullptr, 10);
    }
    if (mid != 0 && (best_depot == 0 || msz > best_sz)) {
      best_depot = did;
      best_manifest = mid;
      best_sz = msz;
    }
  }
  *out_depot = best_depot;
  *out_manifest = best_manifest;
  *out_size = best_sz;
}

struct depot_chunk_t {
  uint8_t sha[20];
  uint32_t crc = 0;      // Adler-32 of the decompressed bytes (zero seed)
  uint64_t offset = 0;
  uint32_t cb_orig = 0;
  uint32_t cb_comp = 0;
};

struct depot_file_t {
  std::string filename;
  uint64_t size = 0;
  uint32_t flags = 0;
  uint8_t content_sha[20]{};
  bool has_content_sha = false;
  bool linked_path = false;
  std::vector<depot_chunk_t> chunks;
};

// Steam supplies the complete signed query, including its parameter names.
std::string cdn_query(const std::string& token) {
  if (token.empty() || token.front() == '?') return token;
  return "?" + token;
}

bool parse_manifest_blob(const std::vector<uint8_t>& blob,
                         uint32_t expected_depot, uint64_t expected_manifest,
                         const uint8_t depot_key[32],
                         std::vector<depot_file_t>* files, std::string* err) {
  std::vector<uint8_t> inner;
  if (blob.size() >= 2 && blob[0] == 'P' && blob[1] == 'K') {
    if (!decompress_steam_payload(blob.data(), blob.size(), 0, &inner, err))
      return false;
  } else {
    inner = blob;
  }
  // Valve manifest container: magic(4) + len(4) + bytes
  const uint8_t* payload = nullptr;
  size_t payload_len = 0;
  uint64_t actual_depot = 0, actual_manifest = 0;
  bool filenames_encrypted = false;
  size_t off = 0;
  while (off + 8 <= inner.size()) {
    uint32_t magic = 0, blen = 0;
    std::memcpy(&magic, inner.data() + off, 4);
    std::memcpy(&blen, inner.data() + off + 4, 4);
    off += 8;
    if (magic == 0x32C415ABu) break; // EOF
    if (blen > inner.size() - off) {
      if (err) *err = "truncated manifest section";
      return false;
    }
    if (magic == 0x71F617D0u) {
      payload = inner.data() + off;
      payload_len = blen;
    } else if (magic == 0x1F4812BEu) {
      pb_reader mr{inner.data() + off, blen};
      uint32_t f, wt;
      while (mr.next(&f, &wt)) {
        if (f == 1 && wt == 0) {
          if (!mr.read_varint(&actual_depot)) return false;
        } else if (f == 2 && wt == 0) {
          if (!mr.read_varint(&actual_manifest)) return false;
        } else if (f == 4 && wt == 0) {
          uint64_t v = 0;
          if (!mr.read_varint(&v)) return false;
          filenames_encrypted = (v != 0);
        } else {
          if (!mr.skip(wt)) return false;
        }
      }
    }
    off += blen;
  }
  if (actual_depot != expected_depot || actual_manifest != expected_manifest) {
    if (err) *err = "depot or manifest ID differs from requested version";
    return false;
  }
  if (!payload || payload_len == 0) {
    if (err) *err = "manifest missing protobuf payload";
    return false;
  }

  pb_reader pr{payload, payload_len};
  uint32_t f, wt;
  while (pr.next(&f, &wt)) {
    if (f == 1 && wt == 2) {
      const uint8_t* fp = nullptr;
      size_t fl = 0;
      if (!pr.read_bytes(&fp, &fl)) return false;
      pb_reader fr{fp, fl};
      depot_file_t df;
      uint32_t ff, fwt;
      while (fr.next(&ff, &fwt)) {
        if (ff == 1 && fwt == 2) {
          const uint8_t* sp = nullptr;
          size_t sl = 0;
          if (!fr.read_bytes(&sp, &sl)) return false;
          df.filename.assign((const char*)sp, sl);
        } else if (ff == 2 && fwt == 0) {
          if (!fr.read_varint(&df.size)) return false;
        } else if (ff == 3 && fwt == 0) {
          uint64_t v = 0;
          if (!fr.read_varint(&v)) return false;
          df.flags = (uint32_t)v;
        } else if (ff == 5 && fwt == 2) {
          const uint8_t* hash = nullptr;
          size_t length = 0;
          if (!fr.read_bytes(&hash, &length)) return false;
          if (length == sizeof(df.content_sha)) {
            std::memcpy(df.content_sha, hash, length);
            df.has_content_sha = true;
          }
        } else if (ff == 6 && fwt == 2) {
          const uint8_t* cp = nullptr;
          size_t cl = 0;
          if (!fr.read_bytes(&cp, &cl)) return false;
          pb_reader cr{cp, cl};
          depot_chunk_t ck{};
          uint32_t cf, cwt;
          while (cr.next(&cf, &cwt)) {
            if (cf == 1 && cwt == 2) {
              const uint8_t* shp = nullptr;
              size_t shl = 0;
              if (!cr.read_bytes(&shp, &shl)) return false;
              if (shl == 20) std::memcpy(ck.sha, shp, 20);
            } else if (cf == 2 && cwt == 5) {
              uint32_t v = 0;
              if (!cr.read_fixed32(&v)) return false;
              ck.crc = v;
            } else if (cf == 3 && cwt == 0) {
              if (!cr.read_varint(&ck.offset)) return false;
            } else if (cf == 4 && cwt == 0) {
              uint64_t v = 0;
              if (!cr.read_varint(&v)) return false;
              ck.cb_orig = (uint32_t)v;
            } else if (cf == 5 && cwt == 0) {
              uint64_t v = 0;
              if (!cr.read_varint(&v)) return false;
              ck.cb_comp = (uint32_t)v;
            } else {
              if (!cr.skip(cwt)) return false;
            }
          }
          df.chunks.push_back(ck);
        } else if (ff == 7 && fwt == 2) {
          const uint8_t* target = nullptr;
          size_t size = 0;
          if (!fr.read_bytes(&target, &size)) return false;
          df.linked_path = size != 0;
        } else {
          if (!fr.skip(fwt)) return false;
        }
      }
      if (filenames_encrypted && !df.filename.empty()) {
        auto enc = b64_decode(df.filename);
        std::vector<uint8_t> dec;
        if (!bcrypt_aes256_decrypt_steam(enc.data(), enc.size(), depot_key, &dec)) {
          if (err) *err = "manifest filename decryption failed";
          return false;
        }
        while (!dec.empty() && dec.back() == 0) dec.pop_back();
        if (dec.empty()) return false;
        df.filename.assign((const char*)dec.data(), dec.size());
      }
      // Normalize path separators to backslash and strip trailing whitespace
      for (char& c : df.filename)
        if (c == '/') c = '\\';
      files->push_back(std::move(df));
    } else {
      if (!pr.skip(wt)) return false;
    }
  }
  return !files->empty();
}

void scan_installed_into_items(const std::string& games_dir,
                               std::vector<steam_library_item>* items) {
  std::unordered_map<uint32_t, std::string> local_dirs;
  WIN32_FIND_DATAA fd{};
  HANDLE h = FindFirstFileA((games_dir + "\\*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
      if (fd.cFileName[0] == '.') continue;
      std::string gdir = games_dir + "\\" + fd.cFileName;
      std::string appid_path = gdir + "\\steam_appid.txt";
      FILE* f = fopen(appid_path.c_str(), "rb");
      if (f) {
        char buf[64] = {};
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        if (n > 0) {
          uint32_t id = (uint32_t)strtoul(buf, nullptr, 10);
          if (id > 0) local_dirs[id] = gdir;
        }
      }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
  }

  std::vector<uint32_t> owned_ids;
  for (auto& it : *items) {
    owned_ids.push_back(it.app_id);
    auto found = local_dirs.find(it.app_id);
    if (found != local_dirs.end()) {
      it.installed = true;
      it.install_dir = found->second;
      it.download_pct = 1.0f;
      if (it.status_text.empty()) it.status_text = "Ready to Play";
      local_dirs.erase(found);
    } else if (!it.downloading) {
      it.installed = false;
    }
  }
  // Include any side-loaded Steam titles already on disk
  for (const auto& kv : local_dirs) {
    owned_ids.push_back(kv.first);
    steam_library_item extra;
    extra.app_id = kv.first;
    size_t sl = kv.second.rfind('\\');
    extra.title = (sl == std::string::npos) ? kv.second : kv.second.substr(sl + 1);
    extra.size_str = "Installed locally";
    extra.installed = true;
    extra.install_dir = kv.second;
    extra.download_pct = 1.0f;
    extra.status_text = "Ready to Play";
    items->push_back(std::move(extra));
  }
  trt_steam_set_owned_appids(owned_ids.data(), (int)owned_ids.size());
}

void fetch_remote_account_data(std::function<void()> on_library_updated) {
  std::string refresh_tok, access_tok, games_dir, state_dir;
  uint64_t sid = 0;
  std::string acct_name;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    refresh_tok = g_refresh_token;
    access_tok = g_access_token;
    sid = g_acct.steamid64;
    acct_name = g_acct.account_name;
    games_dir = g_games_dir;
    state_dir = g_state_dir;
  }
  if (sid == 0 && refresh_tok.empty()) return;

  // Refresh access token via protobuf GenerateAccessTokenForApp/v1 if needed
  auto refresh_access_token = [&]() -> bool {
    if (refresh_tok.empty() || sid == 0) return false;
    pb_writer req;
    req.write_string(1, refresh_tok);
    req.write_fixed64(2, sid);
    req.write_uint32(3, 0);  // renewal_type = k_ETokenRenewalType_None
    std::vector<uint8_t> resp;
    std::string err;
    if (!http_post_protobuf_steam(
            "https://api.steampowered.com/IAuthenticationService/GenerateAccessTokenForApp/v1/",
            req.buf, &resp, &err)) {
      log_line(("steam: GenerateAccessTokenForApp failed: " + err).c_str());
      return false;
    }
    pb_reader rd{resp.data(), resp.size()};
    uint32_t f = 0, wt = 0;
    std::string new_acc, new_ref;
    while (rd.next(&f, &wt)) {
      if (f == 1 && wt == 2) {
        const uint8_t* p = nullptr; size_t l = 0;
        if (rd.read_bytes(&p, &l)) new_acc.assign((const char*)p, l);
      } else if (f == 2 && wt == 2) {
        const uint8_t* p = nullptr; size_t l = 0;
        if (rd.read_bytes(&p, &l)) new_ref.assign((const char*)p, l);
      } else {
        rd.skip(wt);
      }
    }
    if (new_acc.empty()) return false;
    access_tok = new_acc;
    if (!new_ref.empty()) refresh_tok = new_ref;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_acct.steamid64 != sid) return false;
    g_access_token = access_tok;
    g_refresh_token = refresh_tok;
    save_credentials(state_dir, sid, acct_name, g_acct.persona, refresh_tok,
                     access_tok);
    log_line("steam: refreshed access_token via protobuf GenerateAccessTokenForApp");
    return true;
  };
  if (access_tok.empty()) refresh_access_token();

  // 1) Player summary (public Steam persona)
  std::string persona = acct_name;
  {
    std::string resp, err;
    std::string url =
        "https://api.steampowered.com/ISteamUser/GetPlayerSummaries/v2/?steamids=" +
        std::to_string(sid) +
        (access_tok.empty() ? "" : "&access_token=" + access_tok);
    if (http_get_text(url, &resp, &err)) {
      try {
        auto j = winrt::Windows::Data::Json::JsonObject::Parse(
            winrt::hstring(u8to16(resp)));
        auto players =
            j.GetNamedObject(L"response").GetNamedArray(L"players");
        if (players.Size() > 0) {
          auto p0 = players.GetObjectAt(0);
          if (p0.HasKey(L"personaname")) {
            persona = u16to8(std::wstring(p0.GetNamedString(L"personaname").c_str()));
          }
        }
      } catch (...) {
      }
    }
  }
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_acct.steamid64 != sid) return;
    if (!persona.empty()) g_acct.persona = persona;
    g_acct.signed_in = true;
    trt_steam_set_identity(std::to_string(sid).c_str(), g_acct.persona.c_str());
    save_credentials(state_dir, sid, acct_name, g_acct.persona, refresh_tok,
                     access_tok);
  }

  // 2) Owned Games (requires valid access_token; auto-refresh & retry once if stale)
  std::vector<steam_library_item> fetched_items;
  for (int attempt = 0; attempt < 2 && !access_tok.empty(); ++attempt) {
    std::string resp, err;
    std::string url =
        "https://api.steampowered.com/IPlayerService/GetOwnedGames/v1/?"
        "access_token=" +
        access_tok + "&steamid=" + std::to_string(sid) +
        "&include_appinfo=1&include_played_free_games=1&include_free_sub=1"
        "&skip_unvetted_apps=false";
    if (!http_get_text(url, &resp, &err)) {
      log_line(("steam: GetOwnedGames failed (" + err + "), retrying token refresh").c_str());
      if (attempt == 0 && refresh_access_token()) continue;
      break;
    }
    try {
      auto j = winrt::Windows::Data::Json::JsonObject::Parse(
          winrt::hstring(u8to16(resp)));
      auto r = j.GetNamedObject(L"response");
      if (r.HasKey(L"games")) {
        auto games = r.GetNamedArray(L"games");
        for (uint32_t i = 0; i < games.Size(); ++i) {
          auto g = games.GetObjectAt(i);
          steam_library_item it;
          it.app_id = (uint32_t)g.GetNamedNumber(L"appid");
          it.title = g.HasKey(L"name")
                         ? u16to8(std::wstring(g.GetNamedString(L"name").c_str()))
                         : ("App " + std::to_string(it.app_id));
          int mins = g.HasKey(L"playtime_forever")
                         ? (int)g.GetNamedNumber(L"playtime_forever")
                         : 0;
          it.size_str = mins > 0 ? (std::to_string(mins / 60) + "h played")
                                 : "Steam Purchase";
          fetched_items.push_back(std::move(it));
        }
        std::sort(fetched_items.begin(), fetched_items.end(),
                  [](const steam_library_item& a, const steam_library_item& b) {
                    return a.title < b.title;
                  });
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "steam: loaded %zu owned games for SteamID %llu",
                 fetched_items.size(), (unsigned long long)sid);
        log_line(msg);
      } else {
        log_line("steam: GetOwnedGames returned empty response.games");
      }
    } catch (...) {
      log_line("steam: GetOwnedGames JSON parse exception");
    }
    break;
  }

  std::vector<steam_library_item> family_items;
  const bool family_loaded =
      !access_tok.empty() && fetch_family_apps(sid, access_tok, &family_items);
  if (!family_loaded && !access_tok.empty())
    log_line("steam: Steam Families catalog unavailable");

  // 3) Friends List + Friend Summaries
  if (!access_tok.empty()) {
    std::string resp, err;
    std::string url =
        "https://api.steampowered.com/ISteamUser/GetFriendList/v1/?"
        "access_token=" +
        access_tok + "&steamid=" + std::to_string(sid) + "&relationship=friend";
    if (http_get_text(url, &resp, &err)) {
      try {
        auto j = winrt::Windows::Data::Json::JsonObject::Parse(
            winrt::hstring(u8to16(resp)));
        auto fl = j.GetNamedObject(L"friendslist").GetNamedArray(L"friends");
        std::string ids_csv;
        uint32_t n = std::min<uint32_t>(fl.Size(), 32);
        for (uint32_t i = 0; i < n; ++i) {
          auto fo = fl.GetObjectAt(i);
          if (i > 0) ids_csv += ',';
          ids_csv += u16to8(std::wstring(fo.GetNamedString(L"steamid").c_str()));
        }
        if (!ids_csv.empty()) {
          std::string sresp, serr;
          std::string surl =
              "https://api.steampowered.com/ISteamUser/GetPlayerSummaries/v2/?"
              "access_token=" +
              access_tok + "&steamids=" + ids_csv;
          if (http_get_text(surl, &sresp, &serr)) {
            auto sj = winrt::Windows::Data::Json::JsonObject::Parse(
                winrt::hstring(u8to16(sresp)));
            auto players =
                sj.GetNamedObject(L"response").GetNamedArray(L"players");
            std::vector<trt_steam_friend_info> finfos;
            for (uint32_t i = 0; i < players.Size() && finfos.size() < 64; ++i) {
              auto p = players.GetObjectAt(i);
              trt_steam_friend_info fi{};
              fi.steam_id = strtoull(
                  u16to8(std::wstring(p.GetNamedString(L"steamid").c_str()))
                      .c_str(),
                  nullptr, 10);
              std::string fn = u16to8(
                  std::wstring(p.GetNamedString(L"personaname").c_str()));
              snprintf(fi.persona, sizeof(fi.persona), "%s", fn.c_str());
              fi.state = p.HasKey(L"personastate")
                             ? (int)p.GetNamedNumber(L"personastate")
                             : 0;
              if (p.HasKey(L"gameextrainfo")) {
                std::string gn = u16to8(
                    std::wstring(p.GetNamedString(L"gameextrainfo").c_str()));
                snprintf(fi.game_name, sizeof(fi.game_name), "%s", gn.c_str());
                fi.state = 6;
              }
              finfos.push_back(fi);
            }
            // Sort online/in-game friends first
            std::sort(finfos.begin(), finfos.end(),
                      [](const trt_steam_friend_info& a,
                         const trt_steam_friend_info& b) {
                        int sa = (a.game_name[0] ? 2 : (a.state > 0 ? 1 : 0));
                        int sb = (b.game_name[0] ? 2 : (b.state > 0 ? 1 : 0));
                        return sa > sb;
                      });
            trt_steam_set_friends(finfos.data(), (int)finfos.size());
          }
        }
      } catch (...) {
      }
    }
  }

  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_acct.steamid64 != sid || !g_acct.signed_in) return;
    for (auto& item : family_items) {
      const auto prior = std::find_if(g_family_items.begin(), g_family_items.end(),
                                     [&item](const auto& row) { return row.app_id == item.app_id; });
      if (prior == g_family_items.end()) continue;
      item.downloading = prior->downloading;
      item.download_pct = prior->download_pct;
      item.status_text = prior->status_text;
    }
    g_family_items = std::move(family_items);
    scan_installed_into_items(games_dir, &fetched_items);
    if (!fetched_items.empty()) g_items = std::move(fetched_items);
    else scan_installed_into_items(games_dir, &g_items);
    g_acct.status_line = "steam: signed in as " +
                         (g_acct.persona.empty() ? g_acct.account_name
                                                 : g_acct.persona) +
                         " (" + std::to_string(g_items.size()) + " titles)";
    if (!family_loaded && !access_tok.empty())
      g_acct.status_line += " · Steam Families unavailable";
  }
  if (on_library_updated) on_library_updated();
}

} // namespace

void steam_client_init(const std::string& state_dir,
                       const std::string& games_dir) {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_state_dir = state_dir;
    g_games_dir = games_dir;
    g_acct = {};
    g_acct.status_line = "press Y to sign in with Steam QR";
  }

  // Load persisted Steam account token if available
  FILE* fp = fopen(token_file_path(state_dir).c_str(), "rb");
  if (fp) {
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::string raw;
    if (sz > 0 && sz < 65536) {
      raw.resize((size_t)sz);
      fread(&raw[0], 1, (size_t)sz, fp);
    }
    fclose(fp);
    if (!raw.empty() && raw[0] == '{') {
      try {
        auto obj = winrt::Windows::Data::Json::JsonObject::Parse(
            winrt::hstring(u8to16(raw)));
        std::lock_guard<std::mutex> lk(g_mu);
        if (obj.HasKey(L"refresh_token"))
          g_refresh_token =
              u16to8(std::wstring(obj.GetNamedString(L"refresh_token").c_str()));
        if (obj.HasKey(L"access_token"))
          g_access_token =
              u16to8(std::wstring(obj.GetNamedString(L"access_token").c_str()));
        if (obj.HasKey(L"account_name"))
          g_acct.account_name =
              u16to8(std::wstring(obj.GetNamedString(L"account_name").c_str()));
        if (obj.HasKey(L"persona"))
          g_acct.persona =
              u16to8(std::wstring(obj.GetNamedString(L"persona").c_str()));
        if (obj.HasKey(L"steamid64"))
          g_acct.steamid64 = strtoull(
              u16to8(std::wstring(obj.GetNamedString(L"steamid64").c_str()))
                  .c_str(),
              nullptr, 10);
      } catch (...) {
      }
    } else if (!raw.empty()) {
      std::lock_guard<std::mutex> lk(g_mu);
      g_refresh_token = raw;
      g_acct.steamid64 = extract_steamid_from_jwt(raw);
    }
  }

  trt_steam_identity id{};
  if (trt_steam_auth_load(state_dir, &id) && id.signed_in) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_acct.steamid64 == 0)
      g_acct.steamid64 = strtoull(id.steamid64.c_str(), nullptr, 10);
    if (g_acct.persona.empty()) g_acct.persona = id.persona;
  }

  bool do_refresh = false;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    scan_installed_into_items(games_dir, &g_items);
    if (g_acct.steamid64 != 0 || !g_refresh_token.empty()) {
      if (g_acct.steamid64 == 0)
        g_acct.steamid64 = extract_steamid_from_jwt(g_refresh_token);
      g_acct.signed_in = (g_acct.steamid64 != 0);
      if (g_acct.signed_in) {
        trt_steam_set_identity(std::to_string(g_acct.steamid64).c_str(),
                               g_acct.persona.empty() ? g_acct.account_name.c_str()
                                                      : g_acct.persona.c_str());
        g_acct.status_line = "steam: signed in as " +
                             (g_acct.persona.empty() ? std::to_string(g_acct.steamid64)
                                                     : g_acct.persona);
        do_refresh = true;
      }
    }
  }
  if (do_refresh) {
    std::thread([] { fetch_remote_account_data(nullptr); }).detach();
  }
}

void steam_client_sign_out() {
  std::lock_guard<std::mutex> lk(g_mu);
  DeleteFileA(token_file_path(g_state_dir).c_str());
  trt_steam_auth_sign_out(g_state_dir);
  trt_steam_set_identity(nullptr, nullptr);
  trt_steam_set_friends(nullptr, 0);
  g_refresh_token.clear();
  g_access_token.clear();
  g_acct = {};
  g_acct.status_line = "steam: signed out — press Y to sign in";
  g_items.clear();
  g_family_items.clear();
  scan_installed_into_items(g_games_dir, &g_items);
}

void steam_client_rescan_local() {
  std::lock_guard<std::mutex> lk(g_mu);
  scan_installed_into_items(g_games_dir, &g_items);
}

void steam_client_refresh_remote(std::function<void()> on_library_updated) {
  std::thread([cb = std::move(on_library_updated)]() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    fetch_remote_account_data(cb);
  }).detach();
}

void steam_client_begin_qr(std::function<void()> on_library_updated) {
  if (g_auth_busy.exchange(true)) return;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_acct.busy = true;
    g_acct.status_line = "steam: requesting QR login challenge…";
  }
  std::thread([cb = std::move(on_library_updated)]() {
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);

      // CAuthentication_BeginAuthSessionViaQR_Request (binary protobuf)
      pb_writer dev;
      dev.write_string(1, "Tiramisu Xbox");
      dev.write_uint32(2, 1);   // platform_type = SteamClient
      dev.write_uint32(3, 16);  // os_type = Win10/11
      pb_writer req;
      req.write_string(1, "Tiramisu Xbox");
      req.write_uint32(2, 1);   // platform_type = SteamClient
      req.write_bytes(3, dev.buf.data(), dev.buf.size());

      std::vector<uint8_t> resp;
      std::string err;
      if (!http_post_protobuf_steam(
              "https://api.steampowered.com/IAuthenticationService/BeginAuthSessionViaQR/v1/",
              req.buf, &resp, &err)) {
        log_line(("steam: BeginAuthSessionViaQR failed: " + err).c_str());
        std::lock_guard<std::mutex> lk(g_mu);
        g_acct.status_line = "steam QR request failed: " + err;
        g_acct.busy = false;
        g_auth_busy = false;
        return;
      }

      uint64_t client_id = 0;
      std::string challenge;
      std::vector<uint8_t> request_id;
      float interval_sec = 5.0f;
      {
        pb_reader rd{resp.data(), resp.size()};
        uint32_t f = 0, wt = 0;
        while (rd.next(&f, &wt)) {
          if (f == 1 && wt == 0) {
            rd.read_varint(&client_id);
          } else if (f == 2 && wt == 2) {
            const uint8_t* p = nullptr; size_t l = 0;
            if (rd.read_bytes(&p, &l)) challenge.assign((const char*)p, l);
          } else if (f == 3 && wt == 2) {
            const uint8_t* p = nullptr; size_t l = 0;
            if (rd.read_bytes(&p, &l)) request_id.assign(p, p + l);
          } else if (f == 4 && wt == 5 && rd.pos + 4 <= rd.len) {
            std::memcpy(&interval_sec, rd.p + rd.pos, 4);
            rd.pos += 4;
          } else {
            rd.skip(wt);
          }
        }
      }
      if (client_id == 0 || challenge.empty() || request_id.empty()) {
        log_line("steam: BeginAuthSessionViaQR returned incomplete protobuf");
        std::lock_guard<std::mutex> lk(g_mu);
        g_acct.status_line = "steam QR decode error — press Y to retry";
        g_acct.busy = false;
        g_auth_busy = false;
        return;
      }
      {
        char msg[192];
        snprintf(msg, sizeof(msg), "steam: QR ready (%s)", challenge.c_str());
        log_line(msg);
        std::lock_guard<std::mutex> lk(g_mu);
        g_acct.qr_url = challenge;
        g_acct.status_line = "Scan QR with the Steam Mobile app to sign in";
      }

      const DWORD poll_ms =
          (DWORD)std::clamp((int)(interval_sec * 1000.0f), 3000, 7000);
      for (int i = 0; i < 45; ++i) {
        Sleep(poll_ms);
        pb_writer preq;
        preq.write_uint64(1, client_id);
        preq.write_bytes(2, request_id.data(), request_id.size());
        std::vector<uint8_t> presp;
        std::string perr;
        if (!http_post_protobuf_steam(
                "https://api.steampowered.com/IAuthenticationService/PollAuthSessionStatus/v1/",
                preq.buf, &presp, &perr)) {
          continue;
        }
        uint64_t new_cid = 0;
        std::string new_url, ref, acc, aname;
        pb_reader prd{presp.data(), presp.size()};
        uint32_t pf = 0, pwt = 0;
        while (prd.next(&pf, &pwt)) {
          if (pf == 1 && pwt == 0) {
            prd.read_varint(&new_cid);
          } else if (pf == 2 && pwt == 2) {
            const uint8_t* p = nullptr; size_t l = 0;
            if (prd.read_bytes(&p, &l)) new_url.assign((const char*)p, l);
          } else if (pf == 3 && pwt == 2) {
            const uint8_t* p = nullptr; size_t l = 0;
            if (prd.read_bytes(&p, &l)) ref.assign((const char*)p, l);
          } else if (pf == 4 && pwt == 2) {
            const uint8_t* p = nullptr; size_t l = 0;
            if (prd.read_bytes(&p, &l)) acc.assign((const char*)p, l);
          } else if (pf == 6 && pwt == 2) {
            const uint8_t* p = nullptr; size_t l = 0;
            if (prd.read_bytes(&p, &l)) aname.assign((const char*)p, l);
          } else {
            prd.skip(pwt);
          }
        }
        if (new_cid != 0) client_id = new_cid;
        if (!new_url.empty()) {
          std::lock_guard<std::mutex> lk(g_mu);
          g_acct.qr_url = new_url;
        }
        if (!ref.empty()) {
          uint64_t sid = extract_steamid_from_jwt(acc.empty() ? ref : acc);
          if (sid == 0) sid = extract_steamid_from_jwt(ref);
          char okmsg[192];
          snprintf(okmsg, sizeof(okmsg),
                   "steam: signed in! account=%s steamid=%llu",
                   aname.c_str(), (unsigned long long)sid);
          log_line(okmsg);
          {
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_acct.steamid64 != sid) {
              g_items.clear();
              g_family_items.clear();
            }
            g_refresh_token = ref;
            g_access_token = acc;
            g_acct.steamid64 = sid;
            g_acct.account_name = aname;
            if (g_acct.persona.empty()) g_acct.persona = aname;
            g_acct.signed_in = (sid != 0);
            g_acct.qr_url.clear();
            g_acct.status_line = "Signed in — loading your Steam library…";
            save_credentials(g_state_dir, sid, aname, g_acct.persona, ref, acc);
          }
          fetch_remote_account_data(cb);
          std::lock_guard<std::mutex> lk(g_mu);
          g_acct.busy = false;
          g_auth_busy = false;
          return;
        }
      }
      std::lock_guard<std::mutex> lk(g_mu);
      g_acct.qr_url.clear();
      g_acct.status_line = "Steam QR expired — press Y to generate a new code";
    } catch (winrt::hresult_error const& e) {
      std::lock_guard<std::mutex> lk(g_mu);
      g_acct.qr_url.clear();
      g_acct.status_line = "steam auth error: " +
                           u16to8(std::wstring(e.message().c_str()));
    } catch (...) {
      std::lock_guard<std::mutex> lk(g_mu);
      g_acct.qr_url.clear();
      g_acct.status_line = "steam auth error";
    }
    std::lock_guard<std::mutex> lk(g_mu);
    g_acct.busy = false;
    g_auth_busy = false;
  }).detach();
}

void steam_client_download_app(uint32_t app_id, bool family_shared,
                               std::function<void()> on_complete) {
  std::string ref_tok, acc_tok, acct_name, games_dir, state_dir, title;
  uint64_t sid = 0;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    sid = g_acct.steamid64;
    if (!g_acct.signed_in || g_refresh_token.empty() || sid == 0) {
      g_acct.status_line = "download blocked: sign in with Steam (Y) first";
      return;
    }
    const auto& items = family_shared ? g_family_items : g_items;
    const auto found = std::find_if(items.begin(), items.end(), [app_id](const auto& item) {
      return item.app_id == app_id;
    });
    if (found == items.end()) {
      g_acct.status_line = "Steam library changed — refresh and select the game again";
      return;
    }
    if (g_dl_busy.exchange(true)) {
      g_acct.status_line = "steam: another download is already active";
      return;
    }
    ref_tok = g_refresh_token;
    acc_tok = g_access_token;
    acct_name = g_acct.account_name;
    games_dir = g_games_dir;
    state_dir = g_state_dir;
    title = found->title;
  }
  if (title.empty()) title = "SteamApp_" + std::to_string(app_id);

  auto set_item_state = [app_id, family_shared, sid](
                            bool downloading, float pct, const std::string& st,
                            bool installed = false, const std::string& idir = "") {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_acct.signed_in || g_acct.steamid64 != sid) return;
    auto& items = family_shared ? g_family_items : g_items;
    for (auto& it : items) {
      if (it.app_id != app_id) continue;
      it.downloading = downloading;
      it.download_pct = pct;
      it.status_text = st;
      if (installed) {
        it.installed = true;
        if (!idir.empty()) it.install_dir = idir;
      }
      break;
    }
    g_acct.status_line = st;
  };

  set_item_state(true, 0.02f, "steam: connecting to CM servers…");
  std::thread([=]() {
    std::filesystem::path family_partial;
    const auto family_account_active = [family_shared, sid, app_id]() {
      if (!family_shared) return true;
      std::lock_guard<std::mutex> lk(g_mu);
      return g_acct.signed_in && g_acct.steamid64 == sid &&
             std::any_of(g_family_items.begin(), g_family_items.end(),
                         [app_id](const auto& item) { return item.app_id == app_id; });
    };
    auto fail = [&](const std::string& msg) {
      log_line(("steam_dl[" + std::to_string(app_id) + "]: FAIL " + msg).c_str());
      set_item_state(false, 0.0f, msg);
      g_dl_busy = false;
    };
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);

      // 1) Fetch WebSocket CM servers
      std::vector<std::string> cm_hosts;
      {
        std::string resp, err;
        if (http_get_text(
                "https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/?cmtype=websockets&cellid=0",
                &resp, &err)) {
          try {
            auto j = winrt::Windows::Data::Json::JsonObject::Parse(
                winrt::hstring(u8to16(resp)));
            auto sl =
                j.GetNamedObject(L"response").GetNamedArray(L"serverlist");
            for (uint32_t i = 0; i < sl.Size() && cm_hosts.size() < 6; ++i) {
              auto s = sl.GetObjectAt(i);
              if (s.HasKey(L"endpoint")) {
                cm_hosts.push_back(
                    u16to8(std::wstring(s.GetNamedString(L"endpoint").c_str())));
              }
            }
          } catch (...) {
          }
        }
      }
      if (cm_hosts.empty()) {
        return fail("no Steam CM WebSocket servers available");
      }

      // 2) Connect CM WebSocket
      auto cm = std::make_shared<cm_session>();
      bool connected = false;
      std::string cm_err;
      for (const auto& h : cm_hosts) {
        if (cm->connect(h, &cm_err)) {
          connected = true;
          break;
        }
      }
      if (!connected) return fail("CM connect failed: " + cm_err);

      // 3) ClientHello + ClientLogon
      {
        pb_writer hello;
        hello.write_uint32(1, 65581); // protocol_version
        cm->send_proto(9805 /* ClientHello */, {}, hello);

        pb_writer logon;
        logon.write_uint32(1, 65581);              // protocol_version
        logon.write_uint32(5, 1771);               // client_package_version
        logon.write_string(6, "english");          // client_language
        logon.write_uint32(7, 16);                 // client_os_type = Win10/11
        logon.write_string(50, acct_name);         // account_name
        logon.write_string(96, "tiramisu-xbox");   // machine_name
        logon.write_uint32(102, 1);                // supports_rate_limit_response
        logon.write_string(108, ref_tok);          // access_token = refresh token
        pb_writer hdr;
        cm->steamid = sid;
        cm->send_proto(5514 /* ClientLogon */, hdr, logon);

        std::vector<uint8_t> resp;
        if (!cm->recv_emsg(751 /* ClientLogOnResponse */, &resp)) {
          return fail("Steam CM logon timeout");
        }
        uint32_t eresult = 0;
        pb_reader lr{resp.data(), resp.size()};
        uint32_t f, wt;
        while (lr.next(&f, &wt)) {
          if (f == 1 && wt == 0) {
            uint64_t v = 0;
            lr.read_varint(&v);
            eresult = (uint32_t)v;
          } else {
            lr.skip(wt);
          }
        }
        if (eresult != 1) {
          return fail("Steam CM logon denied (eresult=" +
                      std::to_string(eresult) + ")");
        }
      }

      set_item_state(true, 0.06f, "steam: fetching appinfo & Windows depot…");

      // 4) ClientPICSProductInfoRequest
      std::string appinfo_vdf;
      {
        pb_writer app_msg;
        app_msg.write_uint32(1, app_id);
        pb_writer req;
        // ClientPICSProductInfoRequest: field 2 is `apps` (field 1 is
        // `packages` per SteamKit's CMsgClientPICSProductInfoRequest).
        req.write_bytes(2, app_msg.buf.data(), app_msg.buf.size());
        // SteamKit sets SourceJobID on PICS requests; without it the CM
        // answers with the appid in unknown_appids instead of appinfo.
        cm->send_proto(8903 /* ClientPICSProductInfoRequest */, {}, req, 1);

        std::vector<uint8_t> resp;
        if (!cm->recv_emsg(8904 /* ClientPICSProductInfoResponse */, &resp)) {
          return fail("appinfo query timeout");
        }
        {
          std::string hex;
          char b[4];
          for (size_t i = 0; i < resp.size() && i < 24; ++i) {
            snprintf(b, sizeof(b), "%02x", resp[i]);
            hex += b;
          }
          log_line(("steam_dl[" + std::to_string(app_id) +
                    "]: PICS resp bytes=" + std::to_string(resp.size()) +
                    " head=" + hex).c_str());
        }
        pb_reader rr{resp.data(), resp.size()};
        uint32_t f, wt;
        while (rr.next(&f, &wt)) {
          log_line(("steam_dl[" + std::to_string(app_id) +
                    "]: PICS field=" + std::to_string(f) +
                    " wt=" + std::to_string(wt)).c_str());
          if (f == 1 && wt == 2) {
            const uint8_t* ap = nullptr;
            size_t al = 0;
            rr.read_bytes(&ap, &al);
            log_line(("steam_dl[" + std::to_string(app_id) +
                      "]: PICS app msg bytes=" + std::to_string(al)).c_str());
            pb_reader ar{ap, al};
            uint32_t af, awt;
            while (ar.next(&af, &awt)) {
              // SteamKit response AppInfo: field 4 = sha, field 5 = buffer
              // (the appinfo VDF). Older docs list it as field 6; accept both.
              if ((af == 5 || af == 6) && awt == 2) {
                const uint8_t* bv = nullptr;
                size_t bl = 0;
                ar.read_bytes(&bv, &bl);
                appinfo_vdf.assign((const char*)bv, bl);
              } else {
                log_line(("steam_dl[" + std::to_string(app_id) +
                          "]: PICS app field=" + std::to_string(af) +
                          " wt=" + std::to_string(awt)).c_str());
                ar.skip(awt);
              }
            }
          } else {
            rr.skip(wt);
          }
        }
      }

      std::string main_exe_rel;
      std::vector<trt_steam_depot> depots;
      if (family_shared) {
        depots = trt_steam_family_depots(appinfo_vdf);
      } else {
        trt_steam_depot selected{};
        parse_appinfo_vdf(appinfo_vdf, &selected.id, &selected.manifest,
                          &selected.size, &main_exe_rel);
        if (selected.id && selected.manifest) depots.push_back(selected);
      }
      if (depots.empty()) return fail("No supported Windows depot for " + title);
      std::string install_dir = games_dir + "\\" + sanitize_dirname(title);
      std::filesystem::path family_root;
      if (family_shared) {
        pending_removal.clear();
        if (state_dir.empty()) return fail("Steam Families storage unavailable");
        family_root = std::filesystem::path(u8to16(state_dir)) / "steam-family" /
                      std::to_string(sid) / std::to_string(app_id);
        uint64_t required = 0;
        std::string plan = std::to_string(sid) + "/" + std::to_string(app_id);
        for (const auto& depot : depots) {
          auto name = std::to_string(depot.id) + "-" + std::to_string(depot.manifest);
          if (std::filesystem::is_directory(family_root / name)) continue;
          if (!depot.size || depot.size > UINT64_MAX - required)
            return fail("Steam did not provide a usable installed size");
          uint64_t remaining = depot.size;
          const auto partial = family_root / (name + ".partial");
          if (std::filesystem::is_directory(partial)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(partial)) {
              if (entry.is_regular_file())
                remaining -= std::min<uint64_t>(remaining, entry.file_size());
            }
          }
          required += remaining;
          plan += "/" + name;
        }
        ULARGE_INTEGER available{};
        if (!GetDiskFreeSpaceExW(u8to16(state_dir).c_str(), &available, nullptr, nullptr))
          return fail("Cannot read available internal storage");
        if (required > available.QuadPart) {
          pending_space_plan.clear();
          return fail("Storage short by " + format_bytes(required - available.QuadPart) +
                      " — required " + format_bytes(required));
        }
        if (required && pending_space_plan != plan) {
          pending_space_plan = plan;
          set_item_state(false, 0, "Required " + format_bytes(required) + ", free " +
                         format_bytes(available.QuadPart) + ". Select Download again to start.");
          g_dl_busy = false;
          return;
        }
        pending_space_plan.clear();
      }
      for (size_t index = 0; index < depots.size(); ++index) {
        const auto& depot = depots[index];
        const auto depot_id = depot.id;
        const auto manifest_id = depot.manifest;
        const auto depot_sz = depot.size;
        const auto report = [&](float progress, const std::string& message) {
          const auto prefix = depots.size() > 1
              ? "Depot " + std::to_string(index + 1) + "/" + std::to_string(depots.size()) + ": "
              : std::string();
          set_item_state(true, 0.06f + 0.91f * (index + progress) / depots.size(), prefix + message);
        };
        // 5) Depot decryption key (proves ownership!)
        report(0.10f, "steam: requesting depot license key…");
        uint8_t depot_key[32] = {};
        {
          pb_writer kreq;
          kreq.write_uint32(1, depot_id);
          kreq.write_uint32(2, app_id);
          cm->send_proto(5438 /* ClientGetDepotDecryptionKey */, {}, kreq);

          std::vector<uint8_t> resp;
          if (!cm->recv_emsg(5439 /* ClientGetDepotDecryptionKeyResponse */,
                            &resp)) {
            return fail("depot decryption key timeout");
          }
          uint32_t eresult = 0;
          uint64_t key_depot = 0;
          bool got_key = false;
          pb_reader kr{resp.data(), resp.size()};
          uint32_t f, wt;
          while (kr.next(&f, &wt)) {
            if (f == 1 && wt == 0) {
              uint64_t v = 0;
              kr.read_varint(&v);
              eresult = (uint32_t)v;
            } else if (f == 2 && wt == 0) {
              if (!kr.read_varint(&key_depot)) return fail("Invalid depot key response");
            } else if (f == 3 && wt == 2) {
              const uint8_t* kp = nullptr;
              size_t kl = 0;
              kr.read_bytes(&kp, &kl);
              if (kl == 32) {
                std::memcpy(depot_key, kp, 32);
                got_key = true;
              }
            } else {
              kr.skip(wt);
            }
          }
          if (eresult == 1 && key_depot != depot_id) return fail("Depot key response ID mismatch");
          if (eresult != 1 || !got_key) {
            return fail("Steam denied depot key (eresult=" +
                        std::to_string(eresult) + " — verify license)");
          }
        }

        // 6) CDN hosts, manifest request code, and per-host auth tokens
        std::vector<std::string> cdn_hosts;
        std::vector<std::string> cdn_vhosts;
        {
          std::string resp, err;
          if (http_get_text(
                  "https://api.steampowered.com/IContentServerDirectoryService/GetServersForSteamPipe/v1/?cell_id=0",
                  &resp, &err)) {
            try {
              auto j = winrt::Windows::Data::Json::JsonObject::Parse(
                  winrt::hstring(u8to16(resp)));
              auto svs =
                  j.GetNamedObject(L"response").GetNamedArray(L"servers");
              for (uint32_t i = 0; i < svs.Size() && cdn_hosts.size() < 4; ++i) {
                auto s = svs.GetObjectAt(i);
                if (s.HasKey(L"host")) {
                  std::string h =
                      u16to8(std::wstring(s.GetNamedString(L"host").c_str()));
                  std::string vh = h;
                  if (s.HasKey(L"vhost")) {
                    vh = u16to8(
                        std::wstring(s.GetNamedString(L"vhost").c_str()));
                  }
                  cdn_hosts.push_back(h);
                  cdn_vhosts.push_back(vh);
                }
              }
            } catch (...) {
            }
          }
        }
        if (cdn_hosts.empty()) {
          cdn_hosts.push_back("cache1-iad1.steamcontent.com");
          cdn_vhosts.push_back(cdn_hosts.back());
        }

        // The manifest request code is granted by the CM against the logged-on
        // session. The old WebAPI GET returned an empty code, which is why the
        // CDN answered 401.
        uint64_t manifest_code = 0;
        {
          pb_writer mreq;
          mreq.write_uint32(1, app_id);
          mreq.write_uint32(2, depot_id);
          mreq.write_uint64(3, manifest_id);
          // Public branch is represented by omitting app_branch; SteamKit
          // explicitly clears "public" because the CM expects it absent.
          std::vector<uint8_t> mresp;
          std::string merr;
          if (cm->unified_call(
                  "ContentServerDirectory.GetManifestRequestCode#1", mreq,
                  &mresp, &merr, family_shared)) {
            pb_reader mr{mresp.data(), mresp.size()};
            uint32_t f = 0, wt = 0;
            while (mr.next(&f, &wt)) {
              if (f == 1 && wt == 0) {
                uint64_t v = 0;
                mr.read_varint(&v);
                manifest_code = v;
              } else {
                mr.skip(wt);
              }
            }
          } else {
            log_line(("steam_dl[" + std::to_string(app_id) +
                      "]: manifest request code failed: " + merr)
                         .c_str());
          }
        }

        if (family_shared && manifest_code == 0)
          return fail("Steam did not authorize a manifest for this family copy");
        if (!family_account_active())
          return fail("Steam Families access changed during the download");

        // Tokens are per depot and per CDN host. They are often empty, but
        // requesting one also tells the cache this session is authorized.
        std::vector<std::string> cdn_tokens;
        for (size_t i = 0; i < cdn_hosts.size(); ++i) {
          std::string token, terr;
          pb_writer treq;
          treq.write_uint32(1, depot_id);
          treq.write_string(2, cdn_vhosts[i]);
          treq.write_uint32(3, app_id);
          std::vector<uint8_t> tresp;
          if (cm->unified_call("ContentServerDirectory.GetCDNAuthToken#1",
                               treq, &tresp, &terr)) {
            pb_reader tr{tresp.data(), tresp.size()};
            uint32_t f = 0, wt = 0;
            while (tr.next(&f, &wt)) {
              if (f == 1 && wt == 2) {
                const uint8_t* p = nullptr;
                size_t n = 0;
                if (tr.read_bytes(&p, &n)) token.assign((const char*)p, n);
              } else {
                tr.skip(wt);
              }
            }
          }
          cdn_tokens.push_back(token);
          std::string tstat = token.empty() ? "none" : "ok";
          if (token.empty() && !terr.empty()) tstat += " (" + terr + ")";
          log_line(("steam_dl[" + std::to_string(app_id) + "]: cdn token " +
                    cdn_vhosts[i] + " -> " + tstat)
                       .c_str());
        }

        log_line(("steam_dl[" + std::to_string(app_id) +
                  "]: manifest authorized=" + (manifest_code ? "yes" : "no") +
                  " cdn=" + (cdn_hosts.empty() ? "none" : cdn_hosts[0])).c_str());

        log_line(("steam_dl[" + std::to_string(app_id) +
                  "]: depot=" + std::to_string(depot_id) +
                  " manifest=" + std::to_string(manifest_id) +
                  " size=" + std::to_string(depot_sz) +
                  " main=" + main_exe_rel).c_str());
        // 7) Download + parse depot manifest
        report(0.14f, "steam: downloading depot manifest…");
        std::vector<uint8_t> manifest_blob;
        std::string dl_err;
        for (size_t hi = 0; hi < cdn_hosts.size(); ++hi) {
          std::string url = "https://" + cdn_vhosts[hi] + "/depot/" +
                            std::to_string(depot_id) + "/manifest/" +
                            std::to_string(manifest_id) + "/5";
          if (manifest_code != 0) url += "/" + std::to_string(manifest_code);
          if (hi < cdn_tokens.size() && !cdn_tokens[hi].empty()) {
            url += cdn_query(cdn_tokens[hi]);
          }
          if (http_get_bytes(url, &manifest_blob, &dl_err)) break;
            log_line(("steam_dl[" + std::to_string(app_id) + "]: manifest CDN try " +
                    cdn_hosts[hi] + " -> " + dl_err).c_str());
        }
        if (manifest_blob.empty()) return fail("manifest CDN failed: " + dl_err);

        std::vector<depot_file_t> files;
        std::string perr;
        if (!parse_manifest_blob(manifest_blob, depot_id, manifest_id, depot_key, &files, &perr)) {
          return fail("manifest parse failed: " + perr);
        }

        uint64_t total_bytes = 0;
        for (const auto& f : files) total_bytes += f.size;
        if (total_bytes == 0) total_bytes = 1;

        std::filesystem::path family_final;
        if (family_shared) {
          if (state_dir.empty()) return fail("Steam Families staging storage unavailable");
          for (auto& file : files) {
            if (file.linked_path || !valid_family_depot_path(file.filename))
              return fail("Steam Families depot contains an unsupported file path");
            if (file.flags & 0x40) continue;
            std::sort(file.chunks.begin(), file.chunks.end(),
                      [](const auto& a, const auto& b) { return a.offset < b.offset; });
            uint64_t next_offset = 0;
            for (const auto& chunk : file.chunks) {
              if (chunk.offset != next_offset || !chunk.cb_orig ||
                  chunk.cb_orig > file.size - next_offset)
                return fail("Steam Families depot contains an invalid file layout");
              next_offset += chunk.cb_orig;
            }
            if (next_offset != file.size)
              return fail("Steam Families depot contains an incomplete file");
          }
          family_final = family_root / (std::to_string(depot_id) + "-" + std::to_string(manifest_id));
          if (std::filesystem::is_directory(family_final)) {
            if (!family_account_active())
              return fail("Steam Families access changed during the download");
            continue;
          }
          family_partial = family_final;
          family_partial += ".partial";
          std::filesystem::create_directories(family_partial);
        } else {
          ensure_dir_recursive(install_dir);
        }

        // 8) Stream every chunk, AES-decrypt, decompress, Adler-32 verify, write
        uint64_t done_bytes = 0;
        for (const auto& f : files) {
          if (!family_account_active())
            return fail("Steam Families access changed during the download");
          std::filesystem::path family_file;
          if (family_shared) family_file = family_partial / u8to16(f.filename);
          std::string fpath = install_dir + "\\" + f.filename;
          if (f.flags & 0x40 /* directory flag */) {
            if (family_shared) std::filesystem::create_directories(family_file);
            else ensure_dir_recursive(fpath);
            continue;
          }
          if (family_shared) {
            std::filesystem::create_directories(family_file.parent_path());
            if (f.has_content_sha && std::filesystem::is_regular_file(family_file) &&
                std::filesystem::file_size(family_file) == f.size) {
              std::ifstream saved(family_file, std::ios::binary);
              if (saved && steam_file_matches(saved, f.size, f.content_sha)) {
                done_bytes += f.size;
                continue;
              }
            }
          } else {
            const size_t sl = fpath.rfind('\\');
            if (sl != std::string::npos) ensure_dir_recursive(fpath.substr(0, sl));
          }

          std::unique_ptr<FILE, decltype(&fclose)> fp(
              family_shared ? _wfopen(family_file.c_str(), L"wb") : fopen(fpath.c_str(), "wb"),
              &fclose);
          if (!fp) return fail("cannot create file: " + f.filename);

          for (const auto& ck : f.chunks) {
            if (!family_account_active()) {
              fp.reset();
              return fail("Steam Families access changed during the download");
            }
            char hex_sha[41];
            for (int i = 0; i < 20; ++i)
              snprintf(hex_sha + i * 2, 3, "%02x", ck.sha[i]);

            std::vector<uint8_t> enc_chunk;
            std::string cerr;
            bool got = false;
            for (size_t hi = 0; hi < cdn_hosts.size(); ++hi) {
              std::string url = "https://" + cdn_vhosts[hi] + "/depot/" +
                                std::to_string(depot_id) + "/chunk/" + hex_sha;
              if (hi < cdn_tokens.size() && !cdn_tokens[hi].empty()) {
                url += cdn_query(cdn_tokens[hi]);
              }
              if (http_get_bytes(url, &enc_chunk, &cerr)) {
                got = true;
                break;
              }
            }
            if (!got) {
              fp.reset();
              return fail("chunk CDN failed (" + std::string(hex_sha) + "): " +
                          cerr);
            }

            if (ck.cb_comp > 0 && enc_chunk.size() != ck.cb_comp) {
              fp.reset();
              return fail("chunk size mismatch (" + std::string(hex_sha) +
                          "): got " + std::to_string(enc_chunk.size()) +
                          ", manifest says " + std::to_string(ck.cb_comp));
            }

            std::vector<uint8_t> comp_chunk;
            if (!bcrypt_aes256_decrypt_steam(enc_chunk.data(), enc_chunk.size(),
                                             depot_key, &comp_chunk)) {
              fp.reset();
              return fail("chunk AES-256 decrypt failed");
            }

            std::vector<uint8_t> plain_chunk;
            std::string derr;
            if (!decompress_steam_payload(comp_chunk.data(), comp_chunk.size(),
                                          ck.cb_orig, &plain_chunk, &derr)) {
              fp.reset();
              return fail("chunk decompress failed: " + derr);
            }

            // The manifest's `sha` is the CDN chunk ID, not a hash of the
            // plaintext. Steam's integrity contract is an Adler-32 of the
            // decompressed bytes plus the uncompressed length (DepotChunk).
            if (ck.cb_orig > 0 && plain_chunk.size() != ck.cb_orig) {
              fp.reset();
              return fail("chunk decompressed to " +
                          std::to_string(plain_chunk.size()) +
                          " bytes, manifest says " +
                          std::to_string(ck.cb_orig));
            }
            if (ck.crc != 0 &&
                adler32_steam(plain_chunk.data(), plain_chunk.size()) != ck.crc) {
              fp.reset();
              return fail("chunk Adler-32 mismatch (" + std::string(hex_sha) +
                          "): corrupt data or wrong depot key");
            }

            if (!plain_chunk.empty()) {
              if (ck.offset > f.size || plain_chunk.size() > f.size - ck.offset ||
                  _fseeki64(fp.get(), static_cast<long long>(ck.offset), SEEK_SET) != 0 ||
                  fwrite(plain_chunk.data(), 1, plain_chunk.size(), fp.get()) != plain_chunk.size()) {
                fp.reset();
                return fail("cannot write depot file: " + f.filename);
              }
              done_bytes += plain_chunk.size();
            }
            float pct = 0.15f + 0.82f * ((float)done_bytes / (float)total_bytes);
            if (pct > 0.97f) pct = 0.97f;
            report(pct,
                           "downloading " + title + ": " +
                               format_bytes(done_bytes) + " / " +
                               format_bytes(total_bytes));
          }
          if (fclose(fp.release()) != 0) return fail("cannot finish depot file: " + f.filename);
          if (family_shared && f.has_content_sha) {
            std::ifstream saved(family_file, std::ios::binary);
            if (!saved || !steam_file_matches(saved, f.size, f.content_sha))
              return fail("Downloaded file failed Steam manifest verification: " + f.filename);
          }
        }

        if (family_shared) {
          if (!family_account_active())
            return fail("Steam Families access changed during the download");
          std::filesystem::rename(family_partial, family_final);
          family_partial.clear();
        }
      }
      if (family_shared) {
        if (!family_account_active()) return fail("Steam Families access changed during the download");
        set_item_state(false, 1.0f, "Base depots downloaded — requires a Steamworks play session");
        g_dl_busy = false;
        if (on_complete) on_complete();
        return;
      }

      // 9) Write steam_appid.txt & run Engine Synthesizer + PC->Xbox translation
      {
        FILE* af = fopen((install_dir + "\\steam_appid.txt").c_str(), "wb");
        if (af) {
          fprintf(af, "%u\n", app_id);
          fclose(af);
        }
      }
      set_item_state(true, 0.98f,
                     "synthesizing Xbox engine profile for " + title + "…");
      trt_engine_profile prof;
      const char* hint_exe =
          main_exe_rel.empty() ? nullptr : main_exe_rel.c_str();
      if (trt_synthesize_engine(install_dir.c_str(), hint_exe, &prof)) {
        std::string tdir = install_dir + "\\.tiramisu";
        CreateDirectoryA(tdir.c_str(), nullptr);
        if (!prof.main_exe_abs.empty()) {
          trt_pc2xb_translate_file(prof.main_exe_abs.c_str(),
                                   (tdir + "\\xb").c_str());
        }
      }
      set_item_state(false, 1.0f, "Ready to Play", true, install_dir);
      g_dl_busy = false;
      if (on_complete) on_complete();
    } catch (winrt::hresult_error const& e) {
      fail(u16to8(std::wstring(e.message().c_str())));
    } catch (...) {
      fail("unexpected error during Steam depot download");
    }
  }).detach();
}

static uint64_t family_package_bytes(const std::filesystem::path& root) {
  uint64_t bytes = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
    if (entry.is_regular_file()) bytes += entry.file_size();
  return bytes;
}

// Storage scans and deletions share the download worker's exclusion flag.
struct family_storage_unlock {
  ~family_storage_unlock() { g_dl_busy = false; }
};

bool steam_family_storage_snapshot(uint64_t& account, uint64_t& available,
                                   std::vector<steam_stored_game>& games, std::string& error) {
  std::wstring state;
  std::unordered_map<uint32_t, std::string> titles;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_acct.signed_in || g_state_dir.empty()) { error = "Sign in to view storage"; return false; }
    state = u8to16(g_state_dir);
    account = g_acct.steamid64;
    for (const auto& item : g_family_items) titles.emplace(item.app_id, item.title);
    if (g_dl_busy.exchange(true)) { error = "Steam storage is busy"; return false; }
  }
  family_storage_unlock unlock;
  try {
    ULARGE_INTEGER free{};
    if (!GetDiskFreeSpaceExW(state.c_str(), &free, nullptr, nullptr)) {
      error = "Cannot measure Xbox storage"; return false;
    }
    available = free.QuadPart;
    auto root = std::filesystem::path(state) / "steam-family" / std::to_string(account);
    if (std::filesystem::exists(root)) {
      for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (!entry.is_directory()) continue;
        auto name = entry.path().filename().string();
        uint32_t app = static_cast<uint32_t>(strtoull(name.c_str(), nullptr, 10));
        if (!app || std::to_string(app) != name) continue;
        games.push_back({app, titles.count(app) ? titles.at(app) : "App " + name,
                         family_package_bytes(entry.path())});
      }
    }
    return true;
  } catch (...) { error = "Cannot measure downloaded packages"; return false; }
}

bool steam_family_remove_files(const std::string& account, uint32_t app, std::string& error) {
  std::filesystem::path root;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_acct.signed_in || account != std::to_string(g_acct.steamid64) ||
        !app || g_state_dir.empty()) { error = "Steam account changed"; return false; }
    root = std::filesystem::path(u8to16(g_state_dir)) / "steam-family" / account / std::to_string(app);
    if (g_dl_busy.exchange(true)) { error = "Steam storage is busy"; return false; }
  }
  family_storage_unlock unlock;
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  pending_removal.clear();
  pending_space_plan.clear();
  if (ec) { error = "Could not remove all downloaded packages"; return false; }
  std::lock_guard<std::mutex> lock(g_mu);
  if (account == std::to_string(g_acct.steamid64))
    for (auto& item : g_family_items)
      if (item.app_id == app) item.status_text = "Downloaded packages removed";
  return true;
}

void steam_client_family_storage(uint32_t app_id) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (!g_acct.signed_in || g_state_dir.empty() || !app_id ||
      std::none_of(g_family_items.begin(), g_family_items.end(),
                   [app_id](const auto& item) { return item.app_id == app_id; })) return;
  if (g_dl_busy.exchange(true)) {
    g_acct.status_line = "Steam storage is busy";
    return;
  }
  const auto sid = g_acct.steamid64;
  const auto root = std::filesystem::path(u8to16(g_state_dir)) / "steam-family" /
                    std::to_string(sid) / std::to_string(app_id);
  const bool confirmed = root == pending_removal &&
                         std::chrono::steady_clock::now() < removal_deadline;
  pending_removal.clear();
  g_acct.status_line = confirmed ? "Removing downloaded packages…" : "Measuring packages…";
  std::thread([root, sid, app_id, confirmed]() {
    std::string status;
    try {
      if (!std::filesystem::exists(root)) {
        status = "No downloaded packages for this game";
      } else if (confirmed) {
        std::filesystem::remove_all(root);
        pending_space_plan.clear();
        status = "Downloaded packages removed";
      } else {
        const auto bytes = family_package_bytes(root);
        status = "Stored: " + (bytes ? format_bytes(bytes) : "0 B") +
                 ". Press Storage again within 10 seconds to delete.";
        pending_removal = root;
        removal_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      }
    } catch (...) {
      status = "Storage operation failed. Open Storage to check remaining files.";
    }
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_acct.signed_in && g_acct.steamid64 == sid) {
      g_acct.status_line = status;
      if (confirmed) for (auto& item : g_family_items)
        if (item.app_id == app_id) item.status_text = status;
    }
    g_dl_busy = false;
  }).detach();
}

steam_account_snapshot steam_client_account() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_acct;
}

std::vector<steam_library_item> steam_client_items() {
  std::lock_guard<std::mutex> lk(g_mu);
  auto items = g_items;
  std::unordered_set<uint32_t> seen;
  for (const auto& item : items) seen.insert(item.app_id);
  for (const auto& item : g_family_items) {
    if (seen.insert(item.app_id).second) items.push_back(item);
  }
  return items;
}

} // namespace longan
