// tiramisu/tests/test_mem_mapfile.cpp
// File-backed read-only MapViewOfFile ("map, don't copy"): a read-only view
// over a real archive is mmap'd straight out of the file's page cache, so it
// charges address space instead of the 5 GB commit ledger — the SSD and RAM
// overlap instead of competing (clean pages are reclaimable for free and the
// next touch re-faults from the SSD). Write/COW views and views reaching past
// EOF keep the private-copy path, whose write-back semantics must not change.
//
// Runs ARGLESS; the TRT_MAP_MODE=copy probe re-execs this binary with the env
// set (the mode is parsed once per process, like TRT_MAX_MAPS).
#include "../src/trt/trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
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
u64 TRT_STDCALL trt_CreateFileMappingW(u64 hfile, void* attrs, u32 protect,
                                       u32 max_high, u32 max_low,
                                       const u16* name);
void* TRT_STDCALL trt_MapViewOfFile(u64 h, u32 access, u32 off_high,
                                    u32 off_low, u64 bytes);
int TRT_STDCALL trt_UnmapViewOfFile(void* view);
void trt_mem_stats(u32* active_mappings, u32* active_views,
                   u64* total_view_bytes);
u64 trt_mem_budget_committed(void);
u64 trt_mem_budget_mapped(void);
u64 trt_mem_budget_reserved(void);
u64 trt_mem_budget_file_mapped(void);
}

// ---------------------------------------------------------------------------
// harness (same shape as test_membudget.cpp / test_mem_crash.cpp)
// ---------------------------------------------------------------------------

namespace {

std::vector<std::string> g_log;
void log_capture(const char* line) { g_log.emplace_back(line); }

struct extent { void* base; size_t len; };
extent g_extents[4096 + 8];
int g_extent_count = 0;

bool log_contains(const char* substr) {
  for (const std::string& l : g_log) {
    if (l.find(substr) != std::string::npos) return true;
  }
  return false;
}

int failures = 0;
#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (cond) printf("  PASS %s\n", msg);                            \
    else { printf("  FAIL %s\n", msg); ++failures; }                 \
  } while (0)

void* mmap_alloc(size_t bytes, u64 preferred) {
  (void)preferred;
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) return nullptr;
  if (g_extent_count < 4096 + 8) {
    g_extents[g_extent_count].base = p;
    g_extents[g_extent_count].len = bytes;
    ++g_extent_count;
  }
  return p;
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

void mmap_free(void* base, size_t) {
  for (int i = 0; i < g_extent_count; ++i) {
    if (g_extents[i].base == base) {
      munmap(base, g_extents[i].len);
      g_extents[i].base = nullptr;
      return;
    }
  }
}

void boot_runtime() {
  const trt_backend be{mmap_alloc, mmap_protect, mmap_free, log_capture};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
}

void make_u16(const char* s, u16* out, size_t cap) {
  size_t i = 0;
  for (; s[i] && i + 1 < cap; ++i) out[i] = (u16)(u8)s[i];
  out[i] = 0;
}

// deterministic byte pattern: any mismatch is a real read error, not chance
u8 expect_byte(u64 i) { return (u8)((i * 37ull + 11ull) & 0xFFull); }

constexpr u64 k_file_size = 1ull << 20;  // 1 MiB asset archive
constexpr u64 KiB = 1024;

// Creates <dir>/assets.pak filled with expect_byte(i). Returns false on error.
bool write_pattern_file(const char* dir, const char* name, std::vector<u8>* out) {
  char host[512];
  snprintf(host, sizeof(host), "%s/%s", dir, name);
  out->assign((size_t)k_file_size, 0);
  for (u64 i = 0; i < k_file_size; ++i) (*out)[(size_t)i] = expect_byte(i);
  FILE* f = fopen(host, "wb");
  if (!f) return false;
  const size_t put = fwrite(out->data(), 1, out->size(), f);
  fclose(f);
  return put == out->size();
}

bool matches(const std::vector<u8>& data, u64 off, const void* p, size_t len) {
  if (off + len > data.size()) return false;
  return memcmp(data.data() + off, p, len) == 0;
}

// ---------------------------------------------------------------------------
// TRT_MAP_MODE=copy probe (fresh process: the mode is parsed once)
// ---------------------------------------------------------------------------

