// tiramisu/tests/test_ntdll.cpp
// The ntdll native surface (work-order item 16 / spec 1K): a guest PE64
// opens and reads a host-written file through NtCreateFile/NtReadFile on the
// VFS, round-trips a write, exercises the Nt event surface (which must share
// kernel32's sync objects), writes PASS/FAIL lines to C:\results.txt, and
// finally calls RaiseFailFastException — honored via the boot exit trap
// (longjmp back here), the same fatal path ExitProcess uses.
#include "../src/trt/trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_ntdll(void);
}

namespace {

struct extent { void* base; size_t len; };
extent g_extents[16];
int g_extent_count = 0;

void* linux_alloc(size_t bytes, u64 preferred_base) {
  void* p = mmap(preferred_base ? (void*)(uintptr_t)preferred_base : nullptr,
                 bytes, PROT_READ | PROT_WRITE,
                 preferred_base ? (MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS)
                                : (MAP_PRIVATE | MAP_ANONYMOUS),
                 -1, 0);
  if (p == MAP_FAILED) return nullptr;
  if (g_extent_count < 16) {
    g_extents[g_extent_count].base = p;
    g_extents[g_extent_count].len = bytes;
    ++g_extent_count;
  }
  return p;
}
int linux_protect(void* base, size_t bytes, unsigned prot) {
  int p = 0;
  if (prot & TRT_PROT_R) p |= PROT_READ;
  if (prot & TRT_PROT_W) p |= PROT_WRITE;
  if (prot & TRT_PROT_X) p |= PROT_EXEC;
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  const uintptr_t b = (uintptr_t)base & ~(uintptr_t)(page - 1);
  const size_t len = ((uintptr_t)base + bytes - b + page - 1) & ~(uintptr_t)(page - 1);
  return mprotect((void*)b, len, p);
}
void linux_release(void* base, size_t) {
  for (int i = 0; i < g_extent_count; ++i)
    if (g_extents[i].base == base) {
      munmap(base, g_extents[i].len);
      g_extents[i].base = nullptr;
      return;
    }
}
void linux_log(const char* line) { printf("  [trt] %s\n", line); }

u8* read_file(const char* path, size_t* size) {
  FILE* f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  u8* buf = (u8*)malloc((size_t)n);
  const size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  *size = got;
  return buf;
}

// call a Win64 guest: shadow space + rcx/rdx/r8, result in rax
u64 win64_call(void* fn, u64 a, u64 b, u64 c) {
  u64 args[3] = {a, b, c};
  u64 ret;
  asm volatile(
    "movq %[a0], %%rcx\n"
    "movq %[a1], %%rdx\n"
    "movq %[a2], %%r8\n"
    "subq $48, %%rsp\n"
    "call *%[fn]\n"
    "addq $48, %%rsp\n"
    : "=a"(ret)
    : [a0] "m"(args[0]), [a1] "m"(args[1]), [a2] "m"(args[2]),
      [fn] "r"(fn)
    : "rcx", "rdx", "r8", "r9", "r10", "r11",
      "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "memory", "cc");
  return ret;
}

int failures = 0;
#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (cond) printf("  PASS %s\n", msg);                            \
    else { printf("  FAIL %s\n", msg); ++failures; }                 \
  } while (0)

// ---- fail-fast trap: RaiseFailFastException must longjmp back here, the
// same way the offline boot harness catches ExitProcess ----
jmp_buf g_ff_back;
volatile int g_ff_trapped = 0;
volatile int g_ff_code = -1;

void ff_trap(int code) {
  g_ff_trapped = 1;
  g_ff_code = code;
  longjmp(g_ff_back, 1);
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc < 2) {
    printf("usage: test_ntdll <guest.exe>\n");
    return 2;
  }

