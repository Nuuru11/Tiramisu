// tiramisu/src/trt/bridge_wininet.cpp
// WinINet bridge. See bridge_wininet.h.
//
// Handle records are real: InternetOpenW/A allocates one, InternetCloseHandle
// frees it. Every request verb fails with ERROR_INTERNET_CANNOT_CONNECT
// (12029) and names itself in the log — WinINet is documented not available
// to UWP and the console has no network stack for the guest, so the engine's
// "if (send failed) use offline mode" path runs exactly as designed.
#include "bridge_wininet.h"
#include "thunk.h"
#include "trt.h"

#include <stdio.h>
#include <string.h>

namespace {
constexpr u32 k_err_cannot_connect = 12029;  // ERROR_INTERNET_CANNOT_CONNECT
constexpr u32 k_err_invalid_handle = 6;      // ERROR_INVALID_HANDLE
constexpr u32 k_err_invalid_parameter = 87;  // ERROR_INVALID_PARAMETER

// ---- handle records (tag 0x57 = 'W') ----
struct inet_rec { u32 tag; u32 kind; };  // kind: 0=internet 1=connect 2=req
inet_rec g_inet[32];
u32 g_inet_lasterr = 0;

void inet_seterr(u32 e) { g_inet_lasterr = e; }

void* inet_alloc(u32 kind) {
  for (u32 i = 0; i < 32; ++i) {
    if (g_inet[i].tag == 0) {
      g_inet[i].tag = 0x57u;
      g_inet[i].kind = kind;
      return (void*)(uintptr_t)(0x57000000u | i);
    }
  }
  return nullptr;
}

bool inet_valid(void* h) {
  const u32 v = (u32)(uintptr_t)h;
  return (v >> 24) == 0x57u && (v & 0xFFFFFFu) < 32 && g_inet[v & 0xFFFFFFu].tag;
}

void inet_free(void* h) {
  const u32 v = (u32)(uintptr_t)h;
  if ((v >> 24) == 0x57u && (v & 0xFFFFFFu) < 32) g_inet[v & 0xFFFFFFu].tag = 0;
}

// unique name, file scope, OUTSIDE extern "C" (the log collision rule)
namespace wininetlog {
static void line(const char* s) { trt_core_log(s); }
}
#define log_line(s_) wininetlog::line(s_)

// utf16 helper (log + agent strings)
void w2a(const u16* in, char* out, u32 cap) {
  u32 o = 0;
  if (in)
    for (; in[o] && o + 1 < cap; ++o) out[o] = (char)in[o];
  out[o] = 0;
}

}  // namespace

