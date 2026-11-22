// tiramisu/src/trt/x86_bridge.cpp
// 32-bit → 64-bit bridge thunking for x86 guest execution.
//
// When a 32-bit guest calls an imported function (via CALL [IAT]),
// this layer intercepts the call, reads arguments from the 32-bit stack,
// invokes the corresponding 64-bit Tiramisu bridge function, and
// writes the 32-bit result back to EAX.
#include "x86_cpu.h"
#include "trt.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// Registry of bridge functions: maps (module, symbol) -> function pointer
struct bridge_entry {
    const char* module;
    const char* symbol;
    void* fn;  // 64-bit function pointer
};

// Forward declaration of Tiramisu's module resolver
extern "C" void* trt_resolve_symbol(const char* module, const char* symbol);

static int lookup_arg_count(const char* module, const char* symbol);
static int lookup_cdecl(const char* module, const char* symbol);
static uint32_t lookup_ptr_mask(x86_context* ctx, const char* module,
                                const char* symbol, const uint32_t* args,
                                int nargs);

// Call an already-resolved 64-bit bridge function with 32-bit arguments.
static uint32_t x86_call_fn(x86_context* ctx, void* fn, const char* module,
                            const char* symbol, uint32_t* args, int nargs) {

    // Widen every argument to 64 bits. On both Win64 and SysV x64 an integer
    // argument narrower than 64 bits occupies the low bits of its register
    // slot, so a bridge expecting a 32-bit int reads the correct value, while
    // a bridge expecting a pointer gets the full host address.
    uint32_t mask = lookup_ptr_mask(ctx, module, symbol, args, nargs);
    uint64_t a[16];
    for (int i = 0; i < 16; i++) {
        uint32_t v = (i < nargs) ? args[i] : 0;
        if (i < nargs && (mask & (1u << i)) && ctx->mem.base && v) {
            // Guest VA -> host pointer into the contiguous 4GB reservation.
            a[i] = (uint64_t)(uintptr_t)(ctx->mem.base + v);
        } else {
            a[i] = (uint64_t)v;
        }
    }

    typedef uint64_t (*generic_fn)(...);
    generic_fn gfn = (generic_fn)fn;
    uint64_t r = 0;
    switch (nargs) {
        case 0: r = gfn(); break;
        case 1: r = gfn(a[0]); break;
        case 2: r = gfn(a[0], a[1]); break;
        case 3: r = gfn(a[0], a[1], a[2]); break;
        case 4: r = gfn(a[0], a[1], a[2], a[3]); break;
        case 5: r = gfn(a[0], a[1], a[2], a[3], a[4]); break;
        case 6: r = gfn(a[0], a[1], a[2], a[3], a[4], a[5]); break;
        case 7: r = gfn(a[0], a[1], a[2], a[3], a[4], a[5], a[6]); break;
        case 8: r = gfn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]); break;
        case 9: r = gfn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]); break;
        case 10: r = gfn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]); break;
        case 11: r = gfn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10]); break;
        case 12: r = gfn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]); break;
        case 13: r = gfn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12]); break;
        case 14: r = gfn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13]); break;
        case 15: r = gfn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13], a[14]); break;
        case 16: r = gfn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]); break;
        default: {
            char buf[256];
            snprintf(buf, sizeof(buf), "x86_bridge: too many args (%d) for %s!%s", nargs, module, symbol);
            if (ctx->log) ctx->log(buf);
            return 0;
        }
    }

    return (uint32_t)r;
}

// Resolve then call (public entry used by the older direct-call path).
uint32_t x86_call_bridge(x86_context* ctx, const char* module, const char* symbol,
                         uint32_t* args, int nargs) {
    void* fn = trt_resolve_symbol(module, symbol);
    if (!fn) {
        char buf[256];
        snprintf(buf, sizeof(buf), "x86_bridge: unresolved %s!%s -> returning 0", module, symbol);
        if (ctx->log) ctx->log(buf);
        return 0;
    }
    return x86_call_fn(ctx, fn, module, symbol, args, nargs);
}


