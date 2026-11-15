// tiramisu/src/trt/apiset.cpp
// API-set contract -> real DLL mapping (the Windows apiset schema, trimmed
// to what PC games actually import).
//
// A game built after ~2015 imports contract names:
//     api-ms-win-core-synch-l1-2-0.dll!WaitOnAddress
//     api-ms-win-core-winrt-l1-1-0.dll!WindowsCreateString
//     api-ms-win-crt-heap-l1-1-0.dll!_callnewh
// On desktop the OS loader resolves those through its apiset schema. We have
// no schema on the console, so the mapping is explicit here.
//
// Rules for this table:
//   - kernel32-family contracts -> kernel32.dll (our BUILTIN surface first,
//     then the console's real kernel32 as fallback)
//   - winrt/com contracts        -> combase.dll (console real)
//   - crt contracts              -> ucrtbase.dll (console real)
//   - graphics contracts         -> the real d3d11/dxgi/gdi32 on the console
// Unknown contracts are NOT guessed: the resolver falls through to the
// console's own LoadLibrary, which fails visibly if the contract is unknown
// there too.
#include "apiset.h"

#include <string.h>

namespace {

struct apiset_rule {
  const char* family;   // contract family, e.g. "api-ms-win-core-synch"
  const char* target;   // real DLL that implements it
};

// Longest-prefix wins at lookup time, so specific families must precede the
// generic ones they extend (e.g. winrt-registry before winrt).
const apiset_rule k_rules[] = {
    // ---- WinRT / COM ----
    {"api-ms-win-core-winrt-registry", "advapi32.dll"},
    {"api-ms-win-core-winrt-string", "combase.dll"},
    {"api-ms-win-core-winrt-error", "combase.dll"},
    {"api-ms-win-core-winrt-notification", "combase.dll"},
    {"api-ms-win-core-winrt-threadpool", "combase.dll"},
    {"api-ms-win-core-winrt-hstring", "combase.dll"},
    {"api-ms-win-core-winrt-object", "combase.dll"},
    {"api-ms-win-core-winrt-async", "combase.dll"},
    {"api-ms-win-core-winrt-activation", "combase.dll"},
    {"api-ms-win-core-winrt-threading", "combase.dll"},
    {"api-ms-win-core-winrt", "combase.dll"},
    {"api-ms-win-core-com", "combase.dll"},
    {"api-ms-win-core-marshal", "combase.dll"},
    {"api-ms-win-core-appmodel", "combase.dll"},
    {"api-ms-win-core-weakref", "combase.dll"},
    {"api-ms-win-com", "combase.dll"},
    // ---- kernel32 families ----
    {"api-ms-win-core-synch", "kernel32.dll"},
    {"api-ms-win-core-interlocked", "kernel32.dll"},
    {"api-ms-win-core-processthreads", "kernel32.dll"},
    {"api-ms-win-core-file", "kernel32.dll"},
    {"api-ms-win-core-heap", "kernel32.dll"},
    {"api-ms-win-core-memory", "kernel32.dll"},
    {"api-ms-win-core-string", "kernel32.dll"},
    {"api-ms-win-core-sysinfo", "kernel32.dll"},
    {"api-ms-win-core-systemtopology", "kernel32.dll"},
    {"api-ms-win-core-timezone", "kernel32.dll"},
    {"api-ms-win-core-datetime", "kernel32.dll"},
    {"api-ms-win-core-util", "kernel32.dll"},
    {"api-ms-win-core-localization", "kernel32.dll"},
    {"api-ms-win-core-wow64", "kernel32.dll"},
    {"api-ms-win-core-firmware", "kernel32.dll"},
    {"api-ms-win-core-comm", "kernel32.dll"},
    {"api-ms-win-core-job", "kernel32.dll"},
    {"api-ms-win-core-psapi", "kernel32.dll"},
    {"api-ms-win-core-realtime", "kernel32.dll"},
    {"api-ms-win-core-debug", "kernel32.dll"},
    {"api-ms-win-core-multiprocess", "kernel32.dll"},
    {"api-ms-win-core-namedpipe", "kernel32.dll"},
    {"api-ms-win-core-sharedmemory", "kernel32.dll"},
    {"api-ms-win-core-console", "kernel32.dll"},
    {"api-ms-win-core-fibers", "kernel32.dll"},
    {"api-ms-win-core-machinelanguage", "kernel32.dll"},
    {"api-ms-win-core-virtualalloc", "kernel32.dll"},
    {"api-ms-win-core-virtualquery", "kernel32.dll"},
    {"api-ms-win-core-profile", "kernel32.dll"},
    {"api-ms-win-core-libraryloader", "kernel32.dll"},
    {"api-ms-win-core-xact", "kernel32.dll"},
    {"api-ms-win-core-errorcodehelper", "kernel32.dll"},
    {"api-ms-win-appmodel-runtime", "kernel32.dll"},
    // STAGE 7 (corpus-run DLL phase regression, root-caused): dstorage.dll's
    // DllMain now RUNS (image_code_run=1) and imports GetCommandLineW via
    // api-ms-win-core-processenvironment-l1-1-0.dll — a family that was NOT
    // in this table, so the import never reached the kernel32 BUILTIN surface
    // (which has a real answer) and fell to the blanket-0 SYSTEM bind; the
    // guest derived a struct from the NULL answer and SIGSEGV'd. These are
    // the contract families the whole run corpus (gonext, gow, ortmm) imports
    // that had NO exact rule AND no covering prefix rule.
    {"api-ms-win-core-errorhandling", "kernel32.dll"},
    {"api-ms-win-core-handle", "kernel32.dll"},
    {"api-ms-win-core-io", "kernel32.dll"},
    {"api-ms-win-core-processenvironment", "kernel32.dll"},
    {"api-ms-win-core-registry", "advapi32.dll"},
    {"api-ms-win-core-threadpool", "kernel32.dll"},
    {"api-ms-win-core-versionprivate", "version.dll"},
    {"api-ms-win-core-version", "version.dll"},
    {"api-ms-win-core-path", "kernelbase.dll"},
    {"api-ms-win-core-kernel32-legacy", "kernel32.dll"},
    {"api-ms-win-core-kernel32-private", "kernel32.dll"},
    {"api-ms-win-core-ioring", "kernelbase.dll"},
    {"api-ms-win-core-file-fromapp", "kernelbase.dll"},
    {"api-ms-win-core-windowserrorreporting", "kernel32.dll"},
    {"api-ms-win-core-apiquery", "kernelbase.dll"},
    {"api-ms-win-core-delayload", "kernel32.dll"},
    {"api-ms-win-core-xstate", "kernel32.dll"},
    {"api-ms-win-core-psm-appnotify", "kernel32.dll"},
    {"api-ms-win-core-enclave", "kernel32.dll"},
    {"api-ms-win-core-appcompat", "kernel32.dll"},
    {"api-ms-win-core-namespace", "kernel32.dll"},
    {"api-ms-win-core-atoms", "kernel32.dll"},
    {"api-ms-win-security-lsalookup", "advapi32.dll"},
    {"api-ms-win-security-sddl", "advapi32.dll"},
    {"api-ms-win-security-provider", "advapi32.dll"},
    {"api-ms-win-security-credentials", "advapi32.dll"},
    {"api-ms-win-security-cryptoapi", "advapi32.dll"},
    {"api-ms-win-security-audit", "advapi32.dll"},
    {"api-ms-win-security-lsapolicy", "advapi32.dll"},
    {"api-ms-win-power", "powrprof.dll"},
    {"api-ms-win-shell", "shell32.dll"},
    {"api-ms-win-shlwapi", "shlwapi.dll"},
    // ---- ext-ms-win-* extension contracts (Unreal/Chromium/Qt/PC games) ----
    {"ext-ms-win-ntuser", "user32.dll"},
    {"ext-ms-win-rtcore-ntuser", "user32.dll"},
    {"ext-ms-win-rtcore-gdi", "gdi32.dll"},
    {"ext-ms-win-gdi", "gdi32.dll"},
    {"ext-ms-win-kernel32", "kernel32.dll"},
    {"ext-ms-win-core", "kernel32.dll"},
    {"ext-ms-win-com", "ole32.dll"},
    {"ext-ms-win-shell", "shell32.dll"},
    {"ext-ms-win-advapi32", "advapi32.dll"},
    {"ext-ms-win-uxtheme", "uxtheme.dll"},
    {"ext-ms-win-dwmapi", "dwmapi.dll"},
    {"ext-ms-win-dx", "d3d11.dll"},
    {"ext-ms-win-session", "wtsapi32.dll"},
    {"ext-ms-win-security", "advapi32.dll"},
    {"api-ms-win-core-winrt-robuffer", "combase.dll"},
    {"api-ms-win-eventing-provider", "ntdll.dll"},
    {"api-ms-win-shcore-scaling", "shcore.dll"},
    {"api-ms-win-core-nt", "ntdll.dll"},
    {"api-ms-win-core-rtlsupport", "ntdll.dll"},
    {"api-ms-win-downlevel-kernel32", "kernel32.dll"},
    {"api-ms-win-downlevel-profile", "kernel32.dll"},
    {"api-ms-win-downlevel-shcore", "shcore.dll"},

    // ---- security / service ----
    {"api-ms-win-security-basecrypt", "bcrypt.dll"},
    {"api-ms-win-security-cryptui", "cryptui.dll"},
    {"api-ms-win-security-sspi", "secur32.dll"},
    // STAGE 7 (dstoragecore): its obfuscation runtime lazy-loads
    // api-ms-win-security-systemfunctions-l1-1-0 for advapi32's
    // SystemFunction036 (RtlGenRandom). On real Windows the contract
    // resolves to advapi32; here it must reach the advapi32 bridge.
    {"api-ms-win-security-systemfunctions", "advapi32.dll"},
    {"api-ms-win-service-core", "sechost.dll"},
    {"api-ms-win-service", "sechost.dll"},

    // ---- graphics / media / user ----
    {"api-ms-win-dxgi", "dxgi.dll"},
    {"api-ms-win-d3d11", "d3d11.dll"},
    {"api-ms-win-d3d10", "d3d11.dll"},
    {"api-ms-win-d3d12", "d3d12.dll"},
    {"api-ms-win-gdi", "gdi32.dll"},
    {"api-ms-win-dinput", "dinput8.dll"},
    {"api-ms-win-xinput", "xinput1_4.dll"},
    {"api-ms-win-mm", "winmm.dll"},
    {"api-ms-win-ole", "ole32.dll"},
    {"api-ms-win-shcore", "shcore.dll"},
    {"api-ms-win-user", "user32.dll"},

    // ---- CRT ----
    {"api-ms-win-crt", "ucrtbase.dll"},
    {"api-ms-win-core-crt", "ucrtbase.dll"},
};

size_t lower_ascii(char c) {
  return (c >= 'A' && c <= 'Z') ? (size_t)(c - 'A' + 'a') : (size_t)(unsigned char)c;
}

bool ieq(const char* a, const char* b) {
  while (*a && *b) {
    if (lower_ascii(*a) != lower_ascii(*b)) return false;
    ++a; ++b;
  }
  return *a == *b;
}

// portable case-insensitive prefix test (no strncasecmp: this file builds
// for the console host too, where that name does not exist)
bool starts_with_ci(const char* s, const char* prefix) {
  while (*prefix) {
    if (!*s) return false;
    if (lower_ascii(*s) != lower_ascii(*prefix)) return false;
    ++s; ++prefix;
  }
  return true;
}

// "api-ms-win-core-synch-l1-2-0.dll" -> "api-ms-win-core-synch"
void family_of(const char* name, char* out, size_t cap) {
  size_t n = 0;
  while (name[n] && name[n] != '.' && n + 1 < cap) { out[n] = name[n]; ++n; }
  out[n] = 0;
  // cut the version tail: first "-l<digit>"
  for (size_t i = 1; i + 2 < n; ++i) {
    if (out[i] == '-' && out[i + 1] == 'l' && out[i + 2] >= '0' && out[i + 2] <= '9') {
      out[i] = 0;
      return;
    }
  }
}

} // namespace

