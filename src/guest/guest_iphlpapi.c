// tiramisu/src/guest/guest_iphlpapi.c
// Proves the iphlpapi bridge: adapter/interface tables answer honest
// no-data (the console has no network stack for the guest), and the ICMP
// probe surface gets real handle records whose echo honestly fails.
// Returns a bitmask:
//   0x001 GetAdaptersInfo -> ERROR_NO_DATA (232)
//   0x002 GetAdaptersAddresses -> ERROR_NO_DATA (232)
//   0x004 GetIfTable -> 0 rows (honest empty table)
//   0x008 IcmpCreateFile -> real handle record
//   0x010 IcmpSendEcho -> 0 (honest: no raw sockets)
//   0x020 IcmpCloseHandle -> TRUE (record freed)
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

__attribute__((dllimport)) u32 GSTD GetAdaptersInfo(void* info, u32* len);
__attribute__((dllimport)) u32 GSTD GetAdaptersAddresses(u32 family, u32 flags,
                                                         void* rsv, void* aa,
                                                         u32* len);
__attribute__((dllimport)) u32 GSTD GetIfTable(void* table, u32* size,
                                               int ordered);
__attribute__((dllimport)) void* GSTD IcmpCreateFile(void);
__attribute__((dllimport)) i32 GSTD IcmpSendEcho(void* h, u32 dest,
                                                 void* send, u16 ssize,
                                                 void* opts, void* reply,
                                                 u32 rsize, u32 timeout);
__attribute__((dllimport)) i32 GSTD IcmpCloseHandle(void* h);

u32 GSTD guest_iphlpapi_main(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  // ---- 1/2. adapter tables: honest no-data ----
  u32 len = 0;
  if (GetAdaptersInfo(0, &len) == 232) bits |= 0x001u;
  len = 0;
  char aa[16];
  if (GetAdaptersAddresses(0, 0, 0, aa, &len) == 232) bits |= 0x002u;

  // ---- 3. interface table: 0 rows, NO_ERROR ----
  u32 tsize = 64;
  char table[64];
  if (GetIfTable(table, &tsize, 0) == 0) bits |= 0x004u;

  // ---- 4/5/6. ICMP probes: record + honest echo failure + close ----
  void* h = IcmpCreateFile();
  if (h) bits |= 0x008u;
  char send[32], reply[64];
  if (IcmpSendEcho(h, 0x08080808, send, sizeof(send), 0, reply, sizeof(reply),
                   1000) == 0)
    bits |= 0x010u;
  if (IcmpCloseHandle(h)) bits |= 0x020u;

  return bits;
}
