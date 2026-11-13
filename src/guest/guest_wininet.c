// tiramisu/src/guest/guest_wininet.c
// Proves the wininet bridge: WinINet is NOT available to UWP, so the
// bridge binds and answers honestly — InternetOpen gives a real handle
// record, the send verb fails with ERROR_INTERNET_CANNOT_CONNECT, the read
// reports clean EOF, and the close frees the record. Never silent-success.
// Returns a bitmask:
//   0x001 InternetOpenA -> non-null handle record
//   0x002 InternetOpenW -> non-null handle record
//   0x004 InternetConnectA -> non-null connect record
//   0x008 HttpOpenRequestA -> non-null request record
//   0x010 HttpSendRequestA -> FALSE (named: cannot connect)
//   0x020 HttpQueryInfoA -> FALSE (no data)
//   0x040 InternetCrackUrlA -> FALSE (named)
//   0x080 InternetReadFile -> TRUE with 0 bytes (EOF, no fake payload)
//   0x100 InternetCloseHandle -> TRUE (record freed)
//   0x200 InternetCloseHandle on a bogus handle -> FALSE (honest)
typedef unsigned short u16;
typedef unsigned int u32;
typedef int i32;
typedef unsigned long long u64;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

int _fltused = 1;

__attribute__((dllimport)) void* GSTD InternetOpenA(const char* agent,
                                                    u32 access,
                                                    const char* proxy,
                                                    const char* bypass,
                                                    u32 flags);
__attribute__((dllimport)) void* GSTD InternetOpenW(const u16* agent,
                                                    u32 access,
                                                    const u16* proxy,
                                                    const u16* bypass,
                                                    u32 flags);
__attribute__((dllimport)) void* GSTD InternetConnectA(void* h,
                                                       const char* server,
                                                       u16 port,
                                                       const char* user,
                                                       const char* pass,
                                                       u32 service, u32 flags,
                                                       u64 ctx);
__attribute__((dllimport)) void* GSTD HttpOpenRequestA(void* conn,
                                                       const char* verb,
                                                       const char* object,
                                                       const char* version,
                                                       const char* referrer,
                                                       const char** types,
                                                       u32 flags, u64 ctx);
__attribute__((dllimport)) i32 GSTD HttpSendRequestA(void* req,
                                                     const char* headers,
                                                     u32 hdr_len, void* opt,
                                                     u32 opt_len);
__attribute__((dllimport)) i32 GSTD HttpQueryInfoA(void* req, u32 info,
                                                   void* buf, u32* len,
                                                   u32* idx);
__attribute__((dllimport)) i32 GSTD InternetCrackUrlA(const char* url, u32 len,
                                                      u32 flags, void* comp);
__attribute__((dllimport)) i32 GSTD InternetReadFile(void* file, void* buf,
                                                     u32 bytes, u32* read);
__attribute__((dllimport)) i32 GSTD InternetCloseHandle(void* h);
__attribute__((dllimport)) void* GSTD InternetSetStatusCallback(void* h,
                                                                void* cb);

u32 GSTD guest_wininet_main(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  // ---- 1. agent handles: real records ----
  void* h1 = InternetOpenA("TiramisuGuest/1.0", 0, 0, 0, 0);
  if (h1) bits |= 0x001u;
  void* h2 = InternetOpenW((const u16*)L"TiramisuGuest/1.0", 0, 0, 0, 0);
  if (h2) bits |= 0x002u;

  // ---- 2. connect + open request: records chain off the agent handle ----
  void* conn = InternetConnectA(h1, "example.com", 80, 0, 0, 3, 0, 0);
  if (conn) bits |= 0x004u;
  void* req = HttpOpenRequestA(conn, "GET", "/", 0, 0, 0, 0, 0);
  if (req) bits |= 0x008u;

  // ---- 3. the honest send failure (cannot connect, no network) ----
  if (HttpSendRequestA(req, 0, 0, 0, 0) == 0) bits |= 0x010u;
  u32 qlen = 64;
  char qbuf[64];
  if (HttpQueryInfoA(req, 19 /*HTTP_QUERY_STATUS_CODE*/, qbuf, &qlen, 0) == 0)
    bits |= 0x020u;
  struct { u32 scheme; char host[64]; } comp;
  if (InternetCrackUrlA("http://example.com/x", 0, 0, &comp) == 0)
    bits |= 0x040u;

  // ---- 4. read: clean EOF, never fake data ----
  u32 got = 99;
  char rbuf[16];
  if (InternetReadFile(req, rbuf, sizeof(rbuf), &got) && got == 0)
    bits |= 0x080u;

  // ---- 5. close: records freed; a bogus handle fails honestly ----
  if (InternetCloseHandle(req) && InternetCloseHandle(conn) &&
      InternetCloseHandle(h1) && InternetCloseHandle(h2))
    bits |= 0x100u;
  if (InternetCloseHandle((void*)0x57575757) == 0) bits |= 0x200u;
  (void)InternetSetStatusCallback(h2, 0);  // would be a double-close handle;
                                           // callback surface just answers NULL

  return bits;
}
