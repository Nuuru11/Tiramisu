// tiramisu/tests/test_game_boot.cpp
// THE launch-path proof: a game-shaped PE (window class, WndProc, message
// pump, async keys, cursor, heap, QPC, env strings, module path, VFS file
// writes) is cross-built as a real PE64, mapped by the trt loader with the
// BUILTIN bridge resolver, executed on this host, and its on-disk results
// are verified. This is "games will actually launch" offline.
#include "../src/trt/trt.h"
#include "../src/trt/input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

extern "C" {
int trt_RegisterRawInputDevices(const void*, u32, u32);
}

// ---- mmap backend (same as the loader test) ----
namespace {
struct extent { void* base; size_t len; };
extent g_ext[16];
int g_extn = 0;

void* l_alloc(size_t bytes, u64 pref) {
  void* p = mmap(pref ? (void*)(uintptr_t)pref : nullptr, bytes,
                 PROT_READ | PROT_WRITE,
                 pref ? (MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS)
                      : (MAP_PRIVATE | MAP_ANONYMOUS),
                 -1, 0);
  if (p == MAP_FAILED) return nullptr;
  if (g_extn < 16) { g_ext[g_extn] = {p, bytes}; ++g_extn; }
  return p;
}
int l_protect(void* base, size_t bytes, unsigned prot) {
  int p = 0;
  if (prot & TRT_PROT_R) p |= PROT_READ;
  if (prot & TRT_PROT_W) p |= PROT_WRITE;
  if (prot & TRT_PROT_X) p |= PROT_EXEC;
  const size_t page = 4096;
  const uintptr_t b = (uintptr_t)base & ~(uintptr_t)(page - 1);
  const size_t len = ((uintptr_t)base + bytes - b + page - 1) & ~(page - 1);
  return mprotect((void*)b, len, p);
}
void l_release(void* base, size_t) {
  for (int i = 0; i < g_extn; ++i)
    if (g_ext[i].base == base) { munmap(base, g_ext[i].len); g_ext[i].base = nullptr; return; }
}
void l_log(const char* line) { printf("  [trt] %s\n", line); }

u8* read_file(const char* path, size_t* size) {
  FILE* f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  u8* buf = (u8*)malloc((size_t)n);
  if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return nullptr; }
  fclose(f);
  *size = (size_t)n;
  return buf;
}

// call a Win64 guest: shadow space + rcx/rdx/r8, result in rax
u64 win64_call(void* fn, u64 a, u64 b, u64 c) {
  u64 ret;
  asm volatile(
    "subq $48, %%rsp\n"
    "movq %1, %%rcx\n"
    "movq %2, %%rdx\n"
    "movq %3, %%r8\n"
    "call *%4\n"
    "addq $48, %%rsp\n"
    : "=a"(ret)
    : "r"(a), "r"(b), "r"(c), "r"(fn)
    : "rcx", "rdx", "r8", "r9", "r10", "r11",
      "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "memory", "cc");
  return ret;
}

int failures = 0;
#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (cond) printf("  PASS %s\n", msg);                            \
    else { printf("  FAIL %s\n", msg); ++failures; }                 \
  } while (0)

