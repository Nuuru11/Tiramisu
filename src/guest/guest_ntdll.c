// tiramisu/src/guest/guest_ntdll.c
// Proves the ntdll native surface (work-order item 16 / spec 1K): NtCreateFile
// and NtReadFile ride the VFS, the Nt event surface shares kernel32's sync
// objects, and RaiseFailFastException is honored by the host trap.
// The guest writes PASS/FAIL lines to C:\results.txt through the VFS and
// returns a bitmask (mirrored by the host test's CHECKs).
//
//   0x001 NtCreateFile open existing -> STATUS_SUCCESS + handle
//   0x002 NtReadFile reads the exact host-written bytes
//   0x004 NtReadFile at EOF -> STATUS_END_OF_FILE
//   0x008 NtClose -> STATUS_SUCCESS
//   0x010 NtCreateFile FILE_CREATE on existing -> STATUS_OBJECT_NAME_COLLISION
//   0x020 NtCreateFile FILE_OPEN missing -> STATUS_OBJECT_NAME_NOT_FOUND
//   0x040 NtCreateFile+NtWriteFile+NtReadFile round-trip (own file)
//   0x080 NtQueryInformationFile(FileStandardInformation) on the Nt handle
//   0x100 NtCreateEvent (nameless) -> STATUS_SUCCESS
//   0x200 NtWaitForSingleObject unsignaled 50ms -> STATUS_TIMEOUT
//   0x400 NtSetEvent -> previous state 0, then wait succeeds
//   0x800 NtSignalAndWaitForSingleObject -> STATUS_SUCCESS
//   0x1000 NtWaitForSingleObject bad handle -> STATUS_INVALID_HANDLE
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int i32;
typedef long long i64;
typedef unsigned long long u64;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

int _fltused = 1;

// ---- kernel32 (results.txt writer — the proven path) ----
__attribute__((dllimport)) void* GSTD CreateFileW(const u16* name, u32 access,
                                                  u32 share, void* sa,
                                                  u32 disp, u32 flags,
                                                  void* tmpl);
__attribute__((dllimport)) int GSTD WriteFile(void* h, const void* buf,
                                              u32 len, u32* written,
                                              void* overlapped);
__attribute__((dllimport)) int GSTD CloseHandle(void* h);

// ---- ntdll (the surface under test) ----
__attribute__((dllimport)) u32 GSTD NtCreateFile(void** handle, u32 access,
                                                 void* attr, void* iosb,
                                                 const i64* alloc_size,
                                                 u32 file_attr, u32 share,
                                                 u32 disp, u32 options,
                                                 void* ea, u32 ea_len);
__attribute__((dllimport)) u32 GSTD NtReadFile(void* h, void* ev, void* apc,
                                               void* apc_ctx, void* iosb,
                                               void* buf, u32 len,
                                               const i64* byte_offset,
                                               u32* key);
__attribute__((dllimport)) u32 GSTD NtWriteFile(void* h, void* ev, void* apc,
                                                void* apc_ctx, void* iosb,
                                                const void* buf, u32 len,
                                                const i64* byte_offset,
                                                void* key);
__attribute__((dllimport)) u32 GSTD NtClose(void* h);
__attribute__((dllimport)) u32 GSTD NtQueryInformationFile(void* h, void* iosb,
                                                           void* info, u32 len,
                                                           u32 cls);
__attribute__((dllimport)) u32 GSTD NtCreateEvent(void** handle, u32 access,
                                                  void* attr, u32 event_type,
                                                  u8 initial_state);
__attribute__((dllimport)) u32 GSTD NtSetEvent(void* h, i32* previous_state);
__attribute__((dllimport)) u32 GSTD NtWaitForSingleObject(void* h,
                                                          u8 alertable,
                                                          const i64* timeout);
__attribute__((dllimport)) u32 GSTD NtSignalAndWaitForSingleObject(
    void* signal_handle, void* wait_handle, u8 alertable,
    const i64* timeout);
__attribute__((dllimport)) u32 GSTD RaiseFailFastException(void* exrec,
                                                           void* ctx,
                                                           u32 flags);

