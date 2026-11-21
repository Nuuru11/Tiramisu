# UWP patches for stock Mono 6.8.0.105

Drop `.patch` files here (unified diff, `-p1` against the extracted
`mono-6.8.0.105/` tree); `build-mono-uwp.sh` applies them in sorted
order to each fresh extraction, before generating `config.h` and
compiling. Every patch is guarded so the desktop build is unchanged:
prefer upstreamable `#if G_HAVE_API_SUPPORT(...)` gating (Mono's own
WINAPI-family idiom) over Tiramisu-specific hacks.

- `01-gfile-posix-getcwd.patch` — `g_get_current_dir` uses `_getcwd`
  on Windows: the POSIX alias `getcwd` is not declared under
  WINAPI_FAMILY_APP (UCRT gates it), and the implicit declaration it
  fell back to truncated the pointer on x64.
- `02-mono-tls-winrt-teb.patch` — `mono-tls.h` falls back to a local
  TEB definition (copied verbatim from the Unity mono fork, MIT) when
  `HAVE_WINTERNL_H` is unset: under UWP the SDK's `winternl.h` is
  empty (desktop-gated), so `PTEB` was undeclared everywhere.
  `NtCurrentTeb()` itself is declared in `winnt.h` with no partition
  guard, so only the type was missing.
- `03-winconfig-winrt-winternl.patch` — `winconfig.h` defines
  `HAVE_WINTERNL_H` only under the classic (desktop) WINAPI family,
  so the UWP build takes patch 02's fallback. (Touches the `config.h`
  template — this is why patches apply before `config.h` generation.)
- `04-w32file-winrt-createfile.patch` — `w32file-win32.c` and
  `file-mmap-windows.c` use `CreateFileFromAppW` under UWP:
  `CreateFileW` is not declared under the APP partition
  (`fileapi.h` gates it to DESKTOP | SYSTEM | GAMES), and
  `CreateFileFromAppW` is its UWP equivalent with the same signature.
  Keeps managed file I/O working in the AppContainer.
- `05-w32process-winrt-fileversion.patch` — guard the
  `FileVersionInfo.GetVersionInfo` icall's call to
  `mono_w32process_get_fileversion`, which is only defined under the
  classic family (the version-resource APIs it uses are
  desktop-only). Under UWP the icall sets the filename but reports
  empty version fields.
- `06-exceptions-amd64-winrt-veh.patch` — don't install/remove the
  vectored exception handler under UWP: `Add/RemoveVectoredException-
  Handler` are desktop-only. The VEH serves JIT implicit null-checks
  and is not needed in interpreter mode. `SetUnhandledExceptionFilter`
  (APP-visible) is left alone.
- `07-sgen-protocol-winrt-createfile.patch` — the SGen binary-protocol
  debug log uses `CreateFileFromAppW` with a UTF-16 filename under
  UWP: `CreateFileA` is desktop-gated.
- `08-mono-dl-winrt-loadpackagedlibrary.patch` — pass `0`, not `NULL`,
  as `LoadPackagedLibrary`'s DWORD flags argument (clang rejects the
  pointer-to-int conversion MSVC only warns about).
- `09-handle-clang-typeof.patch` — let clang-cl use the `__typeof__`
  branch of `MONO_TYPEOF_CAST` instead of the MSVC ternary trick:
  MSVC (non-conformingly) types a ternary of two unrelated pointers
  as the first operand's type, clang types it `void*` per the C
  standard, so `MONO_HANDLE_RAW` returned `void*` and every handle
  field access failed (755 errors). A clang-cl fix, not a UWP fix.
- `10-icall-winrt-logical-drives.patch` — `Environment.GetLogical-
  Drives` returns an empty array under UWP: upstream 6.8 declares
  `mono_icall_get_logical_drives (void)` for the non-classic family
  but never implements it, and the ves_icall calls it with the
  classic signature. UWP has no drive enumeration API anyway.

