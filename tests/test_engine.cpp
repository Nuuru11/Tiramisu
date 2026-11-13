// tiramisu/tests/test_engine.cpp
// The engine-API batch: memory mapping, threads, system info, wide-char,
// and the quit/focus lifecycle - the APIs a real engine needs before it
// will show a window.
//
// The guest spawns a real worker thread, waits on it, maps a file it wrote
// through the VFS, and queries the OS version. The assertions check that
// each of those reached the runtime.
#include "../src/trt/trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_user32(void);
}

namespace {

// ---- Win64 -> SysV trampolines (TEST HARNESS ONLY) ----
// The bridge is host-ABI (the console host is Win64, where no marshalling is
// needed); this Linux harness needs it. One generic marshaller plus a
// one-instruction stub per symbol: the stub loads the target into r11 and
// jumps into the marshaller, which moves rcx/rdx/r8/r9 (+ stack) into the
// SysV registers and preserves rdi/rsi (they are NONVOLATILE in Win64).
#define TRAMP_STUB(sym, target)                       \
  extern "C" void sym();                             \
  asm(".globl " #sym "\n" #sym ":\n"                \
      "  movq $" #target ", %r11\n"                 \
      "  jmp generic_w64_to_sysv\n");
extern "C" void generic_w64_to_sysv(void);
asm(
  ".text\n"
  ".globl generic_w64_to_sysv\n"
  "generic_w64_to_sysv:\n"
  "  subq $104, %rsp\n"
  "  movq %rdi, 72(%rsp)\n"
  "  movq %rsi, 80(%rsp)\n"
  "  movq %rcx, %rdi\n"        // arg1
  "  movq %rdx, %rsi\n"        // arg2
  "  movq %r8, %rdx\n"         // arg3
  "  movq %r9, %rcx\n"         // arg4
  "  movq 144(%rsp), %r8\n"     // arg5 (Win64 stack)
  "  movq 152(%rsp), %r9\n"     // arg6
  "  movq 160(%rsp), %rax\n"    // arg7 -> SysV stack slot 0
  "  movq %rax, (%rsp)\n"
  "  movq 168(%rsp), %rax\n"    // arg8 -> SysV stack slot 1
  "  movq %rax, 8(%rsp)\n"
  "  call *%r11\n"
  "  movq 72(%rsp), %rdi\n"
  "  movq 80(%rsp), %rsi\n"
  "  addq $104, %rsp\n"
  "  ret\n"
);

extern "C" {
void* trt_CreateFileMappingW(void*, void*, u32, u32, u32, const u16*);
void* trt_MapViewOfFile(void*, u32, u32, u32, u64);
int trt_UnmapViewOfFile(void*);
void* trt_CreateThread(void*, void*, void*, void*, u32, u32*);
u32 trt_WaitForSingleObject(void*, u32);
void* trt_CreateEventW(void*, u32, u32, const u16*);
int trt_SetEvent(void*);
int trt_ResetEvent(void*);
void trt_InitializeCriticalSection(void*);
void trt_EnterCriticalSection(void*);
void trt_LeaveCriticalSection(void*);
i32 trt_GetVersionExW(void*);
void trt_GetSystemInfo(void*);
i32 trt_MultiByteToWideChar(u32, u32, const char*, i32, u16*, i32);
i32 trt_WideCharToMultiByte(u32, u32, const u16*, i32, char*, i32, const char*, void*);
u32 trt_GetTempPathW(u32, u16*);
u64 trt_GetTickCount64(void);
void trt_PostQuitMessage(i32);
void* trt_SetFocus(void*);
void* trt_GetFocus(void);
int trt_GetClientRect(void*, void*);
void* trt_CreateFileW(const u16*, u32, u32, void*, u32, u32, void*);
i32 trt_WriteFile(void*, const void*, u32, u32*, void*);
i32 trt_CloseHandle(void*);
void* trt_GetStdHandle(u32);
int trt_SetStdHandle(u32, void*);
int trt_GetConsoleMode(void*, u32*);
int trt_SetConsoleCtrlHandler(void*, int);
int trt_ReadConsoleW(void*, void*, u32, u32*, void*);
int trt_WriteConsoleW(void*, const void*, u32, u32*, void*);
int trt_CreateProcessW(const u16*, u16*, void*, void*, int, u32, void*,
                       const u16*, const void*, void*);
void trt_SetLastError(u32);
u32 trt_GetLastError(void);
}

TRAMP_STUB(w_CreateFileMappingW, trt_CreateFileMappingW)
TRAMP_STUB(w_MapViewOfFile, trt_MapViewOfFile)
TRAMP_STUB(w_UnmapViewOfFile, trt_UnmapViewOfFile)
TRAMP_STUB(w_CreateThread, trt_CreateThread)
TRAMP_STUB(w_WaitForSingleObject, trt_WaitForSingleObject)
TRAMP_STUB(w_CreateEventW, trt_CreateEventW)
TRAMP_STUB(w_SetEvent, trt_SetEvent)
TRAMP_STUB(w_ResetEvent, trt_ResetEvent)
TRAMP_STUB(w_InitializeCriticalSection, trt_InitializeCriticalSection)
TRAMP_STUB(w_EnterCriticalSection, trt_EnterCriticalSection)
TRAMP_STUB(w_LeaveCriticalSection, trt_LeaveCriticalSection)
TRAMP_STUB(w_GetVersionExW, trt_GetVersionExW)
TRAMP_STUB(w_GetSystemInfo, trt_GetSystemInfo)
TRAMP_STUB(w_MultiByteToWideChar, trt_MultiByteToWideChar)
TRAMP_STUB(w_WideCharToMultiByte, trt_WideCharToMultiByte)
TRAMP_STUB(w_GetTempPathW, trt_GetTempPathW)
TRAMP_STUB(w_GetTickCount64, trt_GetTickCount64)
TRAMP_STUB(w_PostQuitMessage, trt_PostQuitMessage)
TRAMP_STUB(w_SetFocus, trt_SetFocus)
TRAMP_STUB(w_GetFocus, trt_GetFocus)
TRAMP_STUB(w_GetClientRect, trt_GetClientRect)
TRAMP_STUB(w_CreateFileW, trt_CreateFileW)
TRAMP_STUB(w_WriteFile, trt_WriteFile)
TRAMP_STUB(w_CloseHandle, trt_CloseHandle)


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
void linux_release(void* base, size_t bytes) {
  for (int i = 0; i < g_extent_count; ++i)
    if (g_extents[i].base == base) {
      munmap(base, g_extents[i].len);
      g_extents[i].base = nullptr;
      return;
    }
  (void)bytes;
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

} // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    printf("usage: test_engine <guest.exe> <drive_c_dir>\n");
    return 2;
  }
  size_t size = 0;
  u8* file = read_file(argv[1], &size);
  if (!file) { printf("cannot read %s\n", argv[1]); return 2; }

  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &linux_log,
                 nullptr};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();

  // the guest calls these through the trampolines (harness only)
  {
    static const trt_named_fn k_k32[] = {
        {"CreateFileMappingW", (void*)&w_CreateFileMappingW},
        {"MapViewOfFile", (void*)&w_MapViewOfFile},
        {"UnmapViewOfFile", (void*)&w_UnmapViewOfFile},
        {"CreateThread", (void*)&w_CreateThread},
        {"WaitForSingleObject", (void*)&w_WaitForSingleObject},
        {"CreateEventW", (void*)&w_CreateEventW},
        {"SetEvent", (void*)&w_SetEvent},
        {"ResetEvent", (void*)&w_ResetEvent},
        {"InitializeCriticalSection", (void*)&w_InitializeCriticalSection},
        {"EnterCriticalSection", (void*)&w_EnterCriticalSection},
        {"LeaveCriticalSection", (void*)&w_LeaveCriticalSection},
        {"GetVersionExW", (void*)&w_GetVersionExW},
        {"GetSystemInfo", (void*)&w_GetSystemInfo},
        {"MultiByteToWideChar", (void*)&w_MultiByteToWideChar},
        {"WideCharToMultiByte", (void*)&w_WideCharToMultiByte},
        {"GetTempPathW", (void*)&w_GetTempPathW},
        {"GetTickCount64", (void*)&w_GetTickCount64},
        {"CreateFileW", (void*)&w_CreateFileW},
        {"WriteFile", (void*)&w_WriteFile},
        {"CloseHandle", (void*)&w_CloseHandle},
        {"GetStdHandle", (void*)&trt_GetStdHandle},
        {"SetStdHandle", (void*)&trt_SetStdHandle},
        {"GetConsoleMode", (void*)&trt_GetConsoleMode},
        {"SetConsoleCtrlHandler", (void*)&trt_SetConsoleCtrlHandler},
        {"ReadConsoleW", (void*)&trt_ReadConsoleW},
        {"WriteConsoleW", (void*)&trt_WriteConsoleW},
        {"CreateProcessW", (void*)&trt_CreateProcessW},
        {"SetLastError", (void*)&trt_SetLastError},
        {"GetLastError", (void*)&trt_GetLastError},
    };
    static const trt_named_fn k_u32[] = {
        {"PostQuitMessage", (void*)&w_PostQuitMessage},
        {"SetFocus", (void*)&w_SetFocus},
        {"GetFocus", (void*)&w_GetFocus},
        {"GetClientRect", (void*)&w_GetClientRect},
    };
    trt_module_register("kernel32.dll", TRT_ROUTE_BUILTIN, k_k32,
                         (int)(sizeof(k_k32) / sizeof(k_k32[0])));
    trt_module_register("user32.dll", TRT_ROUTE_BUILTIN, k_u32,
                         (int)(sizeof(k_u32) / sizeof(k_u32[0])));
  }

  // the guest's C: drive (its asset files live there)
  trt_vfs_mount("c:", argv[2]);

  printf("tiramisu engine-API batch test\n");

  trt_resolver rs;
  trt_resolver_default(&rs);
  trt_image* img = nullptr;
  const int rc = trt_map_pe(&be, &rs, file, size, &img);
  CHECK(rc == TRT_OK, "guest PE maps (engine APIs bound)");
  if (rc != TRT_OK || !img) {
    printf("  map failed: %s (%s)\n", trt_errstr(rc),
           img ? img->last_detail : "no image");
    free(file);
    return 1;
  }
  CHECK(trt_seal_image(&be, img) == TRT_OK, "image sealed (W^X)");

  const u64 ret = win64_call(img->base + img->entry_rva, 0, 0, 0);
  printf("  guest returned 0x%llx (want 0x3f1fff)\n", (unsigned long long)ret);

  CHECK(ret & 0x1u, "GetVersionExW answered");
  CHECK(ret & 0x2u, "GetSystemInfo answered");
  CHECK(ret & 0x4u, "MultiByteToWideChar converted");
  CHECK(ret & 0x8u, "WideCharToMultiByte converted");
  CHECK(ret & 0x10u, "GetTempPathW answered");
  CHECK(ret & 0x20u, "CreateFileMapping/MapViewOfFile carried the file's bytes");
  CHECK(ret & 0x40u, "CreateThread started a real worker thread");
  CHECK(ret & 0x80u, "the worker ran and WaitForSingleObject returned");
  CHECK(ret & 0x100u, "SetEvent/ResetEvent worked");
  CHECK(ret & 0x200u, "focus lifecycle (SetFocus/GetFocus)");
  CHECK(ret & 0x400u, "PostQuitMessage accepted");
  CHECK(ret & 0x800u, "GetClientRect answered");
  CHECK(ret & 0x1000u, "GetTickCount64 moves");
  // console/child-process honesty (OrTMM's kernel32 imports)
  CHECK(ret & 0x2000u, "CreateProcessW -> FALSE (console blocks children)");
  CHECK(ret & 0x4000u, "CreateProcessW names ERROR_ACCESS_DENIED");
  CHECK(ret & 0x8000u, "CreateProcessW fills no fake PROCESS_INFORMATION");
  CHECK(ret & 0x10000u, "GetStdHandle -> NULL + ERROR_INVALID_HANDLE");
  CHECK(ret & 0x20000u, "SetStdHandle -> FALSE");
  CHECK(ret & 0x40000u, "GetConsoleMode -> FALSE (no console)");
  CHECK(ret & 0x80000u, "SetConsoleCtrlHandler -> FALSE");
  CHECK(ret & 0x100000u, "ReadConsoleW -> FALSE (no console)");
  CHECK(ret & 0x200000u, "WriteConsoleW -> FALSE (no console)");

  printf("\nRESULT: %s\n", failures ? "FAILURES" : "ALL PASS");
  return failures ? 1 : 0;
}