extern "C" {

int trt_apiset_is_contract(const char* name) {
  if (!name) return 0;
  auto lc = [](char c) -> char {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
  };
  const char c0 = lc(name[0]), c1 = lc(name[1]), c2 = lc(name[2]);
  const char c3 = lc(name[3]), c4 = lc(name[4]), c5 = lc(name[5]);
  return ((c0 == 'a' && c1 == 'p' && c2 == 'i') ||
          (c0 == 'e' && c1 == 'x' && c2 == 't')) &&
         c3 == '-' && c4 == 'm' && c5 == 's';
}

const char* trt_apiset_target(const char* contract_name) {
  if (!trt_apiset_is_contract(contract_name)) return nullptr;
  char fam[128];
  family_of(contract_name, fam, sizeof(fam));

  // 1) exact family match wins
  for (size_t i = 0; i < sizeof(k_rules) / sizeof(k_rules[0]); ++i)
    if (ieq(k_rules[i].family, fam)) return k_rules[i].target;

  // 2) longest prefix match (covers families we did not enumerate)
  const char* best = nullptr;
  size_t best_len = 0;
  for (size_t i = 0; i < sizeof(k_rules) / sizeof(k_rules[0]); ++i) {
    const size_t len = strlen(k_rules[i].family);
    if (len > best_len && starts_with_ci(fam, k_rules[i].family)) {
      best = k_rules[i].target;
      best_len = len;
    }
  }
  return best;
}

} // extern "C"