// ---- guest-side structures (x64 layouts) ----
typedef struct {
  u16 length;       // bytes
  u16 max_length;
  u32 pad;
  u16* buffer;
} uni_str;

typedef struct {
  u32 length;
  u32 pad;
  void* root_directory;
  uni_str* object_name;
  u32 attributes;
  u32 pad2;
  void* security_descriptor;
  void* security_qos;
} obj_attr;

typedef struct {
  u64 status;
  u64 info;
} iosb_t;

typedef struct {
  i64 alloc_size;
  i64 eof;
  u32 links;
  u8 delete_pending;
  u8 directory;
  u16 pad;
} file_std_info;

static void wcopy(const char* s, u16* w) {
  u32 i = 0;
  for (; s[i]; ++i) w[i] = (u16)(u8)s[i];
  w[i] = 0;
}

static u32 slen(const char* s) {
  u32 n = 0;
  while (s[n]) ++n;
  return n;
}

static void* g_res = 0;

static void emit(int ok, const char* name) {
  if (!g_res) return;
  char line[96];
  u32 n = 0;
  const char* tag = ok ? "PASS " : "FAIL ";
  for (; tag[n]; ++n) line[n] = tag[n];
  for (u32 i = 0; name[i] && n < sizeof(line) - 2; ++i) line[n++] = name[i];
  line[n++] = '\n';
  u32 written = 0;
  WriteFile(g_res, line, n, &written, (void*)0);
}

// "BITS 0x????????\n" — the return value never reaches the host (the
// fail-fast longjmps before the entry returns), so the bitmask travels
// through the VFS instead.
static void emit_bits(u32 bits) {
  if (!g_res) return;
  char line[24];
  const char* pre = "BITS 0x";
  u32 n = 0;
  for (; pre[n]; ++n) line[n] = pre[n];
  for (int i = 7; i >= 0; --i) {
    const u8 nib = (u8)((bits >> (i * 4)) & 0xFu);
    line[n++] = (char)(nib < 10 ? ('0' + nib) : ('A' + nib - 10));
  }
  line[n++] = '\n';
  u32 written = 0;
  WriteFile(g_res, line, n, &written, (void*)0);
}

