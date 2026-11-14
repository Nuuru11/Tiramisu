// tiramisu/tests/test_engine.cpp
// The engine-API batch: memory mapping, threads, system info, wide-char,
// and the quit/focus lifecycle - the APIs a real engine needs before it
// will show a window.
//
// The guest spawns a real worker thread, waits on it, maps a file it wrote
// through the VFS, and queries the OS version. The assertions check that
// each of those reached the runtime.
#include "../src/trt/trt.h"
#include "../src/trt/images.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_user32(void);
}

namespace {

// ---- bridge entry points (STAGE 6b) ----
// The bridges are TRT_STDCALL = ms_abi (Win64) on the Linux test host — the
// same ABI the guest uses — so the guest IAT binds the bridge addresses
// DIRECTLY, exactly like the console (which has zero trampolines). The old
// hand-written Win64->SysV asm trampolines are gone.
extern "C" {
void* TRT_STDCALL trt_CreateFileMappingW(void*, void*, u32, u32, u32, const u16*);
void* TRT_STDCALL trt_MapViewOfFile(void*, u32, u32, u32, u64);
int TRT_STDCALL trt_UnmapViewOfFile(void*);
u64 TRT_STDCALL trt_CreateThread(void*, void*, void*, void*, u32, u32*);
u32 TRT_STDCALL trt_WaitForSingleObject(u64, u32);
void* TRT_STDCALL trt_CreateEventW(void*, u32, u32, const u16*);
int TRT_STDCALL trt_SetEvent(u64);
int TRT_STDCALL trt_ResetEvent(u64);
int TRT_STDCALL trt_InitializeCriticalSection(void*);
int TRT_STDCALL trt_EnterCriticalSection(void*);
int TRT_STDCALL trt_LeaveCriticalSection(void*);
u32 TRT_STDCALL trt_GetVersionExW(void*);
void TRT_STDCALL trt_GetSystemInfo(void*);
int TRT_STDCALL trt_MultiByteToWideChar(u32, u32, const char*, i32, u16*, i32);
int TRT_STDCALL trt_WideCharToMultiByte(u32, u32, const u16*, i32, char*, i32, const char*, void*);
u32 TRT_STDCALL trt_GetTempPathW(u32, u16*);
int TRT_STDCALL trt_CreateDirectoryW(const u16*, void*);
u64 TRT_STDCALL trt_GetTickCount64(void);
void TRT_STDCALL trt_PostQuitMessage(i32);
void* TRT_STDCALL trt_SetFocus(void*);
void* TRT_STDCALL trt_GetFocus(void);
int TRT_STDCALL trt_GetClientRect(void*, void*);
void* TRT_STDCALL trt_CreateFileW(const u16*, u32, u32, void*, u32, u32, void*);
int TRT_STDCALL trt_WriteFile(void*, const void*, u32, u32*, void*);
int TRT_STDCALL trt_CloseHandle(void*);
void* TRT_GUEST_ABI trt_GetStdHandle(u32);
int TRT_GUEST_ABI trt_SetStdHandle(u32, void*);
int TRT_GUEST_ABI trt_GetConsoleMode(void*, u32*);
int TRT_GUEST_ABI trt_SetConsoleCtrlHandler(void*, int);
int TRT_GUEST_ABI trt_ReadConsoleW(void*, void*, u32, u32*, void*);
int TRT_GUEST_ABI trt_WriteConsoleW(void*, const void*, u32, u32*, void*);
int TRT_GUEST_ABI trt_CreateProcessW(const u16*, u16*, void*, void*, int, u32, void*,
                        const u16*, const void*, void*);
void TRT_STDCALL trt_SetLastError(u32);
u32 TRT_STDCALL trt_GetLastError(void);
u32 TRT_STDCALL trt_IsProcessorFeaturePresent(u32);
int TRT_STDCALL trt_GetCPInfo(u32, void*);
void* TRT_STDCALL trt_FindFirstFileExW(const u16*, u32, void*, u32, void*, u32);
int TRT_STDCALL trt_GetFileAttributesExW(const u16*, u32, void*);
int TRT_STDCALL trt_FindNextFileW(void*, void*);
int TRT_STDCALL trt_FindClose(void*);
void* TRT_STDCALL trt_LoadLibraryW(const u16*);
void* TRT_STDCALL trt_GetProcAddress(void*, const char*);
u32 TRT_STDCALL trt_FlsAlloc(void*);
void* TRT_STDCALL trt_FlsGetValue(u32);
void* TRT_STDCALL trt_FlsGetValue2(u32);
int TRT_STDCALL trt_FlsSetValue(u32, void*);
}


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

// Linux-host stand-in for RtlAddFunctionTable (mirrors test_corpus_run.cpp).
// The console host registers .pdata with the real RtlAddFunctionTable; the
// trt_RtlLookupFunctionEntry bridge scans img->pdata directly, so a no-op
// success here keeps the backend contract honest without a libgcc unwind
// dependency in this test.
static int linux_register_unwind(void* image_base, const void* pdata,
                                 u32 count) {
  (void)image_base; (void)pdata; (void)count;
  return 0;
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

  // STAGE 7: the SEH/unwind guest (argv[3], optional) exercises the
  // unwind-edit surface OrTMM's CRT startup runs before WinMain
  u8* seh_file = nullptr;
  size_t seh_size = 0;
  if (argc >= 4) seh_file = read_file(argv[3], &seh_size);

  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &linux_log,
                 &linux_register_unwind};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();