// ---- Argument count database for common Windows APIs ----
// Maps "module!symbol" -> number of arguments (for __stdcall)
static int lookup_arg_count(const char* module, const char* symbol) {
    // Helper macro for entries
    #define ARGENTRY(mod, sym, n) if (strcasecmp(module, mod) == 0 && strcmp(symbol, sym) == 0) return n;

    // kernel32
    if (strcasecmp(module, "kernel32.dll") == 0 || strcasecmp(module, "kernel32") == 0) {
        if (strcmp(symbol, "GetModuleHandleA") == 0) return 1;
        if (strcmp(symbol, "GetModuleHandleW") == 0) return 1;
        if (strcmp(symbol, "GetProcAddress") == 0) return 2;
        if (strcmp(symbol, "LoadLibraryA") == 0) return 1;
        if (strcmp(symbol, "LoadLibraryW") == 0) return 1;
        if (strcmp(symbol, "LoadLibraryExA") == 0) return 3;
        if (strcmp(symbol, "LoadLibraryExW") == 0) return 3;
        if (strcmp(symbol, "FreeLibrary") == 0) return 1;
        if (strcmp(symbol, "CreateFileA") == 0) return 7;
        if (strcmp(symbol, "CreateFileW") == 0) return 7;
        if (strcmp(symbol, "ReadFile") == 0) return 5;
        if (strcmp(symbol, "WriteFile") == 0) return 5;
        if (strcmp(symbol, "CloseHandle") == 0) return 1;
        if (strcmp(symbol, "GetFileSize") == 0) return 2;
        if (strcmp(symbol, "GetFileSizeEx") == 0) return 2;
        if (strcmp(symbol, "SetFilePointer") == 0) return 4;
        if (strcmp(symbol, "SetFilePointerEx") == 0) return 4;
        if (strcmp(symbol, "VirtualAlloc") == 0) return 4;
        if (strcmp(symbol, "VirtualFree") == 0) return 3;
        if (strcmp(symbol, "VirtualProtect") == 0) return 4;
        if (strcmp(symbol, "HeapAlloc") == 0) return 3;
        if (strcmp(symbol, "HeapFree") == 0) return 3;
        if (strcmp(symbol, "HeapCreate") == 0) return 3;
        if (strcmp(symbol, "GetLastError") == 0) return 0;
        if (strcmp(symbol, "SetLastError") == 0) return 1;
        if (strcmp(symbol, "GetCommandLineA") == 0) return 0;
        if (strcmp(symbol, "GetCommandLineW") == 0) return 0;
        if (strcmp(symbol, "ExitProcess") == 0) return 1;
        if (strcmp(symbol, "GetCurrentProcess") == 0) return 0;
        if (strcmp(symbol, "GetCurrentThread") == 0) return 0;
        if (strcmp(symbol, "GetCurrentThreadId") == 0) return 0;
        if (strcmp(symbol, "Sleep") == 0) return 1;
        if (strcmp(symbol, "GetTickCount") == 0) return 0;
        if (strcmp(symbol, "QueryPerformanceCounter") == 0) return 1;
        if (strcmp(symbol, "QueryPerformanceFrequency") == 0) return 1;
        if (strcmp(symbol, "InitializeCriticalSection") == 0) return 1;
        if (strcmp(symbol, "EnterCriticalSection") == 0) return 1;
        if (strcmp(symbol, "LeaveCriticalSection") == 0) return 1;
        if (strcmp(symbol, "DeleteCriticalSection") == 0) return 1;
        if (strcmp(symbol, "CreateEventA") == 0) return 4;
        if (strcmp(symbol, "CreateEventW") == 0) return 4;
        if (strcmp(symbol, "SetEvent") == 0) return 1;
        if (strcmp(symbol, "WaitForSingleObject") == 0) return 2;
        if (strcmp(symbol, "WaitForMultipleObjects") == 0) return 4;
        if (strcmp(symbol, "CreateThread") == 0) return 6;
        if (strcmp(symbol, "GetModuleFileNameA") == 0) return 3;
        if (strcmp(symbol, "GetModuleFileNameW") == 0) return 3;
        if (strcmp(symbol, "GetEnvironmentVariableA") == 0) return 3;
        if (strcmp(symbol, "GetEnvironmentVariableW") == 0) return 3;
        if (strcmp(symbol, "SetEnvironmentVariableA") == 0) return 2;
        if (strcmp(symbol, "MultiByteToWideChar") == 0) return 6;
        if (strcmp(symbol, "WideCharToMultiByte") == 0) return 8;
        if (strcmp(symbol, "GetStdHandle") == 0) return 1;
        if (strcmp(symbol, "WriteConsoleA") == 0) return 5;
        if (strcmp(symbol, "WriteConsoleW") == 0) return 5;
        if (strcmp(symbol, "GetConsoleMode") == 0) return 2;
        if (strcmp(symbol, "SetConsoleMode") == 0) return 2;
        if (strcmp(symbol, "GetSystemInfo") == 0) return 1;
        if (strcmp(symbol, "GetVersionExA") == 0) return 1;
        if (strcmp(symbol, "GetVersionExW") == 0) return 1;
        if (strcmp(symbol, "IsDebuggerPresent") == 0) return 0;
        if (strcmp(symbol, "OutputDebugStringA") == 0) return 1;
        if (strcmp(symbol, "OutputDebugStringW") == 0) return 1;
        if (strcmp(symbol, "RaiseException") == 0) return 4;
        if (strcmp(symbol, "UnhandledExceptionFilter") == 0) return 1;
        if (strcmp(symbol, "SetUnhandledExceptionFilter") == 0) return 1;
        if (strcmp(symbol, "TerminateProcess") == 0) return 2;
        if (strcmp(symbol, "GetProcessHeap") == 0) return 0;
        if (strcmp(symbol, "RtlMoveMemory") == 0) return 3;
        if (strcmp(symbol, "RtlZeroMemory") == 0) return 2;
        if (strcmp(symbol, "RtlFillMemory") == 0) return 3;
    }

    // user32
    if (strcasecmp(module, "user32.dll") == 0 || strcasecmp(module, "user32") == 0) {
        if (strcmp(symbol, "MessageBoxA") == 0) return 4;
        if (strcmp(symbol, "MessageBoxW") == 0) return 4;
        if (strcmp(symbol, "RegisterClassA") == 0) return 1;
        if (strcmp(symbol, "RegisterClassW") == 0) return 1;
        if (strcmp(symbol, "RegisterClassExA") == 0) return 1;
        if (strcmp(symbol, "RegisterClassExW") == 0) return 1;
        if (strcmp(symbol, "CreateWindowExA") == 0) return 12;
        if (strcmp(symbol, "CreateWindowExW") == 0) return 12;
        if (strcmp(symbol, "DestroyWindow") == 0) return 1;
        if (strcmp(symbol, "ShowWindow") == 0) return 2;
        if (strcmp(symbol, "UpdateWindow") == 0) return 1;
        if (strcmp(symbol, "GetMessageA") == 0) return 4;
        if (strcmp(symbol, "GetMessageW") == 0) return 4;
        if (strcmp(symbol, "PeekMessageA") == 0) return 5;
        if (strcmp(symbol, "PeekMessageW") == 0) return 5;
        if (strcmp(symbol, "TranslateMessage") == 0) return 1;
        if (strcmp(symbol, "DispatchMessageA") == 0) return 1;
        if (strcmp(symbol, "DispatchMessageW") == 0) return 1;
        if (strcmp(symbol, "DefWindowProcA") == 0) return 4;
        if (strcmp(symbol, "DefWindowProcW") == 0) return 4;
        if (strcmp(symbol, "PostQuitMessage") == 0) return 1;
        if (strcmp(symbol, "LoadIconA") == 0) return 2;
        if (strcmp(symbol, "LoadIconW") == 0) return 2;
        if (strcmp(symbol, "LoadCursorA") == 0) return 2;
        if (strcmp(symbol, "LoadCursorW") == 0) return 2;
        if (strcmp(symbol, "GetDC") == 0) return 1;
        if (strcmp(symbol, "ReleaseDC") == 0) return 2;
        if (strcmp(symbol, "BeginPaint") == 0) return 2;
        if (strcmp(symbol, "EndPaint") == 0) return 2;
        if (strcmp(symbol, "GetClientRect") == 0) return 2;
        if (strcmp(symbol, "GetWindowRect") == 0) return 2;
        if (strcmp(symbol, "SetWindowTextA") == 0) return 2;
        if (strcmp(symbol, "SetWindowTextW") == 0) return 2;
        if (strcmp(symbol, "GetWindowTextA") == 0) return 3;
        if (strcmp(symbol, "GetWindowTextW") == 0) return 3;
        if (strcmp(symbol, "InvalidateRect") == 0) return 3;
        if (strcmp(symbol, "GetSystemMetrics") == 0) return 1;
    }

    // gdi32
    if (strcasecmp(module, "gdi32.dll") == 0 || strcasecmp(module, "gdi32") == 0) {
        if (strcmp(symbol, "CreateCompatibleDC") == 0) return 1;
        if (strcmp(symbol, "DeleteDC") == 0) return 1;
        if (strcmp(symbol, "SelectObject") == 0) return 2;
        if (strcmp(symbol, "DeleteObject") == 0) return 1;
        if (strcmp(symbol, "CreateSolidBrush") == 0) return 1;
        if (strcmp(symbol, "CreatePen") == 0) return 3;
        if (strcmp(symbol, "TextOutA") == 0) return 5;
        if (strcmp(symbol, "TextOutW") == 0) return 5;
        if (strcmp(symbol, "BitBlt") == 0) return 9;
        if (strcmp(symbol, "StretchBlt") == 0) return 11;
        if (strcmp(symbol, "GetStockObject") == 0) return 1;
        if (strcmp(symbol, "SetBkMode") == 0) return 2;
        if (strcmp(symbol, "SetTextColor") == 0) return 2;
    }

    // advapi32
    if (strcasecmp(module, "advapi32.dll") == 0 || strcasecmp(module, "advapi32") == 0) {
        if (strcmp(symbol, "RegOpenKeyExA") == 0) return 5;
        if (strcmp(symbol, "RegOpenKeyExW") == 0) return 5;
        if (strcmp(symbol, "RegQueryValueExA") == 0) return 6;
        if (strcmp(symbol, "RegQueryValueExW") == 0) return 6;
        if (strcmp(symbol, "RegCloseKey") == 0) return 1;
        if (strcmp(symbol, "RegCreateKeyExA") == 0) return 9;
        if (strcmp(symbol, "RegCreateKeyExW") == 0) return 9;
    }

    // ole32
    if (strcasecmp(module, "ole32.dll") == 0 || strcasecmp(module, "ole32") == 0) {
        if (strcmp(symbol, "CoInitialize") == 0) return 1;
        if (strcmp(symbol, "CoInitializeEx") == 0) return 2;
        if (strcmp(symbol, "CoUninitialize") == 0) return 0;
        if (strcmp(symbol, "CoCreateInstance") == 0) return 5;
    }

    // winmm
    if (strcasecmp(module, "winmm.dll") == 0 || strcasecmp(module, "winmm") == 0) {
        if (strcmp(symbol, "timeBeginPeriod") == 0) return 1;
        if (strcmp(symbol, "timeEndPeriod") == 0) return 1;
        if (strcmp(symbol, "timeGetTime") == 0) return 0;
    }

    // d3d9
    if (strcasecmp(module, "d3d9.dll") == 0 || strcasecmp(module, "d3d9") == 0) {
        if (strcmp(symbol, "Direct3DCreate9") == 0) return 1;
    }

    // Default: unknown, return -1 to indicate "use default"
    return -1;
    #undef ARGENTRY
}

