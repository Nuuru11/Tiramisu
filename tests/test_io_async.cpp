// tiramisu/tests/test_io_async.cpp
// The async file-I/O contract (Wine/Windows semantics) for streaming engines:
//
//   1. Overlapped ReadFile at a 64-bit offset completes with the OVERLAPPED
//      filled (Internal=0, InternalHigh=n), hEvent signaled, and a completion
//      packet dequeued by GetQueuedCompletionStatus with the association's
//      completion key and the caller's OVERLAPPED pointer.
//   2. SetFileCompletionNotificationModes(FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)
//      is REAL: successful overlapped I/O signals the event but posts NO
//      packet (previously an ok-stub — the game believed the skip was active
//      and got phantom double-completions).
//   3. Invalid mode flags fail with ERROR_INVALID_PARAMETER (87), named.
//   4. Multiple overlapped reads queue packets in FIFO order.
//
// Runs ARGLESS host-side through the bridge entry points (the same functions
// the guest IAT resolves to), like test_mem_crash / test_membudget.
#include "../src/trt/trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string>
#include <vector>

extern "C" {
int trt_bridge_register_kernel32(void);
void trt_vfs_mount(const char* drive, const char* host_dir);

void* TRT_STDCALL trt_CreateFileW(const u16* path, u32 access, u32 share,
                                  void* security, u32 disposition, u32 flags,
                                  void* template_handle);
int TRT_STDCALL trt_CloseHandle(void* h);
int TRT_STDCALL trt_ReadFile(void* h, void* buf, u32 bytes, u32* read,
                             void* overlapped);
u64 TRT_STDCALL trt_CreateIoCompletionPort(void* file_handle, u64 existing,
                                           u64 key, u32 flags);
int TRT_STDCALL trt_GetQueuedCompletionStatus(u64 port, u32* bytes, u64* key,
                                              void** overlapped, u32 ms);
int TRT_STDCALL trt_SetFileCompletionNotificationModes(void* file_handle,
                                                       u8 flags);
int TRT_STDCALL trt_WriteFile(void* h, const void* buf, u32 bytes, u32* written,
                              void* overlapped);
u32 TRT_STDCALL trt_GetLastError(void);
// kernel32 event surface (OVERLAPPED.hEvent is a real event handle)
u64 TRT_STDCALL trt_CreateEventW(void* attrs, u32 manual, u32 initial,
                                 const u16* name);
int TRT_STDCALL trt_WaitForSingleObject(u64 h, u32 ms);
}

namespace {

std::vector<std::string> g_log;
void log_capture(const char* line) { g_log.emplace_back(line); }
void log_quiet(const char*) {}

int log_count(const char* substr) {
  int n = 0;
  for (const std::string& l : g_log) {
    if (l.find(substr) != std::string::npos) ++n;
  }
  return n;
}
bool log_contains(const char* substr) { return log_count(substr) > 0; }

int failures = 0;
#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (cond) printf("  PASS %s\n", msg);                            \
    else { printf("  FAIL %s\n", msg); ++failures; }                 \
  } while (0)

void* mmap_alloc(size_t bytes, u64) {
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? nullptr : p;
}
int mmap_protect(void* base, size_t bytes, unsigned prot) {
  int p = 0;
  if (prot & TRT_PROT_R) p |= PROT_READ;
  if (prot & TRT_PROT_W) p |= PROT_WRITE;
  if (prot & TRT_PROT_X) p |= PROT_EXEC;
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  const uintptr_t b = (uintptr_t)base & ~(uintptr_t)(page - 1);
  const size_t len =
      ((uintptr_t)base + bytes - b + page - 1) & ~(uintptr_t)(page - 1);
  return mprotect((void*)b, len, p);
}
void mmap_free(void* base, size_t) { munmap(base, 0); }

void boot_runtime(bool capture_log) {
  const trt_backend be{mmap_alloc, mmap_protect, mmap_free,
                       capture_log ? log_capture : log_quiet};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
}

void make_u16(const char* s, u16* out, size_t cap) {
  size_t i = 0;
  for (; s[i] && i + 1 < cap; ++i) out[i] = (u16)s[i];
  out[i] = 0;
}