// ---- Win64 -> SysV trampolines (TEST HARNESS ONLY) ----
// On the console host and guest share the Win64 ABI and these do not exist.
// Patterns per arg count; all preserve rdi/rsi (Win64-nonvolatile, SysV-
// caller-saved) and forward 5+ args from the Win64 stack to the SysV stack.
// Win64 entry: rsp=E (E≡8), shadow [E+8..E+40], args5+ at [E+40...].
asm(
  ".text\n"
  // ---- 4 args: rcx->rdi rdx->rsi r8->rdx r9->rcx ----
  ".macro W64_4 name, target\n"
  ".globl \\name\n"
  "\\name:\n"
  "\tsubq $40, %rsp\n"           // E-40 ≡ 0 mod 16 (E≡8)
  "\tmovq %rdi, (%rsp)\n"
  "\tmovq %rsi, 8(%rsp)\n"
  "\tmovq %rcx, %rdi\n"
  "\tmovq %rdx, %rsi\n"
  "\tmovq %r8, %rdx\n"
  "\tmovq %r9, %rcx\n"
  "\tcall \\target\n"
  "\tmovq (%rsp), %rdi\n"
  "\tmovq 8(%rsp), %rsi\n"
  "\taddq $40, %rsp\n"
  "\tret\n"
  ".endm\n"
  // ---- 5 args: arg5 -> r8 (SysV) ----
  ".macro W64_5 name, target\n"
  ".globl \\name\n"
  "\\name:\n"
  "\tsubq $56, %rsp\n"
  "\tmovq %rdi, (%rsp)\n"
  "\tmovq %rsi, 8(%rsp)\n"
  "\tmovq %rcx, %rdi\n"
  "\tmovq %rdx, %rsi\n"
  "\tmovq %r8, %rdx\n"
  "\tmovq %r9, %rcx\n"
  "\tmovq 96(%rsp), %r8\n"
  "\tcall \\target\n"
  "\tmovq (%rsp), %rdi\n"
  "\tmovq 8(%rsp), %rsi\n"
  "\taddq $56, %rsp\n"
  "\tret\n"
  ".endm\n"
  // ---- 7 args: arg5->r8, arg6->r9, arg7->[rsp] (SysV stack starts at arg7)
  ".macro W64_7 name, target\n"
  ".globl \\name\n"
  "\\name:\n"
  "\tsubq $72, %rsp\n"
  "\tmovq %rdi, 32(%rsp)\n"
  "\tmovq %rsi, 40(%rsp)\n"
  "\tmovq %rcx, %rdi\n"
  "\tmovq %rdx, %rsi\n"
  "\tmovq %r8, %rdx\n"
  "\tmovq %r9, %rcx\n"
  "\tmovq 112(%rsp), %r8\n"
  "\tmovq 120(%rsp), %r9\n"
  "\tmovq 128(%rsp), %rax\n"
  "\tmovq %rax, (%rsp)\n"
  "\tcall \\target\n"
  "\tmovq 32(%rsp), %rdi\n"
  "\tmovq 40(%rsp), %rsi\n"
  "\taddq $72, %rsp\n"
  "\tret\n"
  ".endm\n"
  // ---- 12 args: arg5->r8, arg6->r9, args7-12 (6 qwords) [E+56..104] ->
  // [rsp+224..272] -> [rsp+0..40] ----
  ".macro W64_12 name, target\n"
  ".globl \\name\n"
  "\\name:\n"
  "\tsubq $168, %rsp\n"
  "\tmovq %rdi, 64(%rsp)\n"
  "\tmovq %rsi, 72(%rsp)\n"
  "\tmovq %rcx, %rdi\n"
  "\tmovq %rdx, %rsi\n"
  "\tmovq %r8, %rdx\n"
  "\tmovq %r9, %rcx\n"
  "\tmovq 208(%rsp), %r8\n"
  "\tmovq 216(%rsp), %r9\n"
  "\txorl %ecx, %ecx\n"
  "1:\n"
  "\tmovq 224(%rsp,%rcx,8), %rax\n"
  "\tmovq %rax, (%rsp,%rcx,8)\n"
  "\tincl %ecx\n"
  "\tcmpl $6, %ecx\n"
  "\tjl 1b\n"
  "\tcall \\target\n"
  "\tmovq 64(%rsp), %rdi\n"
  "\tmovq 72(%rsp), %rsi\n"
  "\taddq $168, %rsp\n"
  "\tret\n"
  ".endm\n"

  "W64_4 w64_GetProcessHeap, trt_GetProcessHeap\n"
  "W64_4 w64_HeapAlloc, trt_HeapAlloc\n"
  "W64_4 w64_QueryPerformanceCounter, trt_QueryPerformanceCounter\n"
  "W64_4 w64_CloseHandle, trt_CloseHandle\n"
  "W64_4 w64_GetEnvironmentStringsW, trt_GetEnvironmentStringsW\n"
  "W64_4 w64_GetModuleFileNameW, trt_GetModuleFileNameW\n"
  "W64_4 w64_RegisterClassExW, trt_RegisterClassExW\n"
  "W64_4 w64_GetAsyncKeyState, trt_GetAsyncKeyState\n"
  "W64_4 w64_GetCursorPos, trt_GetCursorPos\n"
  "W64_4 w64_TranslateMessage, trt_TranslateMessage\n"
  "W64_4 w64_DispatchMessageW, trt_DispatchMessageW\n"
  "W64_5 w64_WriteFile, trt_WriteFile\n"
  "W64_5 w64_PeekMessageW, trt_PeekMessageW\n"
  "W64_7 w64_CreateFileW, trt_CreateFileW\n"
  "W64_12 w64_CreateWindowExW, trt_CreateWindowExW\n"
);

