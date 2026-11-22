// tiramisu/src/trt/steam_auth.h
// Real Steam account sign-in for Tiramisu, done the only secure way:
// Steam OpenID 2.0 (checkid_setup -> check_authentication).
//
// WHAT THIS DOES NOT DO (on purpose):
//   * It never asks for, receives, stores or transmits a Steam password.
//     The password is typed on steamcommunity.com, in the user's own browser.
//   * It never stores a Steam session, web token, cookie or shared secret.
//     After sign-in the only thing on disk is the 64-bit SteamID, the persona
//     name Steam returned, and a timestamp -- all public information.
//   * It never claims a game license. Ownership/entitlement is Steam's, and
//     the Steam client is the only thing that can attest to it. What sign-in
//     buys is "this console belongs to this Steam account", which is what
//     lets Tiramisu show that account's own installed library.
//
// SECURITY MODEL
//   The browser is sent to Steam with our return_to; Steam redirects back
//   with a signed OpenID assertion. We then POST the assertion back to Steam
//   with openid.mode=check_authentication and accept the identity ONLY when
//   Steam answers "is_valid:true". A forged callback therefore fails, and a
//   replayed one only re-asserts the same public SteamID. The identity file
//   is written 0600 and atomically (temp + rename).
#ifndef TIRAMISU_STEAM_AUTH_H
#define TIRAMISU_STEAM_AUTH_H

#include <map>
#include <string>
#include <vector>

// The signed-in account, as persisted locally.
struct trt_steam_identity {
  std::string steamid64;   // "7656119..." (public)
  std::string persona;     // display name Steam reported (may be empty)
  std::string method;      // "openid2"
  long long signed_in_at = 0;  // unix seconds
  bool signed_in = false;
};

// Steam's OpenID 2.0 endpoint (both the redirect and the validation POST).
extern const char* const TRT_STEAM_OPENID_ENDPOINT;

// Build the browser URL that starts sign-in. `return_to` is our own callback,
// `realm` its origin (scheme://host[:port]).
std::string trt_steam_login_url(const std::string& return_to,
                                const std::string& realm);

// Form encoding helpers (exposed for tests).
std::string trt_steam_url_encode(const std::string& s);
std::string trt_steam_url_decode(const std::string& s);

// Split "a=1&b=2" into a map. Duplicate keys are rejected (0 length value is
// kept) because a duplicated openid.* field is an attack, not a typo.
bool trt_steam_parse_query(const std::string& query,
                           std::map<std::string, std::string>* out,
                           std::string* err);

// Parse the browser's callback query. Requires every OpenID 2.0 field Steam
// sends, op_endpoint == TRT_STEAM_OPENID_ENDPOINT, and an identity/claimed_id
// shaped like Steam's. Returns 0 on success, -1 with *err set otherwise.
int trt_steam_parse_assertion(const std::string& query,
                              std::map<std::string, std::string>* params,
                              std::string* err);

// "https://steamcommunity.com/openid/id/7656119..." -> "7656119...".
// Validates the digits and the SteamID64 type/universe bits.
bool trt_steam_claimed_id_to_steam64(const std::string& claimed_id,
                                     std::string* out);

// The exact POST body for check_authentication (every openid.* field, mode
// swapped to check_authentication, everything else URL-encoded).
std::string trt_steam_validation_body(
    const std::map<std::string, std::string>& params);

// True only when Steam's answer contains "is_valid:true".
bool trt_steam_validation_ok(const std::string& response);

// HTTPS POST application/x-www-form-urlencoded. Windows uses WinHTTP; other
// hosts use curl with the body on stdin (never in the command line).
// The URL must be the Steam endpoint; anything else is refused.
bool trt_steam_http_post(const std::string& url, const std::string& body,
                         std::string* out, std::string* err);

// Test/embed seam: when set, every validation POST goes through this instead
// of the network. Return false to simulate a transport failure.
typedef bool (*trt_steam_http_hook_t)(const std::string& url,
                                      const std::string& body,
                                      std::string* out, std::string* err);
void trt_steam_set_http_hook(trt_steam_http_hook_t hook);

// ---- local identity store -------------------------------------------------

std::string trt_steam_state_path(const std::string& state_dir);
bool trt_steam_auth_load(const std::string& state_dir, trt_steam_identity* out);
bool trt_steam_auth_save(const std::string& state_dir,
                         const trt_steam_identity& id);
bool trt_steam_auth_sign_out(const std::string& state_dir);

// Complete the callback: parse, validate against Steam, persist. Returns 0 on
// success; -1 with *err on any failure (nothing is persisted then).
int trt_steam_auth_complete(const std::string& state_dir,
                            const std::string& query,
                            trt_steam_identity* out, std::string* err);

#endif  // TIRAMISU_STEAM_AUTH_H
