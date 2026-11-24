// tiramisu/src/host/steam_client.cpp
// Native Xbox UWP Steam Client + CM/CDN Depot Downloader.
#include "steam_client.h"
#include "host_log.h"
#include "../trt/bridge_steam.h"
#include "../trt/steam_auth.h"
#include "../trt/steam_library.h"
#include "../trt/engine_synthesizer.h"
#include "../trt/pc2xb_translator.h"
#include "vendor/miniz.h"

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
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace longan {

namespace {

std::mutex g_mu;
std::string g_state_dir;
std::string g_games_dir;
steam_account_snapshot g_acct;
std::string g_refresh_token;
std::string g_access_token;
std::vector<steam_library_item> g_items;
std::atomic<bool> g_auth_busy{false};
std::atomic<bool> g_dl_busy{false};

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

// ---- Base64 + Base64URL ----
int b64_val(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
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
                    std::string* err) {
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
    IBuffer buf = resp.Content().ReadAsBufferAsync().get();
    DataReader dr = DataReader::FromBuffer(buf);
    out->resize(buf.Length());
    if (!out->empty()) dr.ReadBytes(*out);
    return true;
  } catch (winrt::hresult_error const& e) {
    if (err) *err = u16to8(std::wstring(e.message().c_str()));
    return false;
  } catch (...) {
    if (err) *err = "HTTP download failed";
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
    if (!read_varint(&l) || pos + l > len) return false;
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

// ---- BCrypt AES-256 & SHA-1 ----
bool bcrypt_sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
  BCRYPT_ALG_HANDLE hAlg = nullptr;
  BCRYPT_HASH_HANDLE hHash = nullptr;
  if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) < 0)
    return false;
  bool ok = false;
  if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) >= 0) {
    if (BCryptHashData(hHash, (PUCHAR)data, (ULONG)len, 0) >= 0 &&
        BCryptFinishHash(hHash, out, 20, 0) >= 0) {
      ok = true;
    }
    BCryptDestroyHash(hHash);
  }
  BCryptCloseAlgorithmProvider(hAlg, 0);
  return ok;
}

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