Patches 11–22 were added after the first UWP link: the link failed
with 20 undefined symbols (lld-link stops at 20), and a sweep of the
sources for classic-gated definitions referenced from unguarded code
found ~35 in total. They fall into three groups: (a) stubs reporting
`NotSupported` for genuinely desktop-only APIs, following Mono's own
`g_unsupported_api` + `mono_error_set_not_supported` idiom (also used
by Unity-Technologies/mono); (b) unguarding functions whose Win32 APIs
are in fact APP-available (verified by compile test + `llvm-nm`
against the xwin SDK's UWP import libraries); (c) correcting
`w32subset.h` availability macros that said classic-only for APIs the
SDK does provide under UWP.

- `11-w32process-win32-winrt-process.patch` — UWP `NotSupported`
  stubs for the four `System.Diagnostics.Process` icalls
  (GetProcess/ShellExecuteEx/CreateProcess/GetProcesses_internal);
  the nine `Microsoft.Win32.NativeMethods` icalls move OUT of the
  classic guard (CloseHandle, TerminateProcess, GetExitCodeProcess,
  Get/SetProcessWorkingSetSize, Get/SetPriorityClass,
  GetProcessTimes, GetCurrentProcess are all APP-available).
- `12-icall-windows-winrt-stubs.patch` — UWP fallbacks for
  `mono_icall_get_windows_folder_path` (empty string),
  `BroadcastSettingChange` (NotSupported) and
  `mono_icall_wait_for_input_idle` (WAIT_TIMEOUT); all three APIs
  are desktop-only.
- `13-gunicode-winrt-charset.patch` — `g_get_charset` reports UTF-8
  under UWP (GetACP is desktop-only; the AppContainer code page is
  UTF-8).
- `14-w32process-winrt-modules.patch` — UWP `NotSupported` stubs for
  the Process GetModules/ProcessName icalls (EnumProcessModules is
  desktop-only).
- `15-mono-security-winrt-stubs.patch` — UWP `NotSupported` stubs for
  the WindowsIdentity/WindowsImpersonationContext icalls
  (GetCurrentToken, GetRoles, DuplicateToken, SetCurrentToken,
  RevertToSelf) and the KeyPairPersistence ACL helpers
  (Get/SetNamedSecurityInfo are desktop-only). Exception:
  `mono_security_win_get_token_name` returns 0, which the unguarded
  GetTokenName icall treats as an empty name.
- `16-w32subset-winrt-availability.patch` — `HAVE_API_SUPPORT_WIN32_-
  GET_STD_HANDLE` and `_SIGNAL_OBJECT_AND_WAIT` now include UWP:
  both APIs are APP-available (verified), and the classic-only
  values compiled out the MonoIO console icalls, the WaitHandle
  SignalAndWait icall, and its implementation.
- `17-w32file-winrt-movefileex.patch` — `mono_w32file_move` is gated
  on `HAVE_API_SUPPORT_WIN32_MOVE_FILE_EX` (already UWP-available)
  and uses `MoveFileExW` with `MOVEFILE_COPY_ALLOWED` under UWP:
  `MoveFileW` is not declared under the APP family.
- `18-marshal-winrt-hglobal.patch` — unguard the HGlobal marshal
  functions: GlobalAlloc/GlobalReAlloc/GlobalFree are APP-available.
- `19-mono-dl-winrt-error-string.patch` — unguard
  `mono_dl_current_error_string`: FormatMessage/LocalFree are
  APP-available. (`mono_dl_lookup_symbol_in_process` was left gated
  at first — patch 24 stubs it: `mono_dl_lookup_symbol` calls it
  intra-file as its fallback, so the "no callers" claim was wrong.)
- `20-gmisc-winrt-locale.patch` — `g_win32_getlocale` uses
  `GetUserDefaultLocaleName` under UWP (GetThreadLocale/
  GetLocaleInfoA are desktop-only); returns the same "en-US" form.
- `21-gmodule-winrt-find-symbol.patch` — UWP `w32_find_symbol`
  returns NULL (EnumProcessModules is desktop-only); the
  GetProcAddress path in `g_module_symbol` — how `__Internal`
  P/Invoke resolves FNA3D — is unaffected. (`g_module_error` is
  likewise gated but nothing calls it.)
- `22-mono-proclib-winrt-cpu-usage.patch` — `mono_cpu_usage` reports
  0 (idle) under UWP (GetSystemTimes is desktop-only); the
  threadpool monitor treats that as spare capacity and grows the
  pool up to its max.

Patches 23–27 were added after the second UWP link: it failed with 19
undefined symbols (no "too many errors" truncation this time, so the
list was complete). Two of the "deliberately not patched" claims from
round 1 turned out wrong — `mono_mmap_*` IS called (the icall table
and `image.c`), and `mono_dl_lookup_symbol_in_process` IS called
(intra-file, by `mono_dl_lookup_symbol`). Lesson: when checking for
callers, grep INCLUDING the defining file.

- `23-file-mmap-uwp.patch` — `file-mmap-windows.c` compiles under
  UWP (its outer guard becomes classic || UWP): the file already
  contains UWP branches (CreateFileFromAppW at line 279) that were
  dead code behind the classic-only guard. All seven `mono_mmap_*`
  functions are real under UWP now (CreateFileMapping/MapViewOfFile/
  FlushViewOfFile/VirtualQuery/SetHandleInformation are all
  APP-available; VirtualQueryEx appears only in comments). Also
  defines `HANDLE_FLAG_INHERIT` (0x00000001) under UWP — the
  constant is not declared under the APP family.
- `24-mono-dl-lookup-uwp.patch` — UWP stub for
  `mono_dl_lookup_symbol_in_process` returning NULL
  (EnumProcessModules is desktop-only); the GetProcAddress primary
  path in `mono_dl_lookup_symbol` is unaffected.
- `25-coree-uwp.patch` — UWP stubs for the ntdll CLR-hosting entry
  points `_CorDllMain` (FALSE), `_CorExeMain` (1) and
  `_CorValidateImage` (E_FAIL): under UWP there is no mscoree/ntdll
  hosting path, assemblies are opened by `mono_image_open`, and the
  stubs exist only so the `ExportFixups` table links; they are never
  called in the embedding path. `MonoLoadImage` gets a REAL UWP
  implementation (CreateFileFromAppW + GetFileSizeEx +
  CreateFileMapping/MapViewOfFile + LoadLibrary — all APP-available):
  `mono_image_open_a_lot_parameterized` calls it on the assembly
  loading path, so a stub returning NULL would break assembly
  loading. `mono_coree_set_act_ctx` is a no-op under UWP (activation
  contexts are desktop-only; only the mscoree hosting path calls it).
- `26-stat-profiler-uwp.patch` — UWP no-op stubs for the sampling
  stat profiler setup/shutdown (winmm timers + OpenThread are
  desktop-only; the profiler is never enabled under UWP) and for
  `mono_setup_thread_context` (returns FALSE — OpenThread/
  GetThreadContext are desktop-only; the caller ignores the return
  and leaves the unwind state invalid).
- `27-error-internals-intrin.patch` — `#include <intrin.h>` in
  `mono-error-internals.h`, and a clang-only replacement for
  `__writegsdword` in `mono_SetLastError`: clang has no builtin for
  `__writegsdword` — even with `<intrin.h>` it emits an external call
  (and `#pragma intrinsic` warns it is not a recognized builtin), so
  the link failed on it. The clang branch writes the TEB
  LastErrorValue slot directly with
  `__asm__ volatile ("movl %0, %%gs:0x68")`, exactly the instruction
  MSVC's intrinsic compiles to (verified by disassembly).
  `__readgsdword` IS a clang builtin and needs no change. A clang-cl
  fix, not a UWP fix.

The two x64 assembly functions (`mono_context_get_current` and
`copy_stack_data_internal_win32_wrapper`, defined in Mono's
`mono/utils/win64.asm` — MASM, git only, not in the release tarball)
are NOT patched: `asm/win64.S` is a mechanical GNU as translation of
that file, assembled by `build-mono-uwp.sh` and merged into the
archive. Its instruction stream was verified by disassembly.

Deliberately NOT patched (verified no link problem):
`mono_icall_drive_info_get_drive_type` (declared, never defined,
never called); `g_module_error`, `mono_native_thread_join`
(classic-gated, no callers in compiled code);
`mono_security_win_get_token_name`'s only Windows caller is the
gated GetCurrentToken icall (the unguarded GetTokenName icall is
handled by patch 15's 0-returning stub).

Patches 31-32 replace the interpreter's two JIT trampoline transitions
with precompiled, image-backed static stubs (the UWP AppContainer
forbids executing dynamically generated code -- the .122 Celeste death
was the first P/Invoke executing the JIT interp->native trampoline;
see mono-aot-trampoline-analysis.md in the progress docs). The stubs
live in asm/win64.S (the patches only rewire interp.c to use them);
both are mechanical translations of the tramp-amd64.c emitters, so the
C side (which builds and reads the CallContext) is unchanged. Gated on
`G_HAVE_API_SUPPORT(HAVE_UWP_WINAPI_SUPPORT)` like the other UWP
patches; the desktop path is untouched.

- `31-uwp-interp-to-native-static-stub.patch` -- `get_interp_to_native_trampoline`
  (interp.c) returns `mono_interp_to_native_trampoline_static`
  (win64.S) on UWP instead of `mono_arch_get_interp_to_native_trampoline`.
  The stub takes (callme, CallContext*) and performs the Win64 native
  call. Covers the first-P/Invoke death.
- `32-uwp-native-to-interp-static-stub.patch` -- `interp_create_method_pointer`
  (interp.c) on UWP skips the interp_in wrapper JIT compile (leaving
  entry_wrapper NULL takes the existing native->interp trampoline
  branch), uses `mono_native_to_interp_trampoline_static` (win64.S)
  instead of `mono_arch_get_native_to_interp_trampoline`, uses the
  direct `interp_entry_from_trampoline` entry for every signature
  (no LMF-wrapper JIT; the LMF wrapper serves mixed JIT+interp mode
  only), and hands out ftnptr entry points from a fixed pool of 256
  static thunks (`mono_uwp_alloc_ftnptr_thunk`) instead of
  `mono_create_ftnptr_arg_trampoline` (JIT). Each thunk loads its
  MonoFtnDesc* from `mono_ftnptr_table[i]` into r10 and jumps to the
  static native->interp stub.

- `33-uwp-getmodulehandle.patch` -- `HAVE_API_SUPPORT_WIN32_GET_MODULE_HANDLE`
  (w32subset.h) now includes UWP, like GET_STD_HANDLE / SIGNAL_OBJECT_AND_WAIT
  (patch 16). It was classic-only, which compiled out the file==NULL branch
  of `mono_dl_open_file` (mono-dl-windows.c:78-82) and left
  `g_assert_not_reached()` in its place — so the FIRST `__Internal` pinvoke
  lookup aborted with "* Assertion: should not be reached at
  mono-dl-windows.c:81". That was the silent death after
  `mono image: Steamworks.NET` on .112-.126 (caught by the .126 diagnostic:
  stderr redirect + SIGABRT handler). GetModuleHandleW is declared under
  the UWP (APP) family and present in the UWP import libraries (the .126
  host already links and calls it at runtime for the VEH install).