int probe_copy_mode() {
  printf("probe: TRT_MAP_MODE=copy integration\n");
  boot_runtime();
  char tmpdir[] = "/tmp/tiramisu_mapfile_copy_XXXXXX";
  if (!mkdtemp(tmpdir)) {
    printf("  FAIL mkdtemp\n");
    return 1;
  }
  trt_vfs_mount("c:", tmpdir);
  std::vector<u8> data;
  if (!write_pattern_file(tmpdir, "assets.pak", &data)) {
    printf("  FAIL pattern file\n");
    return 1;
  }
  u16 wpath[128];
  make_u16("C:\\assets.pak", wpath, 128);
  void* fh = trt_CreateFileW(wpath, 0x80000000u, 1u, nullptr, 3u, 0u, nullptr);
  CHECK(fh && fh != (void*)(uintptr_t)-1, "read-only archive handle opens");

  const u64 base_committed = trt_mem_budget_committed();
  const u64 mh = trt_CreateFileMappingW((u64)(uintptr_t)fh, nullptr, 0x04, 0, 0,
                                        nullptr);
  CHECK(mh != 0, "mapping over the archive");
  void* v = trt_MapViewOfFile(mh, 0x04 /*READ*/, 0, 0, 64 * KiB);
  CHECK(v != nullptr, "read-only view maps");
  CHECK(matches(data, 0, v, 64 * KiB), "copy view carries the file bytes");
  CHECK(trt_mem_budget_file_mapped() == 0,
        "TRT_MAP_MODE=copy: no file-backed page-cache bytes");
  CHECK(trt_mem_budget_committed() == base_committed + 64 * KiB,
        "TRT_MAP_MODE=copy: the view charges the commit ledger");
  CHECK(!log_contains("map mode = file"),
        "TRT_MAP_MODE=copy never announces the file-backed path");
  CHECK(trt_UnmapViewOfFile(v) == 1, "copy view unmaps");
  CHECK(trt_mem_budget_committed() == base_committed,
        "copy view discharges the commit ledger");
  trt_CloseHandle((void*)(uintptr_t)mh);
  trt_CloseHandle(fh);
  char host[512];
  snprintf(host, sizeof(host), "%s/assets.pak", tmpdir);
  unlink(host);
  rmdir(tmpdir);

  printf("RESULT: %s (%d failures)\n", failures ? "FAILURES" : "ALL PASS",
         failures);
  return failures ? 1 : 0;
}

// ---------------------------------------------------------------------------
// default (file-backed) blocks
// ---------------------------------------------------------------------------