  // the guest binds the bridge addresses DIRECTLY (ms_abi = the guest's
  // own ABI — exactly like the console IAT, zero trampolines)
  {
    static const trt_named_fn k_k32[] = {
        {"CreateFileMappingW", (void*)&trt_CreateFileMappingW},
        {"MapViewOfFile", (void*)&trt_MapViewOfFile},
        {"UnmapViewOfFile", (void*)&trt_UnmapViewOfFile},
        {"CreateThread", (void*)&trt_CreateThread},
        {"WaitForSingleObject", (void*)&trt_WaitForSingleObject},
        {"CreateEventW", (void*)&trt_CreateEventW},
        {"SetEvent", (void*)&trt_SetEvent},
        {"ResetEvent", (void*)&trt_ResetEvent},
        {"InitializeCriticalSection", (void*)&trt_InitializeCriticalSection},
        {"EnterCriticalSection", (void*)&trt_EnterCriticalSection},
        {"LeaveCriticalSection", (void*)&trt_LeaveCriticalSection},
        {"GetVersionExW", (void*)&trt_GetVersionExW},
        {"GetSystemInfo", (void*)&trt_GetSystemInfo},
        {"MultiByteToWideChar", (void*)&trt_MultiByteToWideChar},
        {"WideCharToMultiByte", (void*)&trt_WideCharToMultiByte},
        {"GetTempPathW", (void*)&trt_GetTempPathW},
        {"GetTickCount64", (void*)&trt_GetTickCount64},
        {"CreateFileW", (void*)&trt_CreateFileW},
        {"WriteFile", (void*)&trt_WriteFile},
        {"CloseHandle", (void*)&trt_CloseHandle},
        {"GetStdHandle", (void*)&trt_GetStdHandle},
        {"SetStdHandle", (void*)&trt_SetStdHandle},
        {"GetConsoleMode", (void*)&trt_GetConsoleMode},
        {"SetConsoleCtrlHandler", (void*)&trt_SetConsoleCtrlHandler},
        {"ReadConsoleW", (void*)&trt_ReadConsoleW},
        {"WriteConsoleW", (void*)&trt_WriteConsoleW},
        {"CreateProcessW", (void*)&trt_CreateProcessW},
        {"SetLastError", (void*)&trt_SetLastError},
        {"GetLastError", (void*)&trt_GetLastError},
        {"IsProcessorFeaturePresent", (void*)&trt_IsProcessorFeaturePresent},
        {"FindFirstFileExW", (void*)&trt_FindFirstFileExW},
        {"FindNextFileW", (void*)&trt_FindNextFileW},
        {"FindClose", (void*)&trt_FindClose},
    };
    static const trt_named_fn k_u32[] = {
        {"PostQuitMessage", (void*)&trt_PostQuitMessage},
        {"SetFocus", (void*)&trt_SetFocus},
        {"GetFocus", (void*)&trt_GetFocus},
        {"GetClientRect", (void*)&trt_GetClientRect},
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
  // STAGE 7 (OrTMM): the CRT startup gate. PF_FASTFAIL_AVAILABLE (0x17)
  // MUST answer present — a 0 sent _set_Fflag to __fastfail and the EXE
  // died with int 0x29 before WinMain (measured at OrTMM+0xd1d4).
  CHECK(win64_call((void*)&trt_IsProcessorFeaturePresent, 0x17, 0, 0) == 1,
        "IsProcessorFeaturePresent(PF_FASTFAIL_AVAILABLE) -> 1 (CRT startup gate)");
  CHECK(win64_call((void*)&trt_IsProcessorFeaturePresent, 0, 0, 0) == 1,
        "IsProcessorFeaturePresent(PF_COMPARABLE_EXCEPT) -> 1");
  CHECK(win64_call((void*)&trt_IsProcessorFeaturePresent, 0x30, 0, 0) == 0,
        "IsProcessorFeaturePresent(unknown) -> 0 (honest no)");

  // ---- STAGE 7: GetCPInfo writes EXACTLY sizeof(CPINFO)=18 bytes ----------
  // MAX_LEADBYTES is 12 (winnls.h), not 126. A 132-byte memset here zeroed
  // the MSVC /GS cookie the CRT keeps 0x18 bytes past its frame-local
  // CPINFO — every corpus DLL + OrTMM died in __raise_securityfailure
  // (int 0x29) before WinMain/DllMain finished (pinned under gdb).
  {
    u8 guard_buf[64];
    memset(guard_buf, 0xA5, sizeof(guard_buf));
    // Win64 ABI: rcx=codepage, rdx=cpinfo buffer
    win64_call((void*)&trt_GetCPInfo, 1252, (u64)(uintptr_t)guard_buf, 0);
    int ok = 1;
    for (size_t i = 20; i < sizeof(guard_buf); ++i)
      if (guard_buf[i] != 0xA5) ok = 0;  // anything past 20 = stack overwrite
    CHECK(ok, "GetCPInfo(1252) wrote only sizeof(CPINFO)=20 (no /GS cookie kill)");
    CHECK(((u32*)guard_buf)[0] == 1, "GetCPInfo(1252) MaxCharSize=1 (SBCS)");
  }

  // ---- STAGE 7: FLS surface roundtrip incl. FlsGetValue2 ------------------
  // OrTMM's UCRT resolves FlsGetValue2 BY NAME via GetProcAddress; before the
  // bridge export existed the blanket-0 stub answered NULL forever and the CRT
  // locale/FLS init storm-allocated 968 bytes x716k (2.9GB, watchdog exit 43).
  {
    const u64 fls_idx = win64_call((void*)&trt_FlsAlloc, 0, 0, 0);
    CHECK(fls_idx != 0xFFFFFFFFull, "FlsAlloc returned a valid index");
    const u64 cookie = 0xC0FFEE123456789ull;
    CHECK(win64_call((void*)&trt_FlsSetValue, fls_idx, cookie, 0) == 1,
          "FlsSetValue(idx,cookie) -> nonzero");
    CHECK(win64_call((void*)&trt_FlsGetValue, fls_idx, 0, 0) == cookie,
          "FlsGetValue(idx) == cookie (roundtrip)");
    CHECK(win64_call((void*)&trt_FlsGetValue2, fls_idx, 0, 0) == cookie,
          "FlsGetValue2(idx) == cookie (the OrTMM storm fix)");
    CHECK(win64_call((void*)&trt_FlsGetValue2, 0xFFFFFFFFull, 0, 0) == 0,
          "FlsGetValue2(invalid) -> NULL");
    CHECK(win64_call((void*)&trt_FlsSetValue, 0xFFFFFFFFull, 0, 0) == 0,
          "FlsSetValue(invalid) -> 0");
  }

  // ---- STAGE 7: relative guest paths resolve against the guest cwd drive -
  // PyInstaller extracts its archive via RELATIVE paths (".\_MEIxxxx\entry").
  // The resolver only understood "X:" drives -> every relative CreateFileW
  // failed PATH_NOT_FOUND and extraction aborted ("Failed to extract
  // NoThumbnail.png"). Here only "c:" is mounted, so it becomes the cwd
  // drive; ".\rel.txt" must land under the mount target, not the host cwd.
  {
    char host[1024];
    const u16 dot_rel[] = {'.','\\','r','e','l','.','t','x','t',0};
    CHECK(trt_vfs_resolve_utf16(dot_rel, host, sizeof(host)) == 0,
          "resolve(.\\rel.txt) hits the guest cwd drive");
    CHECK(strstr(host, "/rel.txt") && strstr(host, "drive_c"),
          "resolve(.\\rel.txt) lands under the c: mount (not host cwd)");
    const u16 bare_rel[] = {'s','u','b','\\','f',0};
    CHECK(trt_vfs_resolve_utf16(bare_rel, host, sizeof(host)) == 0,
          "resolve(sub\\f) also resolves relative");
    const u16 drive_rel[] = {'c',':','\\','x',0};
    CHECK(trt_vfs_resolve_utf16(drive_rel, host, sizeof(host)) == 0,
          "resolve(c:\\x) still hits the drive mount");
    const u16 unmounted[] = {'q',':','\\','x',0};
    CHECK(trt_vfs_resolve_utf16(unmounted, host, sizeof(host)) != 0,
          "resolve(q:\\x) fails (unmounted drive, fail-visible)");
  }

  // ---- STAGE 7: GetTempPathW returns a GUEST path; CreateDirectoryW digs --
  // GetTempPathW used to return the HOST path ("/tmp/"): PyInstaller built
  // "/tmp\_MEIxxxx", the resolver mis-read host-absolute paths as relative
  // guest paths, the temp dir never materialised and the candidate probe
  // looped 2.5M opens (watchdog exit 43). The bridge must answer the guest
  // naming world. CreateDirectoryW must create missing parents because the
  // drive_c mount starts empty ("C:\Temp\_MEIxxxx" needs C:\Temp first).
  {
    u16 tbuf[64]; const u32 n =
      (u32)win64_call((void*)&trt_GetTempPathW, 64, (u64)(uintptr_t)tbuf, 0);
    CHECK(n > 1 && n <= 64, "GetTempPathW answers with a length");
    char t8[128];
    for (u32 i = 0; i < n && i < 63; ++i) t8[i] = (char)tbuf[i];
    t8[n && n <= 63 ? n : 63] = 0;
    CHECK(t8[1] == ':' && t8[2] == '\\',
          "GetTempPathW returns a GUEST drive path (C:\\Temp\\), not /tmp/");
    // CreateDirectoryW with parents: real dirs must appear under the mount
    const u16 mk_path[] = {'c',':','\\','T','e','m','p','\\','_','M','E','I','t',
                           0};
    CHECK(win64_call((void*)&trt_CreateDirectoryW, (u64)(uintptr_t)mk_path, 0,
                     0) == 1,
          "CreateDirectoryW(C:\\Temp\\_MEIt) -> nonzero");
    struct stat st;
    char want[1024];
    snprintf(want, sizeof(want), "%s/Temp/_MEIt", argv[2]);
    CHECK(stat(want, &st) == 0 && (st.st_mode & S_IFDIR),
          "C:\\Temp\\_MEIt materialised as real dirs under the c: mount");
  }

  // ---- STAGE 7: LoadLibraryW bare-name miss falls back to system modules --
  // dstoragecore's obfuscation runtime calls LoadLibraryExW("advapi32");
  // trt_LoadLibraryW resolves the bare name to <gamedir>/advapi32 (which does
  // not exist) and used to return NULL without trying the synthetic system
  // module table — the runtime then aborted via int 0x29(ec=7).
  {
    // UTF-16 by hand: clang on Linux makes L"" literals UTF-32 (4 bytes per
    // unit) — a literal here would hand the bridge a 'a',0x0,... string
    const u16 advapi32_w[] = {'a','d','v','a','p','i','3','2',0};
    const int ll_ok = (int)win64_call((void*)&trt_LoadLibraryW,
                                      (u64)(uintptr_t)advapi32_w, 0, 0);
    CHECK(ll_ok != 0, "LoadLibraryW(\"advapi32\") -> non-NULL (system-module fallback)");
    // and the obfuscation runtime's actual target resolves by name
    const u64 gpa = win64_call((void*)&trt_GetProcAddress, ll_ok,
                               (u64)(uintptr_t)"SystemFunction036", 0);
    CHECK(gpa != 0, "GetProcAddress(advapi32, SystemFunction036) -> non-NULL");
  }


  // ---- STAGE 7: the SEH/unwind guest (OrTMM's CRT startup surface) ----
  if (seh_file) {
    trt_resolver rs2;
    trt_resolver_default(&rs2);
    trt_image* img2 = nullptr;
    const int rc2 = trt_map_pe(&be, &rs2, seh_file, seh_size, &img2);
    CHECK(rc2 == TRT_OK, "SEH guest PE maps");
    if (rc2 == TRT_OK && img2) {
      // the console host registers the main guest image (HMODULE == base);
      // RtlLookupFunctionEntry walks exactly that registry
      trt_images_register_main(img2, "guest_seh.exe");
      CHECK(trt_seal_image(&be, img2) == TRT_OK, "SEH image sealed (W^X)");
      const u64 sret = win64_call(img2->base + img2->entry_rva, 0, 0, 0);
      printf("  SEH guest returned 0x%llx (want 0xf)\n", (unsigned long long)sret);

      CHECK((sret & 0x3u) == 0x3u, "VirtualProtect RW->R dance (honest PAGE_* codes)");
      CHECK(sret & 0x4u, "page writable again after the protect dance");
      CHECK(sret & 0x8u, "VirtualProtect restored RW after R");
      CHECK(sret & 0x10u, "RtlLookupFunctionEntry answered for a live RIP");
    }
    free(seh_file);
  }

  // ---- STAGE 8: directory enumeration (the FindFirstFile family) ----
  // The measured PyInstaller/engine blocker: FindFirstFileExW used to be an
  // INVALID_HANDLE-stub. Prove real enumeration through the bridge surface:
  // seed a dedicated subdirectory, enumerate it via a wildcard, verify names
  // arrive in a real-layout WIN32_FIND_DATAW (cFileName at +44), and that
  // FindClose is honest both ways.
  {
    // seed a dedicated dir host-side (keeps other suites' files out of the count)
    char seeddir[600];
    snprintf(seeddir, sizeof(seeddir), "%s/seedfind", argv[2]);
    mkdir(seeddir, 0755);
    char seed[700];
    snprintf(seed, sizeof(seed), "%s/seed_a.dat", seeddir);
    FILE* f = fopen(seed, "wb"); if (f) { fputs("a", f); fclose(f); }
    snprintf(seed, sizeof(seed), "%s/seed_b.dat", seeddir);
    f = fopen(seed, "wb"); if (f) { fputs("bb", f); fclose(f); }
    snprintf(seed, sizeof(seed), "%s/seed_c.txt", seeddir);
    f = fopen(seed, "wb"); if (f) { fputs("ccc", f); fclose(f); }

    // guest-side calls through the registered bridge (ms_abi on Linux);
    // struct mirrors the REAL x64 WIN32_FIND_DATAW the guest would pass
    // (FILETIMEs are 4-aligned in Win32: attrs +0, times +4/+12/+20,
    // size hi/lo +28/+32, reserved +36/+40, name +44)
    struct find_data_w { u32 attrs; u32 c_lo, c_hi, a_lo, a_hi, w_lo, w_hi;
                         u32 szhi, szlo, r1, r2;
                         u16 name[260]; u16 alt[14]; };
    static_assert(offsetof(find_data_w, name) == 44, "real WIN32_FIND_DATAW cFileName offset");
    static find_data_w fd;
    memset(&fd, 0, sizeof(fd));
    u16 pat[] = { 'C', ':', '\\', 's', 'e', 'e', 'd', 'f', 'i', 'n', 'd', '\\', '*', 0 };
    // real Win64 ABI: 6 args (name, level, data, searchOp, extra, flags)
    void* fh = trt_FindFirstFileExW(pat, 0, &fd, 0, nullptr, 0);
    int found = 0, dat_seen = 0, txt_seen = 0;
    char first_name[64] = {};
    if (fh != (void*)(uintptr_t)-1) {
      for (;;) {
        ++found;
        char n[64] = {};
        for (int i = 0; fd.name[i] && i < 63; ++i) n[i] = (char)fd.name[i];
        if (!first_name[0]) snprintf(first_name, sizeof(first_name), "%s", n);
        if (strstr(n, ".dat")) ++dat_seen;
        if (strstr(n, ".txt")) ++txt_seen;
        memset(&fd, 0, sizeof(fd));
        if (!trt_FindNextFileW(fh, &fd)) break;
      }
      CHECK(trt_FindClose(fh) == 1, "FindClose -> TRUE");
      // re-close is honestly FALSE (invalid handle after free)
      CHECK(trt_FindClose(fh) == 0, "double FindClose -> FALSE (honest)");
    }
    CHECK(fh != (void*)(uintptr_t)-1, "FindFirstFileExW(C:\\seedfind\\*) -> real handle");
    // C:\Temp lives in the drive ROOT, not seedfind — the enumeration is
    // exactly the 3 seeded files
    CHECK(found == 3, "enumerated exactly the 3 seeded files");
    CHECK(dat_seen == 2 && txt_seen == 1, "wildcard matched *.dat x2 + *.txt x1");
    // size arrives at the real offset (seed_c.txt = 3 bytes)
    // targeted wildcard
    u16 pat2[] = { 'C', ':', '\\', 's', 'e', 'e', 'd', 'f', 'i', 'n', 'd', '\\', '*', '.', 't', 'x', 't', 0 };
    memset(&fd, 0, sizeof(fd));
    void* fh2 = trt_FindFirstFileExW(pat2, 0, &fd, 0, nullptr, 0);
    CHECK(fh2 != (void*)(uintptr_t)-1, "FindFirstFileExW(C:\\seedfind\\*.txt) -> handle");
    if (fh2 != (void*)(uintptr_t)-1) {
      char n[64] = {};
      for (int i = 0; fd.name[i] && i < 63; ++i) n[i] = (char)fd.name[i];
      CHECK(strstr(n, "seed_c.txt") != nullptr, "pattern *.txt matched seed_c.txt");
      CHECK(trt_FindNextFileW(fh2, &fd) == 0, "no more *.txt (honest end)");
      trt_FindClose(fh2);
    }
    // missing dir -> INVALID_HANDLE_VALUE + named log (fail-visible)
    u16 pat3[] = { 'C', ':', '\\', 'n', 'o', 'd', 'i', 'r', '\\', '*', 0 };
    CHECK(trt_FindFirstFileExW(pat3, 0, &fd, 0, nullptr, 0) == (void*)(uintptr_t)-1,
          "FindFirstFileExW(missing dir) -> INVALID_HANDLE_VALUE");
  }

  // ---- STAGE 9: WIN32_FILE_ATTRIBUTE_DATA layout (GetFileAttributesExW) ----
  // Regression: the sizes were written at +36/+40 (one u32 past the real
  // 36-byte x64 record), which zeroed a guest /GS stack cookie in OrTMM and
  // fastfailed the boot. Guard dwords past the record must survive the call.
  {
    char seed[600];
    snprintf(seed, sizeof(seed), "%s/attr_probe.bin", argv[2]);
    FILE* f = fopen(seed, "wb");
    if (f) { fputs("attribute-probe", f); fclose(f); }
    u16 wname[300];
    const char* rel = "attr_probe.bin";
    u32 wi = 0;
    for (; rel[wi]; ++wi) wname[wi] = (u16)(u8)rel[wi];
    wname[wi] = 0;
    struct attr_data_w { u32 attrs, c_lo, c_hi, a_lo, a_hi, w_lo, w_hi, szhi, szlo; };
    static_assert(sizeof(attr_data_w) == 36, "real x64 WIN32_FILE_ATTRIBUTE_DATA");
    struct guarded { u64 g0, g1; attr_data_w d; u64 g2, g3; };
    static guarded gd;
    gd.g0 = 0xA11CE111; gd.g1 = 0xB22CE222; gd.g2 = 0xC33DE333; gd.g3 = 0xD44EE444;
    memset(&gd.d, 0, sizeof(gd.d));
    CHECK(trt_GetFileAttributesExW(wname, 0, &gd.d) == 1,
          "GetFileAttributesExW -> TRUE");
    CHECK(gd.g0 == 0xA11CE111 && gd.g1 == 0xB22CE222 &&
          gd.g2 == 0xC33DE333 && gd.g3 == 0xD44EE444,
          "guard qwords intact (no write past the 36-byte record)");
    CHECK(gd.d.szlo == 15 && gd.d.szhi == 0, "size at the real +32/+28 offsets");
    // Missing path -> FALSE (real Win32). Returning TRUE wedged PyInstaller's
    // unique-temp-dir search into an infinite silent probe loop (OrTMM v0.33).
    u16 wmiss[300];
    const char* relm = "attr_missing_zz.bin";
    u32 mi = 0;
    for (; relm[mi]; ++mi) wmiss[mi] = (u16)(u8)relm[mi];
    wmiss[mi] = 0;
    CHECK(trt_GetFileAttributesExW(wmiss, 0, &gd.d) == 0,
          "GetFileAttributesExW(missing) -> FALSE, not a silent TRUE");
  }

  printf("\nRESULT: %s\n", failures ? "FAILURES" : "ALL PASS");
  return failures ? 1 : 0;
}