// OVERLAPPED as the guest sees it (x64): Internal, InternalHigh,
// Offset+OffsetHigh (u64 at +16), hEvent (+24).
struct ov_t {
  u64 internal;
  u64 internal_high;
  u64 offset;
  u64 hevent;
};

}  // namespace

int main() {
  setvbuf(stdout, NULL, _IONBF, 0);

  printf("tiramisu async file-I/O contract suite\n");

  char tmpdir[] = "/tmp/tiramisu_ioasync_XXXXXX";
  if (!mkdtemp(tmpdir)) { printf("cannot mkdtemp\n"); return 2; }
  boot_runtime(true);
  trt_vfs_mount("c:", tmpdir);

  // Seed a file with known bytes: 64 KiB, byte i = (i*7)&0xFF.
  u16 fname[64];
  make_u16("c:\\ioasync.bin", fname, 64);
  void* fh = trt_CreateFileW(fname, 0x40000000 /*GENERIC_WRITE*/, 0, nullptr,
                             2 /*CREATE_ALWAYS*/, 0x80 /*NORMAL*/, nullptr);
  CHECK(fh != nullptr, "seed file created");
  {
    static u8 seed[65536];
    for (int i = 0; i < 65536; ++i) seed[i] = (u8)((i * 7) & 0xFF);
    u32 written = 0;
    CHECK(trt_WriteFile(fh, seed, sizeof(seed), &written, nullptr) == 1 &&
              written == sizeof(seed),
          "seed bytes written");
    trt_CloseHandle(fh);
  }

  fh = trt_CreateFileW(fname, 0x80000000 /*GENERIC_READ*/, 1 /*SHARE_READ*/,
                       nullptr, 3 /*OPEN_EXISTING*/, 0x80, nullptr);
  CHECK(fh != nullptr, "seed file reopened for read");

  // =========================================================================
  // Block A — overlapped read at a 64-bit offset -> packet + event + fields
  // =========================================================================
  printf("block A: overlapped read -> GQCS packet contract\n");
  {
    const u64 port = trt_CreateIoCompletionPort(fh, 0, 0xCAFEBABEu, 1);
    CHECK(port != 0, "file associated with a completion port");

    u64 ev = trt_CreateEventW(nullptr, 0, 0, nullptr);
    CHECK(ev != 0, "overlay event created");

    ov_t ov{};
    ov.offset = 4096;  // deep offset: the 64-bit Offset/OffsetHigh pair
    ov.hevent = ev;
    static u8 buf[8192];
    memset(buf, 0xAA, sizeof(buf));
    u32 got = 0;
    CHECK(trt_ReadFile(fh, buf, sizeof(buf), &got, &ov) == 1 && got == sizeof(buf),
          "overlapped ReadFile at offset 4096 returns the full count");

    CHECK(ov.internal == 0, "OVERLAPPED.Internal = 0 (success)");
    CHECK(ov.internal_high == sizeof(buf),
          "OVERLAPPED.InternalHigh = bytes transferred");
    CHECK(trt_WaitForSingleObject(ev, 0) == 0,
          "OVERLAPPED.hEvent signaled on completion");

    // payload came from the RIGHT offset
    bool payload_ok = true;
    for (int i = 0; i < 8192; ++i) {
      if (buf[i] != (u8)(((4096 + i) * 7) & 0xFF)) { payload_ok = false; break; }
    }
    CHECK(payload_ok, "bytes read are the file's bytes at Offset=4096");

    u32 q_bytes = 0;
    u64 q_key = 0;
    void* q_ov = nullptr;
    CHECK(trt_GetQueuedCompletionStatus(port, &q_bytes, &q_key, &q_ov, 500) == 1,
          "GQCS dequeues the completion packet");
    CHECK(q_bytes == sizeof(buf), "packet carries the transferred byte count");
    CHECK(q_key == 0xCAFEBABEu, "packet carries the association's key");
    CHECK(q_ov == &ov, "packet carries the caller's OVERLAPPED pointer");

    // queue drained: the next dequeue times out
    u32 b2 = 0; u64 k2 = 0; void* o2 = nullptr;
    CHECK(trt_GetQueuedCompletionStatus(port, &b2, &k2, &o2, 50) == 0 &&
              trt_GetLastError() == 258,
          "drained queue times out (WAIT_TIMEOUT)");

    trt_CloseHandle((void*)(uintptr_t)port);
  }

  // =========================================================================
  // Block B — FILE_SKIP_COMPLETION_PORT_ON_SUCCESS is real (no packet)
  // =========================================================================
  printf("block B: skip-completion-on-success honored\n");
  {
    const u64 port = trt_CreateIoCompletionPort(fh, 0, 0x1111u, 1);
    CHECK(port != 0, "file associated (second port)");

    CHECK(trt_SetFileCompletionNotificationModes(fh, 0x1) == 1,
          "SetFileCompletionNotificationModes(SKIP_ON_SUCCESS) accepted");

    u64 ev = trt_CreateEventW(nullptr, 0, 0, nullptr);
    ov_t ov{};
    ov.offset = 0;
    ov.hevent = ev;
    static u8 buf[4096];
    u32 got = 0;
    CHECK(trt_ReadFile(fh, buf, sizeof(buf), &got, &ov) == 1 && got == sizeof(buf),
          "overlapped read succeeds with the skip flag set");
    CHECK(trt_WaitForSingleObject(ev, 0) == 0,
          "hEvent STILL signaled (skip suppresses only the packet)");
    CHECK(ov.internal_high == sizeof(buf), "InternalHigh still filled");

    u32 q_bytes = 0; u64 q_key = 0; void* q_ov = nullptr;
    CHECK(trt_GetQueuedCompletionStatus(port, &q_bytes, &q_key, &q_ov, 50) == 0,
          "NO completion packet posted for a skipped success");
    CHECK(trt_GetLastError() == 258, "the no-packet wait is a clean timeout");

    // restore: clearing the flag (modes=0) resumes packet posting
    CHECK(trt_SetFileCompletionNotificationModes(fh, 0) == 1,
          "modes=0 accepted (packets resume)");
    ov_t ov2{};
    ov2.offset = 8192;
    u32 got2 = 0;
    CHECK(trt_ReadFile(fh, buf, sizeof(buf), &got2, &ov2) == 1,
          "post-restore overlapped read succeeds");
    CHECK(trt_GetQueuedCompletionStatus(port, &q_bytes, &q_key, &q_ov, 500) == 1,
          "packets post again after the flag clears");

    trt_CloseHandle((void*)(uintptr_t)port);
  }

  // =========================================================================
  // Block C — mode-flag validation is honest
  // =========================================================================
  printf("block C: mode-flag validation\n");
  {
    CHECK(trt_SetFileCompletionNotificationModes(fh, 0x4) == 0,
          "invalid flag combination (0x4) rejected");
    CHECK(trt_GetLastError() == 87, "rejection sets ERROR_INVALID_PARAMETER");
    CHECK(trt_SetFileCompletionNotificationModes(fh, 0x3) == 1,
          "valid combination (0x3) accepted");
    CHECK(trt_SetFileCompletionNotificationModes(nullptr, 0x1) == 0,
          "null handle rejected");
    CHECK(log_contains("completion notification modes"),
          "mode changes log a named line");
    trt_SetFileCompletionNotificationModes(fh, 0);  // leave clean
  }

  // =========================================================================
  // Block D — packets queue in FIFO order
  // =========================================================================
  printf("block D: FIFO packet order\n");
  {
    const u64 port = trt_CreateIoCompletionPort(fh, 0, 0x2222u, 1);
    ov_t ovs[3] = {};
    for (int i = 0; i < 3; ++i) {
      ovs[i].offset = (u64)(i + 1) * 16384;  // distinct deep offsets
      static u8 buf[512];
      u32 got = 0;
      CHECK(trt_ReadFile(fh, buf, sizeof(buf), &got, &ovs[i]) == 1,
            "overlapped read queued");
    }
    for (int i = 0; i < 3; ++i) {
      u32 qb = 0; u64 qk = 0; void* qo = nullptr;
      CHECK(trt_GetQueuedCompletionStatus(port, &qb, &qk, &qo, 500) == 1,
            "packet dequeued");
      CHECK(qo == &ovs[i], "FIFO order preserved");
    }
    trt_CloseHandle((void*)(uintptr_t)port);
  }

  // =========================================================================
  // Block E — mmap fast path: large positional reads (streaming engines)
  // =========================================================================
  printf("block E: mmap fast path (large positional reads)\n");
  {
    // 4 MiB file, byte i = (i*7)&0xFF — above the 1 MiB TRT_IO_MMAP_MIN default
    u16 big[64];
    make_u16("c:\\ioasync_big.bin", big, 64);
    void* bf = trt_CreateFileW(big, 0x40000000 /*GENERIC_WRITE*/, 0, nullptr,
                               2 /*CREATE_ALWAYS*/, 0x80, nullptr);
    CHECK(bf != nullptr, "4 MiB stream file created");
    static u8 chunk[65536];
    const u32 k_total = 4u * 1024 * 1024;
    bool wok = true;
    for (u32 base = 0; base < k_total && wok; base += sizeof(chunk)) {
      for (u32 i = 0; i < sizeof(chunk); ++i)
        chunk[i] = (u8)(((base + i) * 7) & 0xFF);
      u32 w = 0;
      wok = trt_WriteFile(bf, chunk, sizeof(chunk), &w, nullptr) == 1 &&
            w == sizeof(chunk);
    }
    CHECK(wok, "4 MiB pattern written in 64 KiB chunks");
    trt_CloseHandle(bf);
    bf = trt_CreateFileW(big, 0x80000000 /*GENERIC_READ*/, 1, nullptr,
                         3 /*OPEN_EXISTING*/, 0x80, nullptr);
    CHECK(bf != nullptr, "stream file reopened for read");

    const u64 port = trt_CreateIoCompletionPort(bf, 0, 0x3333u, 1);
    static u8 big_buf[2 * 1024 * 1024];
    memset(big_buf, 0xAA, sizeof(big_buf));
    u64 ev = trt_CreateEventW(nullptr, 0, 0, nullptr);

    ov_t ov{};
    ov.offset = 1024 * 1024;  // deep offset, 2 MiB read -> the mmap path
    ov.hevent = ev;
    u32 got = 0;
    CHECK(trt_ReadFile(bf, big_buf, sizeof(big_buf), &got, &ov) == 1 &&
              got == sizeof(big_buf),
          "2 MiB overlapped read at offset 1 MiB returns the full count");
    bool pok = true;
    for (u32 i = 0; i < sizeof(big_buf); ++i)
      if (big_buf[i] != (u8)(((1024u * 1024 + i) * 7) & 0xFF)) {
        pok = false;
        break;
      }
    CHECK(pok, "mmap-path payload matches the file's bytes at Offset=1 MiB");
    CHECK(trt_WaitForSingleObject(ev, 0) == 0,
          "event signaled on the large read");

    // EOF clamp: a 1 MiB read starting 100 bytes before EOF returns 100
    ov_t ov2{};
    ov2.offset = (u64)k_total - 100;
    ov2.hevent = ev;
    got = 0;
    CHECK(trt_ReadFile(bf, big_buf, sizeof(big_buf), &got, &ov2) == 1 &&
              got == 100,
          "read crossing EOF clamps to the remaining 100 bytes");

    // past EOF: a clean zero-byte read (the fread path's exact contract)
    ov_t ov3{};
    ov3.offset = (u64)k_total + 4096;
    ov3.hevent = ev;
    got = 1;
    CHECK(trt_ReadFile(bf, big_buf, sizeof(big_buf), &got, &ov3) == 1 &&
              got == 0,
          "read past EOF returns a clean zero-byte read");

    trt_CloseHandle((void*)(uintptr_t)port);
    trt_CloseHandle(bf);
  }

  trt_CloseHandle(fh);

  printf("RESULT: %s (%d failures)\n", failures ? "FAILURES" : "ALL PASS",
         failures);
  return failures ? 1 : 0;
}