// Minimal LZMA1 decoder for Valve VZ depot chunks:
// Header: 'V' 'Z' [version byte 'a'] [4B CRC/timestamp] [5B LZMA props (lc/lp/pb + dict)] [compressed...] [4B CRC32] [4B size LE]
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
  if (len >= 4 && raw[0] == 'V' && raw[1] == 'S' && raw[2] == 'Z' &&
      raw[3] == 'a') {
    if (err) *err = "unsupported Steam chunk codec VSZa (zstd not bundled on Xbox)";
    return false;
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
  if (len >= 22 && raw[0] == 'V' && raw[1] == 'Z') {
    // Valve VZ: 'V','Z',ver(1),timestamp/crc(4),props(5),data...,crc32(4),size(4)
    uint32_t sz = (uint32_t)raw[len - 4] | ((uint32_t)raw[len - 3] << 8) |
                  ((uint32_t)raw[len - 2] << 16) | ((uint32_t)raw[len - 1] << 24);
    if (expected_size > 0 && sz != expected_size) sz = expected_size;
    if (!lzma1_decode_raw(raw + 7, raw + 12, len - 12 - 8, sz, out)) {
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

// ---- WebSocket Steam CM session ----
class cm_session {
 public:
  winrt::Windows::Networking::Sockets::MessageWebSocket ws{nullptr};
  std::mutex rx_mu;
  std::condition_variable rx_cv;
  std::deque<std::vector<uint8_t>> rx_msgs;
  bool closed = false;
  uint32_t session_id = 0;
  uint64_t steamid = 0;
  uint64_t job_seq = 1;

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
      ws.MessageReceived([this](MessageWebSocket const&,
                                MessageWebSocketMessageReceivedEventArgs const& a) {
        try {
          auto dr = a.GetDataReader();
          uint32_t n = dr.UnconsumedBufferLength();
          std::vector<uint8_t> pkt(n);
          if (n > 0) dr.ReadBytes(pkt);
          std::lock_guard<std::mutex> lk(rx_mu);
          rx_msgs.push_back(std::move(pkt));
          rx_cv.notify_all();
        } catch (...) {
        }
      });
      ws.Closed([this](auto const&, auto const&) {
        std::lock_guard<std::mutex> lk(rx_mu);
        closed = true;
        rx_cv.notify_all();
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
                  const pb_writer& body, uint64_t job_id = 0) {
    try {
      pb_writer hdr;
      if (steamid != 0) hdr.write_fixed64(1, steamid);
      if (session_id != 0) hdr.write_uint32(2, session_id);
      if (job_id != 0) hdr.write_fixed64(10, job_id);
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
                 int timeout_ms = 12000) {
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
      if (dispatch_pkt(pkt, want_emsg, out_body)) return true;
    }
  }

  bool dispatch_pkt(const std::vector<uint8_t>& pkt, uint32_t want_emsg,
                    std::vector<uint8_t>* out_body) {
    if (pkt.size() < 8) return false;
    uint32_t raw_emsg = 0, hlen = 0;
    std::memcpy(&raw_emsg, pkt.data(), 4);
    std::memcpy(&hlen, pkt.data() + 4, 4);
    if (8 + (size_t)hlen > pkt.size()) return false;
    uint32_t emsg = raw_emsg & 0x7FFFFFFFu;
    pb_reader hr{pkt.data() + 8, hlen};
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
        if (dispatch_pkt(sub, want_emsg, out_body)) return true;
      }
      return false;
    }
    if (emsg == want_emsg) {
      out_body->assign(b, b + blen);
      return true;
    }
    return false;
  }
};

// Parse Steam VDF text sections for depot_id, manifest_id, launch executable
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
      if (q1 != std::string::npos && q2 != std::string::npos) {
        *out_exe = vdf.substr(q1 + 1, q2 - q1 - 1);
      }
    }
  }
  // Find depots section: scan numeric quoted keys with "manifests" -> "public" -> "gid"
  size_t dpos = vdf.find("\"depots\"");
  if (dpos == std::string::npos) return;
  uint32_t best_depot = 0;
  uint64_t best_manifest = 0;
  uint64_t best_sz = 0;
  size_t pos = dpos + 8;
  while (pos < vdf.size()) {
    size_t q1 = vdf.find('"', pos);
    if (q1 == std::string::npos) break;
    size_t q2 = vdf.find('"', q1 + 1);
    if (q2 == std::string::npos) break;
    std::string key = vdf.substr(q1 + 1, q2 - q1 - 1);
    pos = q2 + 1;
    bool all_digits = !key.empty();
    for (char c : key)
      if (!std::isdigit((unsigned char)c)) all_digits = false;
    if (!all_digits) continue;
    uint32_t did = (uint32_t)strtoul(key.c_str(), nullptr, 10);
    if (did < 10 || did == 228980 || did == 228990) continue; // skip shared redist
    // Bound lookahead before next depot block or brace end
    size_t brace = vdf.find('{', pos);
    if (brace == std::string::npos || brace > pos + 32) continue;
    int depth = 1;
    size_t end = brace + 1;
    while (end < vdf.size() && depth > 0) {
      if (vdf[end] == '{') ++depth;
      else if (vdf[end] == '}') --depth;
      ++end;
    }
    std::string blk = vdf.substr(brace, end - brace);
    // Check oslist if present (accept windows or empty)
    if (blk.find("\"oslist\"") != std::string::npos &&
        blk.find("\"windows\"") == std::string::npos) {
      continue;
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
    if (sp != std::string::npos && sp < gp + 80) {
      size_t sq1 = blk.find('"', sp + 6);
      size_t sq2 = (sq1 != std::string::npos) ? blk.find('"', sq1 + 1) : std::string::npos;
      if (sq1 != std::string::npos && sq2 != std::string::npos) {
        msz = strtoull(blk.substr(sq1 + 1, sq2 - sq1 - 1).c_str(), nullptr, 10);
      }
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
  uint64_t offset = 0;
  uint32_t cb_orig = 0;
  uint32_t cb_comp = 0;
};

struct depot_file_t {
  std::string filename;
  uint64_t size = 0;
  uint32_t flags = 0;
  std::vector<depot_chunk_t> chunks;
};

bool parse_manifest_blob(const std::vector<uint8_t>& blob,
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
  bool filenames_encrypted = false;
  size_t off = 0;
  while (off + 8 <= inner.size()) {
    uint32_t magic = 0, blen = 0;
    std::memcpy(&magic, inner.data() + off, 4);
    std::memcpy(&blen, inner.data() + off + 4, 4);
    off += 8;
    if (magic == 0x32C415ABu) break; // EOF
    if (off + blen > inner.size()) break;
    if (magic == 0x71F617D0u) {
      payload = inner.data() + off;
      payload_len = blen;
    } else if (magic == 0x1F4812BEu) {
      pb_reader mr{inner.data() + off, blen};
      uint32_t f, wt;
      while (mr.next(&f, &wt)) {
        if (f == 4 && wt == 0) {
          uint64_t v = 0;
          mr.read_varint(&v);
          filenames_encrypted = (v != 0);
        } else {
          mr.skip(wt);
        }
      }
    }
    off += blen;
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
      if (!pr.read_bytes(&fp, &fl)) break;
      pb_reader fr{fp, fl};
      depot_file_t df;
      uint32_t ff, fwt;
      while (fr.next(&ff, &fwt)) {
        if (ff == 1 && fwt == 2) {
          const uint8_t* sp = nullptr;
          size_t sl = 0;
          fr.read_bytes(&sp, &sl);
          df.filename.assign((const char*)sp, sl);
        } else if (ff == 2 && fwt == 0) {
          fr.read_varint(&df.size);
        } else if (ff == 3 && fwt == 0) {
          uint64_t v = 0;
          fr.read_varint(&v);
          df.flags = (uint32_t)v;
        } else if (ff == 6 && fwt == 2) {
          const uint8_t* cp = nullptr;
          size_t cl = 0;
          fr.read_bytes(&cp, &cl);
          pb_reader cr{cp, cl};
          depot_chunk_t ck{};
          uint32_t cf, cwt;
          while (cr.next(&cf, &cwt)) {
            if (cf == 1 && cwt == 2) {
              const uint8_t* shp = nullptr;
              size_t shl = 0;
              cr.read_bytes(&shp, &shl);
              if (shl == 20) std::memcpy(ck.sha, shp, 20);
            } else if (cf == 3 && cwt == 0) {
              cr.read_varint(&ck.offset);
            } else if (cf == 4 && cwt == 0) {
              uint64_t v = 0;
              cr.read_varint(&v);
              ck.cb_orig = (uint32_t)v;
            } else if (cf == 5 && cwt == 0) {
              uint64_t v = 0;
              cr.read_varint(&v);
              ck.cb_comp = (uint32_t)v;
            } else {
              cr.skip(cwt);
            }
          }
          df.chunks.push_back(ck);
        } else {
          fr.skip(fwt);
        }
      }
      if (filenames_encrypted && !df.filename.empty()) {
        auto enc = b64_decode(df.filename);
        std::vector<uint8_t> dec;
        if (bcrypt_aes256_decrypt_steam(enc.data(), enc.size(), depot_key, &dec)) {
          while (!dec.empty() && dec.back() == 0) dec.pop_back();
          df.filename.assign((const char*)dec.data(), dec.size());
        }
      }
      // Normalize path separators to backslash and strip trailing whitespace
      for (char& c : df.filename)
        if (c == '/') c = '\\';
      files->push_back(std::move(df));
    } else {
      pr.skip(wt);
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

  // Refresh access token if needed
  if (access_tok.empty() && !refresh_tok.empty() && sid != 0) {
    std::string body = "{\"refresh_token\":\"" + refresh_tok +
                       "\",\"steamid\":\"" + std::to_string(sid) +
                       "\",\"renewal_type\":0}";
    std::string resp, err;
    if (http_post_form_steam(
            "https://api.steampowered.com/IAuthenticationService/GenerateAccessTokenForApp/v1/",
            body, &resp, &err)) {
      try {
        auto j = winrt::Windows::Data::Json::JsonObject::Parse(
            winrt::hstring(u8to16(resp)));
        auto r = j.GetNamedObject(L"response");
        if (r.HasKey(L"access_token")) {
          access_tok = u16to8(std::wstring(r.GetNamedString(L"access_token").c_str()));
          std::lock_guard<std::mutex> lk(g_mu);
          g_access_token = access_tok;
          save_credentials(state_dir, sid, acct_name, g_acct.persona, refresh_tok,
                           access_tok);
        }
      } catch (...) {
      }
    }
  }

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
    if (!persona.empty()) g_acct.persona = persona;
    g_acct.signed_in = true;
    trt_steam_set_identity(std::to_string(sid).c_str(), g_acct.persona.c_str());
    save_credentials(state_dir, sid, acct_name, g_acct.persona, refresh_tok,
                     access_tok);
  }

  // 2) Owned Games (requires access_token)
  std::vector<steam_library_item> fetched_items;
  if (!access_tok.empty()) {
    std::string resp, err;
    std::string url =
        "https://api.steampowered.com/IPlayerService/GetOwnedGames/v1/?"
        "access_token=" +
        access_tok + "&steamid=" + std::to_string(sid) +
        "&include_appinfo=1&include_played_free_games=1";
    if (http_get_text(url, &resp, &err)) {
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
        }
      } catch (...) {
      }
    }
  }

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
    scan_installed_into_items(games_dir, &fetched_items);
    if (!fetched_items.empty()) g_items = std::move(fetched_items);
    else scan_installed_into_items(games_dir, &g_items);
    g_acct.status_line = "steam: signed in as " +
                         (g_acct.persona.empty() ? g_acct.account_name
                                                 : g_acct.persona) +
                         " (" + std::to_string(g_items.size()) + " titles)";
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
      std::string resp, err;
      std::string begin_json =
          "{\"client_id\":\"2654200164598180\",\"device_id\":\"tiramisu-xbox\"}";
      if (!http_post_form_steam(
              "https://api.steampowered.com/IAuthenticationService/BeginAuthSessionViaQR/v1/",
              begin_json, &resp, &err)) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_acct.status_line = "steam QR request failed: " + err;
        g_acct.busy = false;
        g_auth_busy = false;
        return;
      }
      auto j = winrt::Windows::Data::Json::JsonObject::Parse(
          winrt::hstring(u8to16(resp)));
      auto r2 = j.GetNamedObject(L"response");
      std::string challenge =
          u16to8(std::wstring(r2.GetNamedString(L"challenge_url").c_str()));
      std::string client_id =
          u16to8(std::wstring(r2.GetNamedString(L"client_id").c_str()));
      std::string request_id =
          u16to8(std::wstring(r2.GetNamedString(L"request_id").c_str()));
      {
        std::lock_guard<std::mutex> lk(g_mu);
        g_acct.qr_url = challenge;
        g_acct.status_line = "steam: scan QR with the Steam mobile app";
      }
      for (int i = 0; i < 36; ++i) {
        Sleep(4000);
        std::string presp, perr;
        std::string pjson = "{\"client_id\":\"" + client_id +
                            "\",\"request_id\":\"" + request_id + "\"}";
        if (!http_post_form_steam(
                "https://api.steampowered.com/IAuthenticationService/PollAuthSessionStatus/v1/",
                pjson, &presp, &perr)) {
          continue;
        }
        auto pj = winrt::Windows::Data::Json::JsonObject::Parse(
            winrt::hstring(u8to16(presp)));
        auto pr2 = pj.GetNamedObject(L"response");
        if (pr2.HasKey(L"refresh_token")) {
          std::string ref = u16to8(
              std::wstring(pr2.GetNamedString(L"refresh_token").c_str()));
          std::string acc = pr2.HasKey(L"access_token")
                                ? u16to8(std::wstring(
                                      pr2.GetNamedString(L"access_token").c_str()))
                                : "";
          std::string aname = pr2.HasKey(L"account_name")
                                  ? u16to8(std::wstring(
                                        pr2.GetNamedString(L"account_name").c_str()))
                                  : "";
          uint64_t sid = extract_steamid_from_jwt(acc.empty() ? ref : acc);
          if (sid == 0) sid = extract_steamid_from_jwt(ref);
          {
            std::lock_guard<std::mutex> lk(g_mu);
            g_refresh_token = ref;
            g_access_token = acc;
            g_acct.steamid64 = sid;
            g_acct.account_name = aname;
            if (g_acct.persona.empty()) g_acct.persona = aname;
            g_acct.signed_in = (sid != 0);
            g_acct.qr_url.clear();
            g_acct.status_line = "steam: signed in — loading library & friends…";
            save_credentials(g_state_dir, sid, aname, g_acct.persona, ref, acc);
          }
          fetch_remote_account_data(cb);
          std::lock_guard<std::mutex> lk(g_mu);
          g_acct.busy = false;
          g_auth_busy = false;
          return;
        }
        if (pr2.HasKey(L"new_challenge_url")) {
          std::lock_guard<std::mutex> lk(g_mu);
          g_acct.qr_url = u16to8(
              std::wstring(pr2.GetNamedString(L"new_challenge_url").c_str()));
        }
      }
      std::lock_guard<std::mutex> lk(g_mu);
      g_acct.qr_url.clear();
      g_acct.status_line = "steam: QR expired — press Y to retry";
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