  printf("tiramisu ntdll native surface test — %s\n", argv[1]);

  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &linux_log,
                 nullptr};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_ntdll();

  // VFS: C:\ -> scratch drive_c, Z:\ -> the test's own dir
  char drive_c[] = "/tmp/tiramisu-ntdll.XXXXXX";
  mkdtemp(drive_c);
  char exe_dir[512];
  snprintf(exe_dir, sizeof(exe_dir), "%s", argv[1]);
  *(strrchr(exe_dir, '/')) = 0;
  trt_vfs_mount("c:", drive_c);
  trt_vfs_mount("z:", exe_dir);
  trt_bridge_set_exe_path("Z:\\guest_ntdll.exe");

  // the probe file the guest must read through NtCreateFile/NtReadFile:
  // 32 bytes, byte i == 0x40 + i
  {
    std::string probe = std::string(drive_c) + "/ntprobe.txt";
    FILE* pf = fopen(probe.c_str(), "wb");
    if (!pf) { printf("cannot write probe file\n"); return 2; }
    for (u32 i = 0; i < 32; ++i) fputc((int)(0x40u + i), pf);
    fclose(pf);
  }

  size_t size = 0;
  u8* file = read_file(argv[1], &size);
  if (!file) { printf("cannot read %s\n", argv[1]); return 2; }

  trt_resolver rs;
  trt_resolver_default(&rs);
  trt_image* img = nullptr;
  const int rc = trt_map_pe(&be, &rs, file, size, &img);
  CHECK(rc == TRT_OK, "guest PE maps (kernel32+ntdll bound)");
  if (rc != TRT_OK || !img) {
    printf("  map failed: %s\n", trt_errstr(rc));
    free(file);
    return 1;
  }
  CHECK(trt_seal_image(&be, img) == TRT_OK, "image sealed (W^X)");

  // RaiseFailFastException is called LAST by the guest: arm the trap, run,
  // and expect the longjmp — a normal return means fail-fast was ignored.
  // The guest's return value never reaches the host in that flow, so the
  // guest writes "BITS 0x????????\n" into results.txt before the fail-fast.
  trt_boot_exit_trap = &ff_trap;
  u64 ret = 0;
  if (setjmp(g_ff_back) == 0) {
    ret = win64_call(img->base + img->entry_rva, 0, 0, 0);
  }
  trt_boot_exit_trap = nullptr;

  CHECK(g_ff_trapped == 1,
        "RaiseFailFastException -> fail-fast honored (exit trap fired)");
  CHECK(g_ff_code == 0x09, "fail-fast exit code 0x09 (0xC0000409 low byte)");
  if (g_ff_trapped != 1) {
    char d[96];
    snprintf(d, sizeof(d), "guest RETURNED 0x%08llx (fail-fast ignored!)",
             (unsigned long long)ret);
    CHECK((ret & 0x80000000u) == 0, d);
  }

  // the guest wrote PASS/FAIL lines through the VFS — verify ON DISK
  std::string res_path = std::string(drive_c) + "/results.txt";
  FILE* rf = fopen(res_path.c_str(), "rb");
  CHECK(rf != nullptr, "guest created C:\\results.txt through the VFS");
  if (rf) {
    fseek(rf, 0, SEEK_END);
    const long n = ftell(rf);
    fseek(rf, 0, SEEK_SET);
    std::string text((size_t)n, 0);
    const size_t got = fread(&text[0], 1, (size_t)n, rf);
    fclose(rf);
    text.resize(got);

    // recover the guest's verdict bitmask ("BITS 0x????????\n")
    u32 bits = 0;
    const size_t bp = text.find("BITS 0x");
    if (bp != std::string::npos && bp + 7 + 8 <= text.size()) {
      for (int i = 0; i < 8; ++i) {
        const char c = text[bp + 7 + i];
        const u32 v = (c >= '0' && c <= '9') ? (u32)(c - '0')
                                            : (u32)(c - 'A' + 10);
        bits = (bits << 4) | (v & 0xFu);
      }
    }
    printf("  guest bits (from results.txt): 0x%08x (want 0x00001fff)\n",
           bits);
    CHECK(bits == 0x1FFFu, "guest bitmask exact-match (all ntdll checks)");

    CHECK(bits & 0x0001u, "NtCreateFile(\\??\\C:\\ntprobe.txt FILE_OPEN) -> STATUS_SUCCESS");
    CHECK(bits & 0x0002u, "NtReadFile -> exact host-written bytes, IOSB info=32");
    CHECK(bits & 0x0004u, "NtReadFile at EOF -> STATUS_END_OF_FILE");
    CHECK(bits & 0x0008u, "NtClose -> STATUS_SUCCESS");
    CHECK(bits & 0x0010u, "NtCreateFile FILE_CREATE existing -> STATUS_OBJECT_NAME_COLLISION");
    CHECK(bits & 0x0020u, "NtCreateFile FILE_OPEN missing -> STATUS_OBJECT_NAME_NOT_FOUND");
    CHECK(bits & 0x0040u, "NtCreateFile+NtWriteFile+NtReadFile round-trip");
    CHECK(bits & 0x0080u, "NtQueryInformationFile(FileStandardInformation) on Nt handle");
    CHECK(bits & 0x0100u, "NtCreateEvent (nameless) -> STATUS_SUCCESS");
    CHECK(bits & 0x0200u, "NtWaitForSingleObject 50ms unsignaled -> STATUS_TIMEOUT");
    CHECK(bits & 0x0400u, "NtSetEvent -> previous state 0, wait then succeeds");
    CHECK(bits & 0x0800u, "NtSignalAndWaitForSingleObject -> STATUS_SUCCESS");
    CHECK(bits & 0x1000u, "NtWaitForSingleObject bad handle -> STATUS_INVALID_HANDLE");

    static const char* k_lines[] = {
        "nt-create-open",   "nt-read-bytes",    "nt-read-eof",
        "nt-query-std-info", "nt-close",        "nt-create-collision",
        "nt-create-missing", "nt-write-roundtrip", "nt-create-event",
        "nt-wait-timeout",  "nt-set-event",     "nt-signal-and-wait",
        "nt-wait-bad-handle",
    };
    for (const char* name : k_lines) {
      std::string want = std::string("PASS ") + name + "\n";
      char d[128];
      snprintf(d, sizeof(d), "results.txt: PASS %s", name);
      CHECK(text.find(want) != std::string::npos, d);
    }
    CHECK(text.find("FAIL ") == std::string::npos,
          "results.txt: zero FAIL lines");
    CHECK(text.find("DONE\n") != std::string::npos,
          "results.txt: DONE written before the fail-fast");
  }

  trt_unmap(&be, img);
  free(file);

  printf(failures ? "\nRESULT: %d FAILURES\n" : "\nRESULT: ALL PASS\n",
         failures);
  return failures ? 1 : 0;
}