int run_default() {
  boot_runtime();
  char tmpdir[] = "/tmp/tiramisu_mapfile_XXXXXX";
  if (!mkdtemp(tmpdir)) {
    printf("  FAIL mkdtemp\n");
    return 1;
  }
  trt_vfs_mount("c:", tmpdir);
  std::vector<u8> data;
  if (!write_pattern_file(tmpdir, "assets.pak", &data)) {
    printf("  FAIL pattern file\n");
    return 1;
  }

  u16 wpath[128];
  make_u16("C:\\assets.pak", wpath, 128);
  void* fh = trt_CreateFileW(wpath, 0x80000000u, 1u, nullptr, 3u, 0u, nullptr);
  CHECK(fh && fh != (void*)(uintptr_t)-1, "read-only archive handle opens");

  const u64 base_committed = trt_mem_budget_committed();
  const u64 base_mapped = trt_mem_budget_mapped();
  const u64 base_reserved = trt_mem_budget_reserved();

  const u64 mh = trt_CreateFileMappingW((u64)(uintptr_t)fh, nullptr, 0x04, 0, 0,
                                        nullptr);
  CHECK(mh != 0, "mapping over the 1 MiB archive");

  // --- 1. read-only view: mapped, not copied ---
  void* v1 = trt_MapViewOfFile(mh, 0x04 /*FILE_MAP_READ*/, 0, 0, 256 * KiB);
  CHECK(v1 != nullptr, "read-only view maps");
  CHECK(matches(data, 0, v1, 256 * KiB), "view serves the file's bytes");
  CHECK(trt_mem_budget_file_mapped() == 256 * KiB,
        "view is counted as file-backed page cache (256 KiB)");
  CHECK(trt_mem_budget_mapped() == base_mapped,
        "view adds NO commit-backed mapped bytes");
  CHECK(trt_mem_budget_committed() == base_committed,
        "view adds NO commit at all (the 5 GB ledger is untouched)");
  CHECK(trt_mem_budget_reserved() == base_reserved + 256 * KiB,
        "view still charges its address space (VA), like Windows");
  CHECK(log_contains("map mode = file"),
        "named line: read-only views are file-backed");

  // --- 2. page-misaligned offset stays exact ---
  void* v2 = trt_MapViewOfFile(mh, 0x04, 0, 0x1234, 32 * KiB);
  CHECK(v2 != nullptr, "misaligned-offset view maps");
  CHECK(matches(data, 0x1234, v2, 32 * KiB),
        "misaligned view serves the exact bytes (page delta handled)");
  // span = page delta (0x234) + the 32 KiB view, rounded at the page start
  CHECK(trt_mem_budget_file_mapped() == 256 * KiB + 0x234 + 32 * KiB,
        "misaligned view charges the page-rounded mapping span");
  CHECK(trt_mem_budget_committed() == base_committed,
        "misaligned view still costs no commit");

  // --- 3. a WRITE view keeps the private-copy path (write-back semantics) ---
  const u64 before_write = trt_mem_budget_committed();
  const u64 file_before_write = trt_mem_budget_file_mapped();
  void* vw = trt_MapViewOfFile(mh, 0x02 /*FILE_MAP_WRITE*/, 0, 0, 16 * KiB);
  CHECK(vw != nullptr, "write view maps");
  CHECK(matches(data, 0, vw, 16 * KiB), "write view is populated from the file");
  CHECK(trt_mem_budget_file_mapped() == file_before_write,
        "write view is NOT file-backed (writes must not land in the archive)");
  CHECK(trt_mem_budget_committed() == before_write + 16 * KiB,
        "write view keeps the copy path's commit charge");
  CHECK(trt_UnmapViewOfFile(vw) == 1, "write view unmaps");
  CHECK(trt_mem_budget_committed() == before_write,
        "write view discharges its copy");

  // --- 3b. a COPY view is guest-writable and stays off the file path ---
  const u64 file_before_cow = trt_mem_budget_file_mapped();
  const u64 commit_before_cow = trt_mem_budget_committed();
  void* vc = trt_MapViewOfFile(mh, 0x01 /*FILE_MAP_COPY*/, 0, 0, 8 * KiB);
  CHECK(vc != nullptr, "copy-on-write view maps");
  CHECK(matches(data, 0, vc, 8 * KiB), "COW view is populated from the file");
  CHECK(trt_mem_budget_file_mapped() == file_before_cow,
        "COW view is NOT file-backed (the guest can write through it)");
  CHECK(trt_mem_budget_committed() == commit_before_cow + 8 * KiB,
        "COW view keeps the copy path's commit charge");
  ((u8*)vc)[0] = (u8)(data[0] ^ 0xFF);  // faults on a PROT_READ mapping
  CHECK(((const u8*)vc)[0] == (u8)(data[0] ^ 0xFF),
        "COW view accepts guest writes (private, not the archive)");
  CHECK(trt_UnmapViewOfFile(vc) == 1, "COW view unmaps");
  CHECK(trt_mem_budget_committed() == commit_before_cow,
        "COW view discharges its copy");

  // --- 4. a view reaching past EOF keeps the copy path (zero-filled tail) ---
  const u64 mh2 = trt_CreateFileMappingW((u64)(uintptr_t)fh, nullptr, 0x04, 0,
                                         (u32)(k_file_size + 0x1000), nullptr);
  CHECK(mh2 != 0, "mapping larger than the file (sparse-style max size)");
  const u64 before_eof = trt_mem_budget_committed();
  const u64 file_before_eof = trt_mem_budget_file_mapped();
  void* ve = trt_MapViewOfFile(mh2, 0x04, 0, (u32)(k_file_size - 0x800),
                               0x1000);
  CHECK(ve != nullptr, "view crossing EOF maps");
  CHECK(matches(data, k_file_size - 0x800, ve, 0x800),
        "bytes inside the file are real");
  bool tail_zero = true;
  for (size_t i = 0x800; i < 0x1000; ++i) {
    if (((const u8*)ve)[i] != 0) tail_zero = false;
  }
  CHECK(tail_zero, "bytes past EOF read as zero (copy path, not SIGBUS)");
  CHECK(trt_mem_budget_file_mapped() == file_before_eof,
        "cross-EOF view is not file-backed");
  CHECK(trt_mem_budget_committed() == before_eof + 0x1000,
        "cross-EOF view charges the copy path");
  CHECK(trt_UnmapViewOfFile(ve) == 1, "cross-EOF view unmaps");
  CHECK(trt_mem_budget_committed() == before_eof,
        "cross-EOF view discharges its copy");

  // --- 5. the archive on disk is untouched by read-only views ---
  char host[512];
  snprintf(host, sizeof(host), "%s/assets.pak", tmpdir);
  {
    FILE* f = fopen(host, "rb");
    bool same = false;
    if (f) {
      std::vector<u8> disk((size_t)k_file_size, 0);
      const size_t got = fread(disk.data(), 1, disk.size(), f);
      fclose(f);
      same = got == disk.size() && disk == data;
    }
    CHECK(same, "views left the archive byte-identical (COW write not flushed)");
  }

  // --- 6. registry unwinds exactly: nothing charged, nothing leaked ---
  CHECK(trt_UnmapViewOfFile(v1) == 1, "read-only view unmaps");
  CHECK(trt_UnmapViewOfFile(v2) == 1, "misaligned view unmaps");
  CHECK(trt_mem_budget_file_mapped() == 0,
        "unmap releases the file-backed bytes");
  CHECK(trt_mem_budget_committed() == base_committed,
        "commit ledger back to baseline");
  CHECK(trt_mem_budget_reserved() == base_reserved,
        "VA ledger back to baseline");
  u32 maps = 0, live = 0;
  u64 view_bytes = 0;
  trt_mem_stats(&maps, &live, &view_bytes);
  CHECK(live == 0 && view_bytes == 0, "view registry empty");
  trt_CloseHandle((void*)(uintptr_t)mh2);
  trt_CloseHandle((void*)(uintptr_t)mh);
  trt_CloseHandle(fh);
  // A small window beyond the 32-bit boundary in a sparse asset archive.
  // Closing both handles before reading also exercises view lifetime.
  constexpr u64 marker_offset = (1ULL << 32) + 0x12345;
  constexpr char marker[] = "archive window";
  FILE* sparse = fopen(host, "wb");
  bool wrote = sparse && fseeko(sparse, static_cast<off_t>(marker_offset), SEEK_SET) == 0 &&
               fwrite(marker, 1, sizeof(marker), sparse) == sizeof(marker);
  if (sparse && fclose(sparse) != 0) wrote = false;
  CHECK(wrote, "created sparse archive larger than 4 GiB");
  if (wrote) {
    fh = trt_CreateFileW(wpath, 0x80000000u, 1, nullptr, 3, 0, nullptr);
    const u64 mapping = trt_CreateFileMappingW((u64)(uintptr_t)fh, nullptr, 0x02, 0, 0, nullptr);
    trt_CloseHandle(fh);
    void* window = trt_MapViewOfFile(mapping, 0x04, 1, 0x12345, sizeof(marker));
    trt_CloseHandle((void*)(uintptr_t)mapping);
    CHECK(window && memcmp(window, marker, sizeof(marker)) == 0, "64-bit archive offset and view lifetime");
    CHECK(window && trt_mem_budget_committed() == base_committed && trt_mem_budget_file_mapped() > 0,
          "high-offset window uses file-backed pages");
    CHECK(window && trt_UnmapViewOfFile(window) && trt_mem_budget_reserved() == base_reserved,
          "high-offset view releases its reservation");
  }
  unlink(host);
  rmdir(tmpdir);

  printf("RESULT: %s (%d failures)\n", failures ? "FAILURES" : "ALL PASS",
         failures);
  return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc >= 2 && strcmp(argv[1], "--probe-copy-mode") == 0) {
    return probe_copy_mode();
  }

  printf("tiramisu file-backed read-only MapViewOfFile suite\n");
  unsetenv("TRT_MAP_MODE");
  const int rc = run_default();

  printf("re-exec with TRT_MAP_MODE=copy for the copy-path probe\n");
  fflush(stdout);
  const pid_t pid = fork();
  if (pid == 0) {
    setenv("TRT_MAP_MODE", "copy", 1);
    execl("/proc/self/exe", "test_mem_mapfile", "--probe-copy-mode",
          (char*)nullptr);
    _exit(127);
  }
  int wstatus = 0;
  waitpid(pid, &wstatus, 0);
  const int probe_rc = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 1;
  if (probe_rc != 0) {
    printf("  FAIL TRT_MAP_MODE=copy probe (fresh process)\n");
    return 1;
  }
  return rc;
}
