#pragma once
#include <chrono>
#include <algorithm>
#include <stdexcept>
#include <winrt/Windows.Security.Cryptography.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>

namespace longan::epic_library {
using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Web::Http;
using winrt::Windows::Foundation::Uri;
using clock = std::chrono::steady_clock;

inline JsonObject request(const std::wstring& url, const winrt::hstring& auth,
                          const std::wstring& form = {}) {
  HttpClient client;
  client.DefaultRequestHeaders().Authorization(Headers::HttpCredentialsHeaderValue::Parse(auth));
  HttpResponseMessage response{nullptr};
  if (form.empty()) response = client.GetAsync(Uri(url)).get();
  else response = client.PostAsync(Uri(url), HttpStringContent(form,
      winrt::Windows::Storage::Streams::UnicodeEncoding::Utf8,
      L"application/x-www-form-urlencoded")).get();
  if (!response.IsSuccessStatusCode()) {
    auto code = static_cast<int>(response.StatusCode());
    if (code == 400 && form.find(L"grant_type=device_code") == 0) {
      auto error = JsonObject::Parse(response.Content().ReadAsStringAsync().get()).GetNamedString(L"errorCode", L"");
      if (error == L"errors.com.epicgames.account.oauth.authorization_pending")
        throw std::runtime_error("Approve on Epic first, then check again.");
    }
    throw std::runtime_error("Epic denied the request (HTTP " + std::to_string(code) + ").");
  }
  return JsonObject::Parse(response.Content().ReadAsStringAsync().get());
}

// A single pending console sign-in. Only its originating panel gets the handle.
inline std::string run(const std::string& state, const std::string& handle) {
  static std::mutex mutex;
  static std::string pending;
  static winrt::hstring device, access;
  static clock::time_point expires, next_check;
  static int interval = 5;
  std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
  if (!lock) throw std::runtime_error("Epic request in progress.");
  std::ifstream config(std::filesystem::u8path(state) / "epic-client.json", std::ios::binary);
  if (!config) throw std::runtime_error("Epic library requires a configured, authorized client (LocalState/epic-client.json).");
  std::string raw((std::istreambuf_iterator<char>(config)), {});
  auto basic = L"Basic " + JsonObject::Parse(winrt::to_hstring(raw)).GetNamedString(L"basic");
  const std::wstring base = L"https://account-public-service-prod.ol.epicgames.com/account/api/oauth/";
  JsonObject result;
  if (handle.empty()) {
    auto app = request(base + L"token", basic, L"grant_type=client_credentials");
    auto grant = request(base + L"deviceAuthorization", L"Bearer " + app.GetNamedString(L"access_token"), L"prompt=login");
    auto code = grant.GetNamedString(L"user_code");
    auto new_device = grant.GetNamedString(L"device_code");
    auto lifetime = std::clamp(grant.GetNamedNumber(L"expires_in", 300), 1.0, 900.0);
    interval = static_cast<int>(std::clamp(grant.GetNamedNumber(L"interval", 5), 1.0, 60.0));
    using winrt::Windows::Security::Cryptography::CryptographicBuffer;
    pending = winrt::to_string(CryptographicBuffer::EncodeToHexString(CryptographicBuffer::GenerateRandom(24)));
    device = new_device;
    access.clear();
    expires = clock::now() + std::chrono::seconds(static_cast<int>(lifetime));
    next_check = clock::now() + std::chrono::seconds(interval);
    result.SetNamedValue(L"handle", JsonValue::CreateStringValue(winrt::to_hstring(pending)));
    result.SetNamedValue(L"code", JsonValue::CreateStringValue(code));
  } else {
    if (handle != pending || clock::now() >= expires)
      throw std::runtime_error("Start a new Epic sign-in.");
    if (access.empty()) {
      if (clock::now() < next_check) throw std::runtime_error("Please wait before checking again.");
      next_check = clock::now() + std::chrono::seconds(interval);
      auto session = request(base + L"token", basic, std::wstring(L"grant_type=device_code&device_code=") +
          Uri::EscapeComponent(device).c_str());
      access = session.GetNamedString(L"access_token");
      device.clear();
    }
    JsonArray titles;
    winrt::hstring cursor;
    std::set<std::wstring> cursors;
    do {
      std::wstring url = L"https://library-service.live.use1a.on.epicgames.com/library/api/public/items?includeMetadata=true";
      if (!cursor.empty()) url += std::wstring(L"&cursor=") + Uri::EscapeComponent(cursor).c_str();
      auto page = request(url, L"Bearer " + access);
      for (auto record : page.GetNamedArray(L"records")) {
        auto item = record.GetObject();
        auto app = item.GetNamedString(L"appName", L"");
        if (app.empty()) continue;
        auto metadata = item.GetNamedObject(L"metadata", nullptr);
        auto title = metadata ? metadata.GetNamedString(L"title", app) : app;
        titles.Append(JsonValue::CreateStringValue(title));
      }
      auto meta = page.GetNamedObject(L"responseMetadata");
      cursor = meta.GetNamedString(L"nextCursor", L"");
      if (!cursor.empty() && !cursors.emplace(cursor.c_str()).second)
        throw std::runtime_error("Epic returned a repeated library page.");
    } while (!cursor.empty());
    result.SetNamedValue(L"titles", titles);
    pending.clear(); access.clear();
  }
  return winrt::to_string(result.Stringify());
}
}