// ---- Calling-convention helpers ----
static int ieq_n(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
    return 1;
}
static int mod_eq(const char* a, const char* b) {
    size_t la = strcspn(a, "."), lb = strcspn(b, ".");
    return la == lb && ieq_n(a, b, la);
}
static int sym_eq(const char* a, const char* b) { return strcmp(a, b) == 0; }

// __cdecl / variadic exports: the CALLER cleans the stack, so the trap
// dispatcher pops only the return address.
static int lookup_cdecl(const char* module, const char* symbol) {
    (void)module;
    static const char* k_cdecl[] = {
        "printf", "fprintf", "sprintf", "snprintf", "_snprintf", "vsprintf",
        "vprintf", "scanf", "fscanf", "sscanf", "malloc", "calloc", "realloc",
        "free", "wsprintfA", "wsprintfW", "wvsprintfA", "wvsprintfW", nullptr};
    for (int i = 0; k_cdecl[i]; i++)
        if (sym_eq(symbol, k_cdecl[i])) return 1;
    return 0;
}

// Which arguments are guest pointers (bit i = arg i). Known APIs get an exact
// mask; unknown ones fall back to a conservative heuristic.
static uint32_t lookup_ptr_mask(x86_context* ctx, const char* module,
                                const char* symbol, const uint32_t* args,
                                int nargs) {
    #define P(m) return (m)
    if (mod_eq(module, "kernel32")) {
        if (sym_eq(symbol, "CreateFileA") || sym_eq(symbol, "CreateFileW")) P(0x9);
        if (sym_eq(symbol, "ReadFile") || sym_eq(symbol, "WriteFile")) P(0x1A);
        if (sym_eq(symbol, "GetModuleHandleA") || sym_eq(symbol, "GetModuleHandleW")) P(0x1);
        if (sym_eq(symbol, "LoadLibraryA") || sym_eq(symbol, "LoadLibraryW")) P(0x1);
        if (sym_eq(symbol, "LoadLibraryExA") || sym_eq(symbol, "LoadLibraryExW")) P(0x1);
        if (sym_eq(symbol, "GetModuleFileNameA") || sym_eq(symbol, "GetModuleFileNameW")) P(0x2);
        if (sym_eq(symbol, "GetEnvironmentVariableA") || sym_eq(symbol, "GetEnvironmentVariableW")) P(0x3);
        if (sym_eq(symbol, "SetEnvironmentVariableA") || sym_eq(symbol, "SetEnvironmentVariableW")) P(0x3);
        if (sym_eq(symbol, "MultiByteToWideChar") || sym_eq(symbol, "WideCharToMultiByte")) P(0x14);
        if (sym_eq(symbol, "OutputDebugStringA") || sym_eq(symbol, "OutputDebugStringW")) P(0x1);
        if (sym_eq(symbol, "GetSystemInfo") || sym_eq(symbol, "GetVersionExA") ||
            sym_eq(symbol, "GetVersionExW")) P(0x1);
        if (sym_eq(symbol, "RtlMoveMemory")) P(0x3);
        if (sym_eq(symbol, "RtlZeroMemory")) P(0x1);
        if (sym_eq(symbol, "RtlFillMemory")) P(0x1);
        if (sym_eq(symbol, "lstrlenA") || sym_eq(symbol, "lstrlenW")) P(0x1);
        if (sym_eq(symbol, "CreateEventA") || sym_eq(symbol, "CreateEventW")) P(0x4);
        if (sym_eq(symbol, "WaitForMultipleObjects")) P(0x1);
    }
    if (mod_eq(module, "user32")) {
        if (sym_eq(symbol, "MessageBoxA") || sym_eq(symbol, "MessageBoxW")) P(0x6);
        if (sym_eq(symbol, "RegisterClassA") || sym_eq(symbol, "RegisterClassW") ||
            sym_eq(symbol, "RegisterClassExA") || sym_eq(symbol, "RegisterClassExW")) P(0x1);
        if (sym_eq(symbol, "GetMessageA") || sym_eq(symbol, "GetMessageW") ||
            sym_eq(symbol, "PeekMessageA") || sym_eq(symbol, "PeekMessageW")) P(0x2);
        if (sym_eq(symbol, "BeginPaint") || sym_eq(symbol, "EndPaint") ||
            sym_eq(symbol, "GetClientRect") || sym_eq(symbol, "GetWindowRect")) P(0x2);
        if (sym_eq(symbol, "SetWindowTextA") || sym_eq(symbol, "SetWindowTextW") ||
            sym_eq(symbol, "GetWindowTextA") || sym_eq(symbol, "GetWindowTextW")) P(0x2);
        if (sym_eq(symbol, "LoadIconA") || sym_eq(symbol, "LoadIconW") ||
            sym_eq(symbol, "LoadCursorA") || sym_eq(symbol, "LoadCursorW")) P(0x2);
        if (sym_eq(symbol, "DispatchMessageA") || sym_eq(symbol, "DispatchMessageW")) P(0x1);
        if (sym_eq(symbol, "InvalidateRect")) P(0x2);
        if (sym_eq(symbol, "CreateWindowExA") || sym_eq(symbol, "CreateWindowExW")) P(0x7006);
    }
    if (mod_eq(module, "gdi32")) {
        if (sym_eq(symbol, "TextOutA") || sym_eq(symbol, "TextOutW")) P(0x8);
        if (sym_eq(symbol, "SelectObject")) P(0x0);
    }
    if (mod_eq(module, "advapi32")) {
        if (sym_eq(symbol, "RegOpenKeyExA") || sym_eq(symbol, "RegOpenKeyExW")) P(0x2);
        if (sym_eq(symbol, "RegQueryValueExA") || sym_eq(symbol, "RegQueryValueExW")) P(0x32);
        if (sym_eq(symbol, "RegSetValueExA") || sym_eq(symbol, "RegSetValueExW")) P(0x12);
        if (sym_eq(symbol, "RegCreateKeyExA") || sym_eq(symbol, "RegCreateKeyExW")) P(0x2);
    }
    if (mod_eq(module, "msvcrt") || mod_eq(module, "ucrtbase")) {
        if (sym_eq(symbol, "printf") || sym_eq(symbol, "sprintf") ||
            sym_eq(symbol, "wsprintf")) P(0x1);
        if (sym_eq(symbol, "strlen") || sym_eq(symbol, "strcmp") ||
            sym_eq(symbol, "strcpy") || sym_eq(symbol, "memcpy") ||
            sym_eq(symbol, "memset")) P(0x3);
    }
    #undef P
    // Heuristic: a value in user space that lands on a committed page is
    // treated as a data pointer. Small integers and sizes stay untouched.
    uint32_t m = 0;
    if (!ctx->mem.base) return 0;
    for (int i = 0; i < nargs; i++) {
        uint32_t v = args[i];
        if (v >= 0x10000u && v < 0x80000000u &&
            x86_page_mapped(ctx, v >> X86_PAGE_SHIFT)) {
            m |= (1u << i);
        }
    }
    return m;
}