u32 GSTD guest_ntdll_main(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  // results.txt through the kernel32 VFS path (proven surface)
  {
    u16 wpath[64];
    wcopy("C:\\results.txt", wpath);
    g_res = CreateFileW(wpath, 0x40000000u /*GENERIC_WRITE*/, 0, (void*)0,
                        2 /*CREATE_ALWAYS*/, 0, (void*)0);
  }

  // ---- 1. NtCreateFile: open the host-written probe file (\??\ form) ----
  void* fh = (void*)0;
  iosb_t iosb;
  obj_attr oa;
  uni_str us;
  u16 wname[64];
  wcopy("\\??\\C:\\ntprobe.txt", wname);
  us.length = slen("\\??\\C:\\ntprobe.txt") * 2;
  us.max_length = us.length + 2;
  us.pad = 0;
  us.buffer = wname;
  oa.length = 48;
  oa.pad = 0;
  oa.root_directory = (void*)0;
  oa.object_name = &us;
  oa.attributes = 0x40 /*OBJ_CASE_INSENSITIVE*/;
  oa.pad2 = 0;
  oa.security_descriptor = (void*)0;
  oa.security_qos = (void*)0;
  {
    const u32 st = NtCreateFile(&fh, 0x120089u /*FILE_GENERIC_READ*/, &oa,
                                &iosb, (const i64*)0, 0,
                                3 /*FILE_SHARE_READ|WRITE*/, 1 /*FILE_OPEN*/,
                                0x20 /*FILE_SYNCHRONOUS_IO_NONALERT*/,
                                (void*)0, 0);
    const int ok = (st == 0 && fh != (void*)0 && iosb.status == 0 &&
                    iosb.info == 1 /*FILE_OPENED*/);
    if (ok) bits |= 0x001u;
    emit(ok, "nt-create-open");
  }

  // ---- 2. NtReadFile: exact bytes the host wrote ----
  u8 got[32];
  u32 expect_fail = 0;
  {
    for (u32 i = 0; i < 32; ++i) got[i] = 0;
    const u32 st = NtReadFile(fh, (void*)0, (void*)0, (void*)0, &iosb, got,
                              32, (const i64*)0, (u32*)0);
    // host wrote bytes 0..31 as 0x40 + i ('A'..)
    u32 match = 1;
    for (u32 i = 0; i < 32; ++i)
      if (got[i] != (u8)(0x40u + i)) match = 0;
    const int ok = (st == 0 && iosb.status == 0 && iosb.info == 32 && match);
    if (ok) bits |= 0x002u;
    emit(ok, "nt-read-bytes");
  }

  // ---- 3. NtReadFile at EOF -> STATUS_END_OF_FILE ----
  {
    u8 sink[4];
    const u32 st = NtReadFile(fh, (void*)0, (void*)0, (void*)0, &iosb, sink,
                              4, (const i64*)0, (u32*)0);
    const int ok = (st == 0xC0000011u /*STATUS_END_OF_FILE*/ &&
                    iosb.status == 0xC0000011u && iosb.info == 0);
    if (ok) bits |= 0x004u;
    emit(ok, "nt-read-eof");
  }

  // ---- 4. NtQueryInformationFile(FileStandardInformation) on the Nt handle ----
  {
    file_std_info fsi;
    const u32 st = NtQueryInformationFile(fh, &iosb, &fsi, sizeof(fsi), 5);
    const int ok = (st == 0 && fsi.eof == 32 && fsi.links == 1);
    if (ok) bits |= 0x080u;
    emit(ok, "nt-query-std-info");
  }

  // ---- 5. NtClose ----
  {
    const u32 st = NtClose(fh);
    const int ok = (st == 0);
    if (ok) bits |= 0x008u;
    emit(ok, "nt-close");
    expect_fail = (st != 0);
  }

  // ---- 6. NtCreateFile FILE_CREATE on an existing file -> collision ----
  {
    void* h2 = (void*)0;
    const u32 st = NtCreateFile(&h2, 0x120089u, &oa, &iosb, (const i64*)0, 0,
                                3, 2 /*FILE_CREATE*/, 0, (void*)0, 0);
    const int ok = (st == 0xC0000035u /*STATUS_OBJECT_NAME_COLLISION*/ &&
                    h2 == (void*)0);
    if (ok) bits |= 0x010u;
    emit(ok, "nt-create-collision");
  }

  // ---- 7. NtCreateFile FILE_OPEN on a missing file -> name not found ----
  {
    u16 wmiss[64];
    obj_attr oa2 = oa;
    uni_str us2 = us;
    wcopy("\\??\\C:\\nt-missing.bin", wmiss);
    us2.length = slen("\\??\\C:\\nt-missing.bin") * 2;
    us2.max_length = us2.length + 2;
    us2.buffer = wmiss;
    oa2.object_name = &us2;
    void* h2 = (void*)0;
    const u32 st = NtCreateFile(&h2, 0x120089u, &oa2, &iosb, (const i64*)0, 0,
                                3, 1 /*FILE_OPEN*/, 0, (void*)0, 0);
    const int ok = (st == 0xC000000Fu /*STATUS_OBJECT_NAME_NOT_FOUND*/ &&
                    h2 == (void*)0);
    if (ok) bits |= 0x020u;
    emit(ok, "nt-create-missing");
  }

  // ---- 8. write round-trip through the Nt surface ----
  {
    u16 wout[64];
    wcopy("\\??\\C:\\ntout.txt", wout);
    us.length = slen("\\??\\C:\\ntout.txt") * 2;
    us.max_length = us.length + 2;
    us.buffer = wout;
    static const u8 payload[19] = {
        'N', 'T', '-', 'W', 'R', 'I', 'T', 'E', '-', 'O', 'K',
        0x54, 0x49, 0x52, 0x41, 0x4D, 0x49, 0x53, 0x55};
    void* h3 = (void*)0;
    u32 st = NtCreateFile(&h3, 0x120116u /*FILE_GENERIC_WRITE*/, &oa, &iosb,
                          (const i64*)0, 0, 3, 5 /*FILE_OVERWRITE_IF*/, 0x20,
                          (void*)0, 0);
    int ok = (st == 0 && h3 != (void*)0);
    if (ok) {
      st = NtWriteFile(h3, (void*)0, (void*)0, (void*)0, &iosb, payload, 19,
                       (const i64*)0, (void*)0);
      ok = (st == 0 && iosb.info == 19);
      NtClose(h3);
    }
    if (ok) {
      // reopen and verify the bytes came back
      void* h4 = (void*)0;
      st = NtCreateFile(&h4, 0x120089u, &oa, &iosb, (const i64*)0, 0, 3, 1,
                        0x20, (void*)0, 0);
      ok = (st == 0 && h4 != (void*)0);
      if (ok) {
        u8 back[24];
        for (u32 i = 0; i < 24; ++i) back[i] = 0;
        st = NtReadFile(h4, (void*)0, (void*)0, (void*)0, &iosb, back, 24,
                        (const i64*)0, (u32*)0);
        u32 match = 1;
        for (u32 i = 0; i < 19; ++i)
          if (back[i] != payload[i]) match = 0;
        ok = (st == 0 && iosb.info == 19 && match);
        NtClose(h4);
      }
    }
    if (ok) bits |= 0x040u;
    emit(ok, "nt-write-roundtrip");
  }

  // ---- 9. NtCreateEvent (nameless, auto-reset, initially unsignaled) ----
  void* evA = (void*)0;
  {
    const u32 st = NtCreateEvent(&evA, 0x00020000u /*EVENT_MODIFY_STATE*/,
                                 (void*)0, 1 /*SynchronizationEvent*/, 0);
    const int ok = (st == 0 && evA != (void*)0);
    if (ok) bits |= 0x100u;
    emit(ok, "nt-create-event");
  }

  // ---- 10. NtWaitForSingleObject unsignaled, 50ms -> STATUS_TIMEOUT ----
  {
    const i64 t50 = -500000;  // 50ms in 100ns units, negative = relative
    const u32 st = NtWaitForSingleObject(evA, 0, &t50);
    const int ok = (st == 0x102u /*STATUS_TIMEOUT*/);
    if (ok) bits |= 0x200u;
    emit(ok, "nt-wait-timeout");
  }

  // ---- 11. NtSetEvent: previous state 0, then the wait succeeds ----
  {
    i32 prev = -1;
    const u32 st = NtSetEvent(evA, &prev);
    const u32 wst = NtWaitForSingleObject(evA, 0, (const i64*)0);
    const int ok = (st == 0 && prev == 0 && wst == 0);
    if (ok) bits |= 0x400u;
    emit(ok, "nt-set-event");
  }

  // ---- 12. NtSignalAndWaitForSingleObject ----
  {
    void* evB = (void*)0;
    u32 st = NtCreateEvent(&evB, 0x00020000u, (void*)0,
                           0 /*NotificationEvent (manual)*/, 1 /*signaled*/);
    int ok = (st == 0 && evB != (void*)0);
    if (ok) {
      st = NtSignalAndWaitForSingleObject(evA, evB, 0, (const i64*)0);
      ok = (st == 0);
    }
    if (ok) bits |= 0x800u;
    emit(ok, "nt-signal-and-wait");
  }

  // ---- 13. bad handle -> STATUS_INVALID_HANDLE ----
  {
    void* bogus = (void*)(u64)0x1;
    i64 t0 = 0;
    const u32 st = NtWaitForSingleObject(bogus, 0, &t0);
    const int ok = (st == 0xC0000008u /*STATUS_INVALID_HANDLE*/);
    if (ok) bits |= 0x1000u;
    emit(ok, "nt-wait-bad-handle");
  }

  // ---- results done; THEN the fail-fast (the host trap catches it) ----
  if (g_res) {
    emit_bits(bits);
    const char done[] = "DONE\n";
    u32 written = 0;
    WriteFile(g_res, done, slen(done), &written, (void*)0);
    CloseHandle(g_res);
    g_res = (void*)0;
  }
  (void)expect_fail;
  RaiseFailFastException((void*)0, (void*)0, 0);

  // If the fail-fast path were NOT honored we would reach here: report it.
  emit(0, "fail-fast-NOT-honored");
  return bits | 0x80000000u;
}