extern "C" {

// ---- agent creation: real handle record ----
void* TRT_GUEST_ABI wininet_InternetOpenW(const u16* agent, u32 access,
                                          const u16* proxy, const u16* bypass,
                                          u32 flags) {
  char agent8[64];
  w2a(agent, agent8, sizeof(agent8));
  void* h = inet_alloc(0);
  char line[160];
  snprintf(line, sizeof(line),
           "wininet: InternetOpenW agent=%s -> %s (handle record; no network "
           "for the guest on console)",
           agent8, h ? "handle" : "NULL");
  log_line(line);
  if (!h) { inet_seterr(k_err_invalid_parameter); return nullptr; }
  return h;
}

void* TRT_GUEST_ABI wininet_InternetOpenA(const char* agent, u32 access,
                                          const char* proxy,
                                          const char* bypass, u32 flags) {
  void* h = inet_alloc(0);
  char line[160];
  snprintf(line, sizeof(line),
           "wininet: InternetOpenA agent=%s -> %s (handle record; no network "
           "for the guest on console)",
           agent ? agent : "?", h ? "handle" : "NULL");
  log_line(line);
  if (!h) { inet_seterr(k_err_invalid_parameter); return nullptr; }
  return h;
}

// ---- connect/request verbs: honest failure, handle records kept real ----
void* TRT_GUEST_ABI wininet_InternetConnectW(void* h, const u16* server,
                                             u16 port, const u16* user,
                                             const u16* pass, u32 service,
                                             u32 flags, uintptr_t ctx) {
  (void)port; (void)user; (void)pass; (void)service; (void)flags; (void)ctx;
  char server8[128];
  w2a(server, server8, sizeof(server8));
  char line[192];
  if (!inet_valid(h)) {
    snprintf(line, sizeof(line),
             "wininet: InternetConnectW %s -> NULL (invalid HINTERNET)",
             server8);
    log_line(line);
    inet_seterr(k_err_invalid_handle);
    return nullptr;
  }
  // real semantics would need the network; honest fail: record + FALSE
  void* c = inet_alloc(1);
  snprintf(line, sizeof(line),
           "wininet: InternetConnectW %s -> %s (record; calls fail at "
           "HttpSendRequest: no network stack for the guest)",
           server8, c ? "connect record" : "NULL");
  log_line(line);
  if (!c) { inet_seterr(k_err_invalid_parameter); return nullptr; }
  return c;
}

void* TRT_GUEST_ABI wininet_InternetConnectA(void* h, const char* server,
                                             u16 port, const char* user,
                                             const char* pass, u32 service,
                                             u32 flags, uintptr_t ctx) {
  (void)port; (void)user; (void)pass; (void)service; (void)flags; (void)ctx;
  char line[192];
  if (!inet_valid(h)) {
    snprintf(line, sizeof(line),
             "wininet: InternetConnectA %s -> NULL (invalid HINTERNET)",
             server ? server : "?");
    log_line(line);
    inet_seterr(k_err_invalid_handle);
    return nullptr;
  }
  void* c = inet_alloc(1);
  snprintf(line, sizeof(line),
           "wininet: InternetConnectA %s -> %s (record; calls fail at "
           "HttpSendRequest: no network stack for the guest)",
           server ? server : "?", c ? "connect record" : "NULL");
  log_line(line);
  if (!c) { inet_seterr(k_err_invalid_parameter); return nullptr; }
  return c;
}

void* TRT_GUEST_ABI wininet_HttpOpenRequestW(void* conn, const u16* verb,
                                             const u16* object,
                                             const u16* version,
                                             const u16* referrer,
                                             const u16** types, u32 flags,
                                             uintptr_t ctx) {
  (void)verb; (void)version; (void)referrer; (void)types; (void)flags;
  (void)ctx;
  char obj8[128];
  w2a(object, obj8, sizeof(obj8));
  char line[192];
  void* r = conn ? inet_alloc(2) : nullptr;
  snprintf(line, sizeof(line),
           "wininet: HttpOpenRequestW %s -> %s (record; will fail honestly at "
           "HttpSendRequestW)", obj8, r ? "request record" : "NULL");
  log_line(line);
  if (!r) { inet_seterr(k_err_invalid_handle); return nullptr; }
  return r;
}

void* TRT_GUEST_ABI wininet_HttpOpenRequestA(void* conn, const char* verb,
                                             const char* object,
                                             const char* version,
                                             const char* referrer,
                                             const char** types, u32 flags,
                                             uintptr_t ctx) {
  (void)verb; (void)version; (void)referrer; (void)types; (void)flags;
  (void)ctx;
  char line[192];
  void* r = conn ? inet_alloc(2) : nullptr;
  snprintf(line, sizeof(line),
           "wininet: HttpOpenRequestA %s -> %s (record; will fail honestly at "
           "HttpSendRequestA)", object ? object : "?",
           r ? "request record" : "NULL");
  log_line(line);
  if (!r) { inet_seterr(k_err_invalid_handle); return nullptr; }
  return r;
}

int TRT_GUEST_ABI wininet_HttpSendRequestW(void* req, const u16* headers,
                                           u32 hdr_len, void* opt,
                                           u32 opt_len) {
  (void)hdr_len; (void)opt; (void)opt_len;
  char line[128];
  snprintf(line, sizeof(line),
           "wininet: HttpSendRequestW -> FALSE (ERROR_INTERNET_CANNOT_CONNECT"
           ": no network stack for the guest on console)");
  log_line(line);
  inet_seterr(k_err_cannot_connect);
  return 0;
}

int TRT_GUEST_ABI wininet_HttpSendRequestA(void* req, const char* headers,
                                           u32 hdr_len, void* opt,
                                           u32 opt_len) {
  (void)headers; (void)hdr_len; (void)opt; (void)opt_len;
  char line[128];
  snprintf(line, sizeof(line),
           "wininet: HttpSendRequestA -> FALSE (ERROR_INTERNET_CANNOT_CONNECT"
           ": no network stack for the guest on console)");
  log_line(line);
  inet_seterr(k_err_cannot_connect);
  return 0;
}

int TRT_GUEST_ABI wininet_InternetReadFile(void* file, void* buf,
                                           u32 bytes, u32* read) {
  char line[128];
  if (!inet_valid(file)) {
    snprintf(line, sizeof(line),
             "wininet: InternetReadFile -> FALSE (invalid HINTERNET)");
    log_line(line);
    inet_seterr(k_err_invalid_handle);
    return 0;
  }
  if (read) *read = 0;  // clean EOF, not a fake payload
  snprintf(line, sizeof(line),
           "wininet: InternetReadFile -> TRUE, 0 bytes (EOF: no network)");
  log_line(line);
  return 1;
}

int TRT_GUEST_ABI wininet_InternetCloseHandle(void* h) {
  char line[96];
  if (!inet_valid(h)) {
    snprintf(line, sizeof(line),
             "wininet: InternetCloseHandle -> FALSE (invalid HINTERNET)");
    log_line(line);
    inet_seterr(k_err_invalid_handle);
    return 0;
  }
  inet_free(h);
  snprintf(line, sizeof(line), "wininet: InternetCloseHandle -> freed record");
  log_line(line);
  return 1;
}

// ---- query verbs: honest failure (no data behind the records) ----
int TRT_GUEST_ABI wininet_HttpQueryInfoA(void* req, u32 info, void* buf,
                                         u32* len, u32* idx) {
  (void)req; (void)buf; (void)len; (void)idx;
  char line[128];
  snprintf(line, sizeof(line),
           "wininet: HttpQueryInfoA -> FALSE (no request data: no network)");
  log_line(line);
  inet_seterr(k_err_cannot_connect);
  return 0;
}

int TRT_GUEST_ABI wininet_InternetCrackUrlA(const char* url, u32 len,
                                            u32 flags, void* comp) {
  (void)len; (void)flags; (void)comp;
  char line[224];
  snprintf(line, sizeof(line),
           "wininet: InternetCrackUrlA -> FALSE (named: no URL parsing on "
           "console; url=%.60s)", url ? url : "?");
  log_line(line);
  inet_seterr(k_err_invalid_parameter);
  return 0;
}

u32 TRT_GUEST_ABI wininet_InternetSetStatusCallback(void* h, void* cb) {
  (void)h; (void)cb;
  char line[128];
  snprintf(line, sizeof(line),
           "wininet: InternetSetStatusCallback -> NULL (no async callbacks: "
           "no network)");
  log_line(line);
  return 0;
}

int trt_bridge_register_wininet(void) {
  static const trt_named_fn k_fns[] = {
      {"InternetOpenW", (void*)&wininet_InternetOpenW},
      {"InternetOpenA", (void*)&wininet_InternetOpenA},
      {"InternetConnectW", (void*)&wininet_InternetConnectW},
      {"InternetConnectA", (void*)&wininet_InternetConnectA},
      {"HttpOpenRequestW", (void*)&wininet_HttpOpenRequestW},
      {"HttpOpenRequestA", (void*)&wininet_HttpOpenRequestA},
      {"HttpSendRequestW", (void*)&wininet_HttpSendRequestW},
      {"HttpSendRequestA", (void*)&wininet_HttpSendRequestA},
      {"InternetReadFile", (void*)&wininet_InternetReadFile},
      {"InternetCloseHandle", (void*)&wininet_InternetCloseHandle},
      {"HttpQueryInfoA", (void*)&wininet_HttpQueryInfoA},
      {"InternetCrackUrlA", (void*)&wininet_InternetCrackUrlA},
      {"InternetSetStatusCallback", (void*)&wininet_InternetSetStatusCallback},
  };
  trt_module_register("wininet.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

}  // extern "C"