// Per-thread state the guest expects to be local, not host-global.
static int special_import(x86_context* ctx, const char* module, const char* symbol,
                          const uint32_t* args, uint32_t* out) {
    if (mod_eq(module, "kernel32") || mod_eq(module, "msvcrt")) {
        if (sym_eq(symbol, "GetLastError")) { *out = (uint32_t)ctx->last_error; return 1; }
        if (sym_eq(symbol, "SetLastError")) { ctx->last_error = (int)args[0]; *out = 0; return 1; }
        if (sym_eq(symbol, "TlsAlloc")) {
            for (int i = 0; i < 64; i++) {
                if (!ctx->regs.tls_used[i]) { ctx->regs.tls_used[i] = 1; *out = (uint32_t)i; return 1; }
            }
            ctx->last_error = 0x5AA; // ERROR_NO_MORE_ITEMS-ish
            *out = 0xFFFFFFFFu;
            return 1;
        }
        if (sym_eq(symbol, "TlsFree")) {
            uint32_t i = args[0];
            if (i < 64) { ctx->regs.tls_used[i] = 0; ctx->regs.tls_slot[i] = 0; *out = 1; }
            else *out = 0;
            return 1;
        }
        if (sym_eq(symbol, "TlsGetValue")) {
            uint32_t i = args[0];
            *out = (i < 64) ? ctx->regs.tls_slot[i] : 0;
            return 1;
        }
        if (sym_eq(symbol, "TlsSetValue")) {
            uint32_t i = args[0];
            if (i < 64) { ctx->regs.tls_slot[i] = args[1]; *out = 1; }
            else { ctx->last_error = 87; *out = 0; }
            return 1;
        }
        // Interlocked*: the guest is single-threaded here, so a plain RMW is
        // equivalent to the atomic form.
        if (symbol[0] == 'I' && ctx->mem.base) {
            uint32_t* p = (uint32_t*)(void*)(ctx->mem.base + args[0]);
            if (sym_eq(symbol, "InterlockedIncrement")) { *out = ++*p; return 1; }
            if (sym_eq(symbol, "InterlockedDecrement")) { *out = --*p; return 1; }
            if (sym_eq(symbol, "InterlockedExchange")) { uint32_t o = *p; *p = args[1]; *out = o; return 1; }
            if (sym_eq(symbol, "InterlockedExchangeAdd")) { uint32_t o = *p; *p += args[1]; *out = o; return 1; }
            if (sym_eq(symbol, "InterlockedAdd")) { *p += args[1]; *out = *p; return 1; }
            if (sym_eq(symbol, "InterlockedCompareExchange")) {
                uint32_t o = *p;
                if (o == args[2]) *p = args[1];
                *out = o;
                return 1;
            }
            if (sym_eq(symbol, "InterlockedOr")) { uint32_t o = *p; *p |= args[1]; *out = o; return 1; }
            if (sym_eq(symbol, "InterlockedAnd")) { uint32_t o = *p; *p &= args[1]; *out = o; return 1; }
            if (sym_eq(symbol, "InterlockedXor")) { uint32_t o = *p; *p ^= args[1]; *out = o; return 1; }
        }
    }
    return 0;
}