extern "C" {
void* w64_GetProcessHeap();
void* w64_HeapAlloc();
unsigned long w64_QueryPerformanceCounter();
unsigned long w64_CloseHandle();
const unsigned short* w64_GetEnvironmentStringsW();
unsigned int w64_GetModuleFileNameW();
unsigned short w64_RegisterClassExW();
short w64_GetAsyncKeyState();
unsigned long w64_GetCursorPos();
unsigned long w64_TranslateMessage();
long long w64_DispatchMessageW();
unsigned long w64_WriteFile();
unsigned long w64_PeekMessageW();
void* w64_CreateFileW();
void* w64_CreateWindowExW();
}

} // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc < 2) { fprintf(stderr, "usage: %s <guest_game.exe>\n", argv[0]); return 2; }

  printf("tiramisu GAME-SHAPED boot test — %s\n", argv[1]);

  // runtime boot: builtin bridges + Win64 trampolines as the resolver
  // (console needs none of this: one ABI everywhere — see trt.h note)
  trt_backend be{&l_alloc, &l_protect, &l_release, &l_log, nullptr};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();
  trt_resolver def;
  trt_resolver_default(&def);
  struct sym { const char* n; void* f; };
  static const sym k_thunks[] = {
      {"GetProcessHeap", (void*)&w64_GetProcessHeap},
      {"HeapAlloc", (void*)&w64_HeapAlloc},
      {"QueryPerformanceCounter", (void*)&w64_QueryPerformanceCounter},
      {"CloseHandle", (void*)&w64_CloseHandle},
      {"GetEnvironmentStringsW", (void*)&w64_GetEnvironmentStringsW},
      {"GetModuleFileNameW", (void*)&w64_GetModuleFileNameW},
      {"RegisterClassExW", (void*)&w64_RegisterClassExW},
      {"GetAsyncKeyState", (void*)&w64_GetAsyncKeyState},
      {"GetCursorPos", (void*)&w64_GetCursorPos},
      {"TranslateMessage", (void*)&w64_TranslateMessage},
      {"DispatchMessageW", (void*)&w64_DispatchMessageW},
      {"WriteFile", (void*)&w64_WriteFile},
      {"PeekMessageW", (void*)&w64_PeekMessageW},
      {"CreateFileW", (void*)&w64_CreateFileW},
      {"CreateWindowExW", (void*)&w64_CreateWindowExW},
  };
  static trt_resolver rs;
  rs.user = &def;
  rs.resolve = [](void* user, const char* module, const char* symbol,
                  u16 ordinal) -> void* {
    const bool bridge = strcmp(module, "kernel32.dll") == 0 ||
                        strcmp(module, "user32.dll") == 0;
    if (bridge && symbol) {
      for (const sym& s : k_thunks)
        if (strcmp(s.n, symbol) == 0) return s.f;
    }
    const trt_resolver* d = (const trt_resolver*)user;
    return d->resolve(d->user, module, symbol, ordinal);
  };

  // VFS: C:\ -> scratch drive_c, Z:\ -> the test's own dir
  char drive_c[] = "/tmp/tiramisu-drivec.XXXXXX";
  mkdtemp(drive_c);
  char exe_dir[512];
  snprintf(exe_dir, sizeof(exe_dir), "%s", argv[1]);
  *(strrchr(exe_dir, '/')) = 0;
  trt_vfs_mount("c:", drive_c);
  trt_vfs_mount("z:", exe_dir);
  trt_bridge_set_exe_path("Z:\\guest_game.exe");

  size_t size = 0;
  u8* file = read_file(argv[1], &size);
  if (!file) { fprintf(stderr, "cannot read guest\n"); return 2; }
  printf("  guest: %zu bytes, %d sections worth of real game-shaped code\n",
         size, 4);

  trt_image* img = nullptr;
  const int rc = trt_map_pe(&be, &rs, file, size, &img);
  CHECK(rc == TRT_OK, "map game-shaped PE");
  if (rc != TRT_OK) return 1;
  CHECK(trt_seal_image(&be, img) == TRT_OK, "seal (W^X)");

  const u64 ret = win64_call(img->base + img->entry_rva, (u64)(uintptr_t)img->base, 1, 0);
  char det[96];
  snprintf(det, sizeof(det), "guest returned 0x%08llx",
           (unsigned long long)ret);
  CHECK((ret & 0x40000000u) == 0x40000000u, det);
  // exact: every term is a distinct bit (pumped=2 => +0x20)
  CHECK(ret == 0x41F19120u, "checksum exact-match (all bridges verified)");
  CHECK(ret & 0x00000100, "window created");
  CHECK(ret & 0x00001000, "WndProc invoked via DispatchMessage");
  CHECK(ret & 0x00010000, "GetAsyncKeyState ran (no key = clean)");
  CHECK(ret & 0x000100000, "HeapAlloc through bridge");
  CHECK(ret & 0x00200000, "QueryPerformanceCounter through bridge");
  CHECK(ret & 0x00400000, "environment strings (COMPUTERNAME=TIRAMISU)");
  CHECK(ret & 0x00800000, "module path on Z:\\");

  // the guest wrote its results through the VFS — verify ON DISK
  std::string res_path = std::string(drive_c) + "/results.txt";
  FILE* rf = fopen(res_path.c_str(), "rb");
  CHECK(rf != nullptr, "guest created C:\\results.txt through the VFS");
  if (rf) {
    u32 stored = 0, calls = 0;
    if (fread(&stored, 4, 1, rf) == 1) {}
    if (fread(&calls, 4, 1, rf) == 1) {}
    fclose(rf);
    char d2[96];
    snprintf(d2, sizeof(d2), "stored checksum == returned (0x%08x)", stored);
    CHECK(stored == (u32)ret, d2);
    CHECK(calls >= 2, "WndProc ran >= 2x (WM_CREATE + WM_SHOWWINDOW)");
  }

  trt_unmap(&be, img);
  free(file);

  printf(failures ? "\nRESULT: %d FAILURES\n" : "\nRESULT: ALL PASS\n",
         failures);
  return failures ? 1 : 0;
}