void steam_client_download_app(uint32_t app_id,
                               std::function<void()> on_complete) {
  auto set_item_state = [app_id](bool downloading, float pct,
                                 const std::string& st, bool installed = false,
                                 const std::string& idir = "") {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& it : g_items) {
      if (it.app_id == app_id) {
        it.downloading = downloading;
        it.download_pct = pct;
        it.status_text = st;
        if (installed) {
          it.installed = true;
          if (!idir.empty()) it.install_dir = idir;
        }
        break;
      }
    }
    g_acct.status_line = st;
  };

  if (g_dl_busy.exchange(true)) {
    set_item_state(false, 0.0f, "steam: another download is already active");
    return;
  }

  std::string ref_tok, acc_tok, acct_name, games_dir, title;
  uint64_t sid = 0;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    ref_tok = g_refresh_token;
    acc_tok = g_access_token;
    acct_name = g_acct.account_name;
    sid = g_acct.steamid64;
    games_dir = g_games_dir;
    for (const auto& it : g_items) {
      if (it.app_id == app_id) title = it.title;
    }
  }
  if (ref_tok.empty() || sid == 0) {
    g_dl_busy = false;
    set_item_state(false, 0.0f,
                   "download blocked: sign in with Steam (Y) first");
    return;
  }
  if (title.empty()) title = "SteamApp_" + std::to_string(app_id);

  set_item_state(true, 0.02f, "steam: connecting to CM servers…");
  std::thread([=]() {
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
      cm_session cm;
      bool connected = false;
      std::string cm_err;
      for (const auto& h : cm_hosts) {
        if (cm.connect(h, &cm_err)) {
          connected = true;
          break;
        }
      }
      if (!connected) return fail("CM connect failed: " + cm_err);

      // 3) ClientHello + ClientLogon
      {
        pb_writer hello;
        hello.write_uint32(1, 65580); // protocol_version
        cm.send_proto(9805 /* ClientHello */, {}, hello);

        pb_writer logon;
        logon.write_uint32(1, 65580);              // protocol_version
        logon.write_uint32(5, 16);                 // client_os_type = Win10/11
        logon.write_string(41, "tiramisu-xbox");   // machine_name
        logon.write_string(50, acct_name);         // account_name
        logon.write_string(108, ref_tok);          // access_token = refresh token
        pb_writer hdr;
        cm.steamid = sid;
        cm.send_proto(5514 /* ClientLogon */, hdr, logon);

        std::vector<uint8_t> resp;
        if (!cm.recv_emsg(751 /* ClientLogOnResponse */, &resp)) {
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
        req.write_bytes(2, app_msg.buf.data(), app_msg.buf.size());
        cm.send_proto(8903 /* ClientPICSProductInfoRequest */, {}, req);

        std::vector<uint8_t> resp;
        if (!cm.recv_emsg(8904 /* ClientPICSProductInfoResponse */, &resp)) {
          return fail("appinfo query timeout");
        }
        pb_reader rr{resp.data(), resp.size()};
        uint32_t f, wt;
        while (rr.next(&f, &wt)) {
          if (f == 1 && wt == 2) {
            const uint8_t* ap = nullptr;
            size_t al = 0;
            rr.read_bytes(&ap, &al);
            pb_reader ar{ap, al};
            uint32_t af, awt;
            while (ar.next(&af, &awt)) {
              if (af == 6 && awt == 2) {
                const uint8_t* bv = nullptr;
                size_t bl = 0;
                ar.read_bytes(&bv, &bl);
                appinfo_vdf.assign((const char*)bv, bl);
              } else {
                ar.skip(awt);
              }
            }
          } else {
            rr.skip(wt);
          }
        }
      }

      uint32_t depot_id = 0;
      uint64_t manifest_id = 0;
      uint64_t depot_sz = 0;
      std::string main_exe_rel;
      parse_appinfo_vdf(appinfo_vdf, &depot_id, &manifest_id, &depot_sz,
                        &main_exe_rel);
      if (depot_id == 0 || manifest_id == 0) {
        return fail("no Windows depot found in appinfo for " +
                    std::to_string(app_id));
      }

      // 5) Depot decryption key (proves ownership!)
      set_item_state(true, 0.10f, "steam: requesting depot license key…");
      uint8_t depot_key[32] = {};
      {
        pb_writer kreq;
        kreq.write_uint32(1, depot_id);
        kreq.write_uint32(2, app_id);
        cm.send_proto(5438 /* ClientGetDepotDecryptionKey */, {}, kreq);

        std::vector<uint8_t> resp;
        if (!cm.recv_emsg(5439 /* ClientGetDepotDecryptionKeyResponse */,
                          &resp)) {
          return fail("depot decryption key timeout");
        }
        uint32_t eresult = 0;
        bool got_key = false;
        pb_reader kr{resp.data(), resp.size()};
        uint32_t f, wt;
        while (kr.next(&f, &wt)) {
          if (f == 1 && wt == 0) {
            uint64_t v = 0;
            kr.read_varint(&v);
            eresult = (uint32_t)v;
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
        if (eresult != 1 || !got_key) {
          return fail("Steam denied depot key (eresult=" +
                      std::to_string(eresult) + " — verify license)");
        }
      }

      // 6) CDN hosts + Manifest Request Code
      std::vector<std::string> cdn_hosts;
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
                cdn_hosts.push_back(
                    u16to8(std::wstring(s.GetNamedString(L"host").c_str())));
              }
            }
          } catch (...) {
          }
        }
      }
      if (cdn_hosts.empty()) cdn_hosts.push_back("cache1-iad1.steamcontent.com");

      uint64_t manifest_code = 0;
      {
        std::string resp, err;
        std::string url =
            "https://api.steampowered.com/IContentServerDirectoryService/GetManifestRequestCode/v1/?"
            "app_id=" +
            std::to_string(app_id) + "&depot_id=" + std::to_string(depot_id) +
            "&manifest_id=" + std::to_string(manifest_id) +
            "&app_branch=public";
        if (!acc_tok.empty()) url += "&access_token=" + acc_tok;
        if (http_get_text(url, &resp, &err)) {
          try {
            auto j = winrt::Windows::Data::Json::JsonObject::Parse(
                winrt::hstring(u8to16(resp)));
            auto r = j.GetNamedObject(L"response");
            if (r.HasKey(L"manifest_request_code")) {
              manifest_code = strtoull(
                  u16to8(std::wstring(
                             r.GetNamedString(L"manifest_request_code").c_str()))
                      .c_str(),
                  nullptr, 10);
            }
          } catch (...) {
          }
        }
      }

      // 7) Download + parse depot manifest
      set_item_state(true, 0.14f, "steam: downloading depot manifest…");
      std::vector<uint8_t> manifest_blob;
      std::string dl_err;
      for (const auto& host : cdn_hosts) {
        std::string url = "https://" + host + "/depot/" +
                          std::to_string(depot_id) + "/manifest/" +
                          std::to_string(manifest_id) + "/5";
        if (manifest_code != 0) url += "/" + std::to_string(manifest_code);
        if (http_get_bytes(url, &manifest_blob, &dl_err)) break;
      }
      if (manifest_blob.empty()) return fail("manifest CDN failed: " + dl_err);

      std::vector<depot_file_t> files;
      std::string perr;
      if (!parse_manifest_blob(manifest_blob, depot_key, &files, &perr)) {
        return fail("manifest parse failed: " + perr);
      }

      uint64_t total_bytes = 0;
      for (const auto& f : files) total_bytes += f.size;
      if (total_bytes == 0) total_bytes = 1;

      std::string install_dir = games_dir + "\\" + sanitize_dirname(title);
      ensure_dir_recursive(install_dir);

      // 8) Stream every chunk, AES-decrypt, decompress, SHA-1 verify, write
      uint64_t done_bytes = 0;
      for (const auto& f : files) {
        if (f.flags & 0x40 /* directory flag */) {
          ensure_dir_recursive(install_dir + "\\" + f.filename);
          continue;
        }
        std::string fpath = install_dir + "\\" + f.filename;
        size_t sl = fpath.rfind('\\');
        if (sl != std::string::npos) ensure_dir_recursive(fpath.substr(0, sl));

        FILE* fp = fopen(fpath.c_str(), "wb");
        if (!fp) return fail("cannot create file: " + f.filename);

        for (const auto& ck : f.chunks) {
          char hex_sha[41];
          for (int i = 0; i < 20; ++i)
            snprintf(hex_sha + i * 2, 3, "%02x", ck.sha[i]);

          std::vector<uint8_t> enc_chunk;
          std::string cerr;
          bool got = false;
          for (const auto& host : cdn_hosts) {
            std::string url = "https://" + host + "/depot/" +
                              std::to_string(depot_id) + "/chunk/" + hex_sha;
            if (http_get_bytes(url, &enc_chunk, &cerr)) {
              got = true;
              break;
            }
          }
          if (!got) {
            fclose(fp);
            return fail("chunk CDN failed (" + std::string(hex_sha) + "): " +
                        cerr);
          }

          std::vector<uint8_t> comp_chunk;
          if (!bcrypt_aes256_decrypt_steam(enc_chunk.data(), enc_chunk.size(),
                                           depot_key, &comp_chunk)) {
            fclose(fp);
            return fail("chunk AES-256 decrypt failed");
          }

          std::vector<uint8_t> plain_chunk;
          std::string derr;
          if (!decompress_steam_payload(comp_chunk.data(), comp_chunk.size(),
                                        ck.cb_orig, &plain_chunk, &derr)) {
            fclose(fp);
            return fail("chunk decompress failed: " + derr);
          }

          uint8_t digest[20] = {};
          if (!bcrypt_sha1(plain_chunk.data(), plain_chunk.size(), digest) ||
              std::memcmp(digest, ck.sha, 20) != 0) {
            fclose(fp);
            return fail("chunk SHA-1 verification mismatch");
          }

          if (!plain_chunk.empty()) {
            _fseeki64(fp, (long long)ck.offset, SEEK_SET);
            fwrite(plain_chunk.data(), 1, plain_chunk.size(), fp);
            done_bytes += plain_chunk.size();
          }
          float pct = 0.15f + 0.82f * ((float)done_bytes / (float)total_bytes);
          if (pct > 0.97f) pct = 0.97f;
          set_item_state(true, pct,
                         "downloading " + title + ": " +
                             format_bytes(done_bytes) + " / " +
                             format_bytes(total_bytes));
        }
        fclose(fp);
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

steam_account_snapshot steam_client_account() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_acct;
}

std::vector<steam_library_item> steam_client_items() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_items;
}

} // namespace longan