extern "C" void* trt_pe32_trap_host_fn(int index);

// Dispatch one imported call. trap_index is -1 when the caller already knows
// the module/symbol (direct call path).
extern "C" uint32_t x86_dispatch_import(x86_context* ctx, int trap_index,
                                        const char* module, const char* symbol,
                                        int* out_nargs, int* out_cdecl) {
    uint32_t args[16];
    uint32_t esp = ctx->regs.esp + 4; // skip return address
    for (int i = 0; i < 16; i++) args[i] = x86_mem_read32(ctx, esp + i * 4);

    int cdecl_call = lookup_cdecl(module, symbol);
    int nargs = lookup_arg_count(module, symbol);
    if (nargs < 0) nargs = 4; // default for unknown

    uint32_t result = 0;
    if (special_import(ctx, module, symbol, args, &result)) {
        *out_nargs = nargs;
        *out_cdecl = cdecl_call;
        return result;
    }

    void* fn = (trap_index >= 0) ? trt_pe32_trap_host_fn(trap_index) : nullptr;
    if (!fn && symbol && symbol[0]) fn = trt_resolve_symbol(module, symbol);
    if (!fn) {
        char buf[256];
        snprintf(buf, sizeof(buf), "x86_bridge: unresolved %s!%s -> returning 0", module, symbol);
        if (ctx->log) ctx->log(buf);
        *out_nargs = nargs;
        *out_cdecl = cdecl_call;
        return 0;
    }
    result = x86_call_fn(ctx, fn, module, symbol, args, nargs);
    *out_nargs = nargs;
    *out_cdecl = cdecl_call;
    return result;
}

// Legacy entry point kept for direct callers.
extern "C" uint32_t x86_import_thunk_handler(x86_context* ctx, const char* module,
                                             const char* symbol) {
    uint32_t args[16];
    uint32_t esp = ctx->regs.esp + 4;
    for (int i = 0; i < 16; i++) args[i] = x86_mem_read32(ctx, esp + i * 4);
    int nargs = lookup_arg_count(module, symbol);
    if (nargs < 0) nargs = 4;
    return x86_call_bridge(ctx, module, symbol, args, nargs);
}
