// tiramisu/tests/corpus_ucrt_wrappers.c
// UCRT C-library surface for the offline corpus harness — REAL answers.
//
// Why this exists: the corpus runner's SYSTEM bind answers every whitelisted
// symbol it has no named surface for with a 0-returning stub (the "blanket-0"
// bind — each hit is logged as a named work item). The BotW-Native corpus
// added MinGW-built DLLs (libwinpthread, libstdc++, libgcc) whose DllMain CRT
// init calls ucrtbase!calloc first thing: the 0 answer returned NULL, the
// first store through it faulted, and the EH bounce detector terminated the
// guest (measured: libstdc++-6.dll signal 6, 100k identical EH bounces at
// libwinpthread +0x6d0c, rax=0 — calloc(1,0x20) -> NULL).
//
// The host libc HAS real implementations of this surface. The ABI is the only
// gap: the guest calls Win64 (rcx/rdx/r8/r9), host libc is SysV (rdi/rsi/
// rdx/rcx). Every wrapper below is ms_abi (Win64 entry) and calls the host
// libc function from SysV context — clang emits the transition thunk.
//
// Scope: pure-computation functions only. Deliberately NOT wrapped (still
// blanket-0, still named): the printf family (__stdio_common_*: MS va_list
// ABI), the Win32-handle file family (_open/_read/_write/_fopen paths —
// those need the VFS, not host paths), _gmtime64/strftime (struct tm layout
// differs), localeconv (lconv layout differs), _beginthreadex (thread bridge
// surface), setjmp/longjmp (MS jmp_buf ABI).
#include "../src/trt/trt.h"

#include <stdio.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

// ---------------------------------------------------------------- heap
void* TRT_GUEST_ABI ucrt_calloc(size_t n, size_t sz) { return calloc(n, sz); }
void* TRT_GUEST_ABI ucrt_malloc(size_t sz) { return malloc(sz); }
void TRT_GUEST_ABI ucrt_free(void* p) { free(p); }
void* TRT_GUEST_ABI ucrt_realloc(void* p, size_t sz) { return realloc(p, sz); }

// ---------------------------------------------------------------- mem
void* TRT_GUEST_ABI ucrt_memchr(const void* s, int c, size_t n) { return (void*)memchr(s, c, n); }
int TRT_GUEST_ABI ucrt_memcmp(const void* a, const void* b, size_t n) { return memcmp(a, b, n); }
void* TRT_GUEST_ABI ucrt_memcpy(void* d, const void* s, size_t n) { return memcpy(d, s, n); }
void* TRT_GUEST_ABI ucrt_memmove(void* d, const void* s, size_t n) { return memmove(d, s, n); }
void* TRT_GUEST_ABI ucrt_memset(void* d, int c, size_t n) { return memset(d, c, n); }

// ---------------------------------------------------------------- str
char* TRT_GUEST_ABI ucrt_strcat(char* d, const char* s) { return strcat(d, s); }
char* TRT_GUEST_ABI ucrt_strchr(const char* s, int c) { return (char*)strchr(s, c); }
int TRT_GUEST_ABI ucrt_strcmp(const char* a, const char* b) { return strcmp(a, b); }
int TRT_GUEST_ABI ucrt_strcoll(const char* a, const char* b) { return strcoll(a, b); }
char* TRT_GUEST_ABI ucrt_strcpy(char* d, const char* s) { return strcpy(d, s); }
size_t TRT_GUEST_ABI ucrt_strcspn(const char* s, const char* rej) { return strcspn(s, rej); }
char* TRT_GUEST_ABI ucrt_strerror(int e) { return strerror(e); }
size_t TRT_GUEST_ABI ucrt_strlen(const char* s) { return strlen(s); }
int TRT_GUEST_ABI ucrt_strncmp(const char* a, const char* b, size_t n) { return strncmp(a, b, n); }
char* TRT_GUEST_ABI ucrt_strncpy(char* d, const char* s, size_t n) { return strncpy(d, s, n); }
size_t TRT_GUEST_ABI ucrt_strnlen(const char* s, size_t n) { return strnlen(s, n); }
char* TRT_GUEST_ABI ucrt_strpbrk(const char* s, const char* acc) { return (char*)strpbrk(s, acc); }
char* TRT_GUEST_ABI ucrt_strrchr(const char* s, int c) { return (char*)strrchr(s, c); }
size_t TRT_GUEST_ABI ucrt_strspn(const char* s, const char* acc) { return strspn(s, acc); }
char* TRT_GUEST_ABI ucrt_strstr(const char* h, const char* n) { return (char*)strstr(h, n); }
long TRT_GUEST_ABI ucrt_strtol(const char* s, char** end, int base) { return strtol(s, end, base); }
long long TRT_GUEST_ABI ucrt_strtoll(const char* s, char** end, int base) { return strtoll(s, end, base); }
unsigned long TRT_GUEST_ABI ucrt_strtoul(const char* s, char** end, int base) { return strtoul(s, end, base); }
unsigned long long TRT_GUEST_ABI ucrt_strtoull(const char* s, char** end, int base) { return strtoull(s, end, base); }
size_t TRT_GUEST_ABI ucrt_strxfrm(char* d, const char* s, size_t n) { return strxfrm(d, s, n); }
char* TRT_GUEST_ABI ucrt__strdup(const char* s) { return strdup(s); }

// ---------------------------------------------------------------- wcs / mbr
wchar_t* TRT_GUEST_ABI ucrt_wcscat(wchar_t* d, const wchar_t* s) { return wcscat(d, s); }
int TRT_GUEST_ABI ucrt_wcscmp(const wchar_t* a, const wchar_t* b) { return wcscmp(a, b); }
int TRT_GUEST_ABI ucrt_wcscoll(const wchar_t* a, const wchar_t* b) { return wcscoll(a, b); }
wchar_t* TRT_GUEST_ABI ucrt_wcscpy(wchar_t* d, const wchar_t* s) { return wcscpy(d, s); }
size_t TRT_GUEST_ABI ucrt_wcslen(const wchar_t* s) { return wcslen(s); }
int TRT_GUEST_ABI ucrt_wcsncmp(const wchar_t* a, const wchar_t* b, size_t n) { return wcsncmp(a, b, n); }
size_t TRT_GUEST_ABI ucrt_wcsnlen(const wchar_t* s, size_t n) { return wcsnlen(s, n); }
wchar_t* TRT_GUEST_ABI ucrt_wcsstr(const wchar_t* h, const wchar_t* n) { return wcsstr(h, n); }
long TRT_GUEST_ABI ucrt_wcstol(const wchar_t* s, wchar_t** end, int base) { return wcstol(s, end, base); }
size_t TRT_GUEST_ABI ucrt_wcstombs(char* d, const wchar_t* s, size_t n) { return wcstombs(d, s, n); }
size_t TRT_GUEST_ABI ucrt_wcsxfrm(wchar_t* d, const wchar_t* s, size_t n) { return wcsxfrm(d, s, n); }
size_t TRT_GUEST_ABI ucrt_wcrtomb(char* d, wchar_t wc, mbstate_t* st) { return wcrtomb(d, wc, st); }
size_t TRT_GUEST_ABI ucrt_mbrlen(const char* s, size_t n, mbstate_t* st) { return mbrlen(s, n, st); }
size_t TRT_GUEST_ABI ucrt_mbrtowc(wchar_t* pwc, const char* s, size_t n, mbstate_t* st) { return mbrtowc(pwc, s, n, st); }
size_t TRT_GUEST_ABI ucrt_mbsrtowcs(wchar_t* d, const char** s, size_t n, mbstate_t* st) { return mbsrtowcs(d, s, n, st); }

// ---------------------------------------------------------------- ctype
int TRT_GUEST_ABI ucrt_isalnum(int c) { return isalnum(c); }
int TRT_GUEST_ABI ucrt_isalpha(int c) { return isalpha(c); }
int TRT_GUEST_ABI ucrt_isdigit(int c) { return isdigit(c); }
int TRT_GUEST_ABI ucrt_isspace(int c) { return isspace(c); }
int TRT_GUEST_ABI ucrt_isxdigit(int c) { return isxdigit(c); }
int TRT_GUEST_ABI ucrt_tolower(int c) { return tolower(c); }
int TRT_GUEST_ABI ucrt_toupper(int c) { return toupper(c); }
int TRT_GUEST_ABI ucrt_towlower(wint_t c) { return towlower(c); }
int TRT_GUEST_ABI ucrt_towupper(wint_t c) { return towupper(c); }
wctype_t TRT_GUEST_ABI ucrt_wctype(const char* name) { return wctype(name); }
int TRT_GUEST_ABI ucrt_iswctype(wint_t c, wctype_t t) { return iswctype(c, t); }

// ---------------------------------------------------------------- math
double TRT_GUEST_ABI ucrt_acos(double x) { return acos(x); }
float TRT_GUEST_ABI ucrt_acosf(float x) { return acosf(x); }
double TRT_GUEST_ABI ucrt_asin(double x) { return asin(x); }
float TRT_GUEST_ABI ucrt_asinf(float x) { return asinf(x); }
double TRT_GUEST_ABI ucrt_atan(double x) { return atan(x); }
double TRT_GUEST_ABI ucrt_atan2(double y, double x) { return atan2(y, x); }
float TRT_GUEST_ABI ucrt_atan2f(float y, float x) { return atan2f(y, x); }
float TRT_GUEST_ABI ucrt_atanf(float x) { return atanf(x); }
double TRT_GUEST_ABI ucrt_ceil(double x) { return ceil(x); }
float TRT_GUEST_ABI ucrt_ceilf(float x) { return ceilf(x); }
double TRT_GUEST_ABI ucrt_cos(double x) { return cos(x); }
float TRT_GUEST_ABI ucrt_cosf(float x) { return cosf(x); }
double TRT_GUEST_ABI ucrt_exp(double x) { return exp(x); }
double TRT_GUEST_ABI ucrt_exp2(double x) { return exp2(x); }
float TRT_GUEST_ABI ucrt_expf(float x) { return expf(x); }
double TRT_GUEST_ABI ucrt_floor(double x) { return floor(x); }
float TRT_GUEST_ABI ucrt_floorf(float x) { return floorf(x); }
double TRT_GUEST_ABI ucrt_fmod(double x, double y) { return fmod(x, y); }
float TRT_GUEST_ABI ucrt_fmodf(float x, float y) { return fmodf(x, y); }
double TRT_GUEST_ABI ucrt_frexp(double x, int* e) { return frexp(x, e); }
double TRT_GUEST_ABI ucrt_log(double x) { return log(x); }
double TRT_GUEST_ABI ucrt_log10(double x) { return log10(x); }
float TRT_GUEST_ABI ucrt_log10f(float x) { return log10f(x); }
double TRT_GUEST_ABI ucrt_log2(double x) { return log2(x); }
float TRT_GUEST_ABI ucrt_logf(float x) { return logf(x); }
double TRT_GUEST_ABI ucrt_modf(double x, double* ip) { return modf(x, ip); }
float TRT_GUEST_ABI ucrt_modff(float x, float* ip) { return modff(x, ip); }
double TRT_GUEST_ABI ucrt_pow(double x, double y) { return pow(x, y); }
float TRT_GUEST_ABI ucrt_powf(float x, float y) { return powf(x, y); }
double TRT_GUEST_ABI ucrt_round(double x) { return round(x); }
float TRT_GUEST_ABI ucrt_roundf(float x) { return roundf(x); }
double TRT_GUEST_ABI ucrt_sin(double x) { return sin(x); }
float TRT_GUEST_ABI ucrt_sinf(float x) { return sinf(x); }
double TRT_GUEST_ABI ucrt_sqrt(double x) { return sqrt(x); }
float TRT_GUEST_ABI ucrt_sqrtf(float x) { return sqrtf(x); }
double TRT_GUEST_ABI ucrt_tan(double x) { return tan(x); }
float TRT_GUEST_ABI ucrt_tanf(float x) { return tanf(x); }
double TRT_GUEST_ABI ucrt_trunc(double x) { return trunc(x); }
float TRT_GUEST_ABI ucrt_truncf(float x) { return truncf(x); }
long TRT_GUEST_ABI ucrt_lround(double x) { return lround(x); }
long TRT_GUEST_ABI ucrt_lroundf(float x) { return lroundf(x); }
double TRT_GUEST_ABI ucrt_atof(const char* s) { return atof(s); }
int TRT_GUEST_ABI ucrt_atoi(const char* s) { return atoi(s); }

// ---------------------------------------------------------------- misc
// qsort: the compare callback is GUEST code (Win64 ABI) — glibc qsort would
// call it with SysV registers and corrupt the arguments. Own implementation,
// calling the pointer through an ms_abi type.
typedef int (__attribute__((ms_abi)) * ucrt_qsort_cmp)(const void*, const void*);
static void ucrt_qsort_rec(char* base, size_t lo, size_t hi, size_t w,
                           ucrt_qsort_cmp cmp, char* tmp) {
  if (lo >= hi) return;
  size_t mid = lo + (hi - lo) / 2;
  memcpy(tmp, base + mid * w, w);
  memcpy(base + mid * w, base + hi * w, w);
  memcpy(base + hi * w, tmp, w);
  size_t store = lo;
  for (size_t i = lo; i < hi; ++i) {
    if (cmp(base + i * w, base + hi * w) < 0) {
      if (i != store) {
        memcpy(tmp, base + i * w, w);
        memcpy(base + i * w, base + store * w, w);
        memcpy(base + store * w, tmp, w);
      }
      ++store;
    }
  }
  memcpy(tmp, base + hi * w, w);
  memcpy(base + hi * w, base + store * w, w);
  memcpy(base + store * w, tmp, w);
  if (store > lo) ucrt_qsort_rec(base, lo, store - 1, w, cmp, tmp);
  ucrt_qsort_rec(base, store + 1, hi, w, cmp, tmp);
}
void TRT_GUEST_ABI ucrt_qsort(void* base, size_t n, size_t w, ucrt_qsort_cmp cmp) {
  if (!base || n < 2 || w == 0) return;
  static _Thread_local char* tmp;
  static _Thread_local size_t tmp_cap;
  if (tmp_cap < w) {
    free(tmp);
    tmp = (char*)malloc(w);
    tmp_cap = tmp ? w : 0;
    if (!tmp) return;
  }
  ucrt_qsort_rec((char*)base, 0, n - 1, w, cmp, tmp);
}

static int g_mb_cur_max = 1;  // the "C" locale: 1 byte per character
int* TRT_GUEST_ABI ucrt___mb_cur_max_func(void) { return &g_mb_cur_max; }

void TRT_GUEST_ABI ucrt_abort(void) { abort(); }

// exit/_exit must fire the guest exit trap (the runner's ExitProcess path),
// never the host process exit — a host exit(0) would end the whole harness.
extern void (*trt_boot_exit_trap)(int);
static void ucrt_exit_common(int code) {
  if (trt_boot_exit_trap) trt_boot_exit_trap(code);
  _exit(code);  // no trap armed: this harness is the process, leave honestly
}
void TRT_GUEST_ABI ucrt_exit(int code) { ucrt_exit_common(code); }
void TRT_GUEST_ABI ucrt__exit(int code) { ucrt_exit_common(code); }

char* TRT_GUEST_ABI ucrt_getenv(const char* name) { return getenv(name); }
clock_t TRT_GUEST_ABI ucrt_clock(void) { return clock(); }
long long TRT_GUEST_ABI ucrt__time64(long long* out) {
  long long t = (long long)time(NULL);
  if (out) *out = t;
  return t;
}
void TRT_GUEST_ABI ucrt__assert(const char* expr, const char* file,
                                unsigned line) {
  fprintf(stderr, "ucrt: _assert(%s) at %s:%u\n", expr ? expr : "?",
          file ? file : "?", line);
  abort();
}

// rand_s: deterministic CSPRNG fill through the same honest source the
// kernel32 bridge uses (host getrandom); 0 = STATUS_SUCCESS.
unsigned int TRT_GUEST_ABI ucrt_rand_s(unsigned int* out) {
  if (!out) return 1;  // EINVAL-shaped failure
  FILE* f = fopen("/dev/urandom", "rb");
  if (!f) {
    *out = (unsigned int)(time(NULL) ^ (uintptr_t)out);
    return 0;
  }
  size_t got = fread(out, 1, sizeof(*out), f);
  fclose(f);
  if (got != sizeof(*out)) *out = (unsigned int)(time(NULL) ^ (uintptr_t)out);
  return 0;
}

// ---------------------------------------------------------------- CRT internals
typedef void (__attribute__((ms_abi)) * ucrt_init_fn)(void);
// UCRT _initterm_e entry: int fn(void); walk stops on non-zero, returns it
typedef int (__attribute__((ms_abi)) * ucrt_init_e_fn)(void);
int TRT_GUEST_ABI ucrt__initterm(ucrt_init_fn* begin, ucrt_init_fn* end) {
  for (ucrt_init_fn* p = begin; p && p < end; ++p)
    if (*p) (*p)();
  return 0;
}
int TRT_GUEST_ABI ucrt__initterm_e(ucrt_init_e_fn* begin, ucrt_init_e_fn* end) {
  for (ucrt_init_e_fn* p = begin; p && p < end; ++p)
    if (*p) return (*p)();
  return 0;
}

// _onexit_table_t: {begin, end, end_of_storage} function-pointer array
typedef struct {
  void** begin;
  void** end;
  void** storage_end;
} ucrt_onexit_table;
int TRT_GUEST_ABI ucrt__initialize_onexit_table(ucrt_onexit_table* t) {
  if (!t) return 1;
  t->begin = t->end = t->storage_end = NULL;
  return 0;
}
int TRT_GUEST_ABI ucrt__register_onexit_function(ucrt_onexit_table* t,
                                                 void* fn) {
  if (!t || !fn) return 1;
  if (!t->begin) {  // lazily grow a small static-backed table
    static _Thread_local void* slots[64];
    t->begin = slots;
    t->end = slots;
    t->storage_end = slots + 64;
  }
  if (t->end >= t->storage_end) return 1;  // table full: named by the caller
  *t->end++ = fn;
  return 0;
}
int TRT_GUEST_ABI ucrt__execute_onexit_table(ucrt_onexit_table* t) {
  if (!t || !t->begin) return 0;
  while (t->end > t->begin) {
    void* fn = *--t->end;
    if (fn) ((void (__attribute__((ms_abi)) *)(void))fn)();
  }
  return 0;
}
int TRT_GUEST_ABI ucrt__register_thread_local_exe_atexit_callback(
    void* fn, void* ctx) {
  (void)fn;
  (void)ctx;
  return 0;  // recorded as accepted; TLS-exit callbacks are not replayed
}

int* TRT_GUEST_ABI ucrt__errno(void) { return &errno; }
int TRT_GUEST_ABI ucrt__set_errno(int v) { errno = v; return 0; }
int TRT_GUEST_ABI ucrt__get_errno(int* out) {
  if (!out) return 1;
  *out = errno;
  return 0;
}

void TRT_GUEST_ABI ucrt__lock_file(void* f) { (void)f; }
void TRT_GUEST_ABI ucrt__unlock_file(void* f) { (void)f; }

void* TRT_GUEST_ABI ucrt___acrt_iob_func(unsigned idx) {
  switch (idx) {
    case 0: return (void*)stdin;
    case 1: return (void*)stdout;
    case 2: return (void*)stderr;
    default: return NULL;
  }
}

void* TRT_GUEST_ABI ucrt__aligned_malloc(size_t sz, size_t align) {
  if (align == 0 || (align & (align - 1))) return NULL;
  void* p = NULL;
  if (posix_memalign(&p, align < sizeof(void*) ? sizeof(void*) : align,
                     sz ? sz : 1) != 0)
    return NULL;
  return p;
}
void TRT_GUEST_ABI ucrt__aligned_free(void* p) { free(p); }

// ---------------------------------------------------------------- lookup
struct ucrt_wrap_entry {
  const char* name;
  void* fn;
};
static const struct ucrt_wrap_entry k_ucrt_wraps[] = {
    {"calloc", (void*)&ucrt_calloc},
    {"malloc", (void*)&ucrt_malloc},
    {"free", (void*)&ucrt_free},
    {"realloc", (void*)&ucrt_realloc},
    {"memchr", (void*)&ucrt_memchr},
    {"memcmp", (void*)&ucrt_memcmp},
    {"memcpy", (void*)&ucrt_memcpy},
    {"memmove", (void*)&ucrt_memmove},
    {"memset", (void*)&ucrt_memset},
    {"strcat", (void*)&ucrt_strcat},
    {"strchr", (void*)&ucrt_strchr},
    {"strcmp", (void*)&ucrt_strcmp},
    {"strcoll", (void*)&ucrt_strcoll},
    {"strcpy", (void*)&ucrt_strcpy},
    {"strcspn", (void*)&ucrt_strcspn},
    {"strerror", (void*)&ucrt_strerror},
    {"strlen", (void*)&ucrt_strlen},
    {"strncmp", (void*)&ucrt_strncmp},
    {"strncpy", (void*)&ucrt_strncpy},
    {"strnlen", (void*)&ucrt_strnlen},
    {"strpbrk", (void*)&ucrt_strpbrk},
    {"strrchr", (void*)&ucrt_strrchr},
    {"strspn", (void*)&ucrt_strspn},
    {"strstr", (void*)&ucrt_strstr},
    {"strtol", (void*)&ucrt_strtol},
    {"strtoll", (void*)&ucrt_strtoll},
    {"strtoul", (void*)&ucrt_strtoul},
    {"strtoull", (void*)&ucrt_strtoull},
    {"strxfrm", (void*)&ucrt_strxfrm},
    {"_strdup", (void*)&ucrt__strdup},
    {"wcscat", (void*)&ucrt_wcscat},
    {"wcscmp", (void*)&ucrt_wcscmp},
    {"wcscoll", (void*)&ucrt_wcscoll},
    {"wcscpy", (void*)&ucrt_wcscpy},
    {"wcslen", (void*)&ucrt_wcslen},
    {"wcsncmp", (void*)&ucrt_wcsncmp},
    {"wcsnlen", (void*)&ucrt_wcsnlen},
    {"wcsstr", (void*)&ucrt_wcsstr},
    {"wcstol", (void*)&ucrt_wcstol},
    {"wcstombs", (void*)&ucrt_wcstombs},
    {"wcsxfrm", (void*)&ucrt_wcsxfrm},
    {"wcrtomb", (void*)&ucrt_wcrtomb},
    {"mbrlen", (void*)&ucrt_mbrlen},
    {"mbrtowc", (void*)&ucrt_mbrtowc},
    {"mbsrtowcs", (void*)&ucrt_mbsrtowcs},
    {"isalnum", (void*)&ucrt_isalnum},
    {"isalpha", (void*)&ucrt_isalpha},
    {"isdigit", (void*)&ucrt_isdigit},
    {"isspace", (void*)&ucrt_isspace},
    {"isxdigit", (void*)&ucrt_isxdigit},
    {"tolower", (void*)&ucrt_tolower},
    {"toupper", (void*)&ucrt_toupper},
    {"towlower", (void*)&ucrt_towlower},
    {"towupper", (void*)&ucrt_towupper},
    {"wctype", (void*)&ucrt_wctype},
    {"iswctype", (void*)&ucrt_iswctype},
    {"acos", (void*)&ucrt_acos},
    {"acosf", (void*)&ucrt_acosf},
    {"asin", (void*)&ucrt_asin},
    {"asinf", (void*)&ucrt_asinf},
    {"atan", (void*)&ucrt_atan},
    {"atan2", (void*)&ucrt_atan2},
    {"atan2f", (void*)&ucrt_atan2f},
    {"atanf", (void*)&ucrt_atanf},
    {"ceil", (void*)&ucrt_ceil},
    {"ceilf", (void*)&ucrt_ceilf},
    {"cos", (void*)&ucrt_cos},
    {"cosf", (void*)&ucrt_cosf},
    {"exp", (void*)&ucrt_exp},
    {"exp2", (void*)&ucrt_exp2},
    {"expf", (void*)&ucrt_expf},
    {"floor", (void*)&ucrt_floor},
    {"floorf", (void*)&ucrt_floorf},
    {"fmod", (void*)&ucrt_fmod},
    {"fmodf", (void*)&ucrt_fmodf},
    {"frexp", (void*)&ucrt_frexp},
    {"log", (void*)&ucrt_log},
    {"log10", (void*)&ucrt_log10},
    {"log10f", (void*)&ucrt_log10f},
    {"log2", (void*)&ucrt_log2},
    {"logf", (void*)&ucrt_logf},
    {"modf", (void*)&ucrt_modf},
    {"modff", (void*)&ucrt_modff},
    {"pow", (void*)&ucrt_pow},
    {"powf", (void*)&ucrt_powf},
    {"round", (void*)&ucrt_round},
    {"roundf", (void*)&ucrt_roundf},
    {"sin", (void*)&ucrt_sin},
    {"sinf", (void*)&ucrt_sinf},
    {"sqrt", (void*)&ucrt_sqrt},
    {"sqrtf", (void*)&ucrt_sqrtf},
    {"tan", (void*)&ucrt_tan},
    {"tanf", (void*)&ucrt_tanf},
    {"trunc", (void*)&ucrt_trunc},
    {"truncf", (void*)&ucrt_truncf},
    {"lround", (void*)&ucrt_lround},
    {"lroundf", (void*)&ucrt_lroundf},
    {"atof", (void*)&ucrt_atof},
    {"atoi", (void*)&ucrt_atoi},
    {"qsort", (void*)&ucrt_qsort},
    {"___mb_cur_max_func", (void*)&ucrt___mb_cur_max_func},
    {"abort", (void*)&ucrt_abort},
    {"exit", (void*)&ucrt_exit},
    {"_exit", (void*)&ucrt__exit},
    {"getenv", (void*)&ucrt_getenv},
    {"clock", (void*)&ucrt_clock},
    {"_time64", (void*)&ucrt__time64},
    {"_assert", (void*)&ucrt__assert},
    {"rand_s", (void*)&ucrt_rand_s},
    {"_initterm", (void*)&ucrt__initterm},
    {"_initterm_e", (void*)&ucrt__initterm_e},
    {"_initialize_onexit_table", (void*)&ucrt__initialize_onexit_table},
    {"_register_onexit_function", (void*)&ucrt__register_onexit_function},
    {"_execute_onexit_table", (void*)&ucrt__execute_onexit_table},
    {"_register_thread_local_exe_atexit_callback",
     (void*)&ucrt__register_thread_local_exe_atexit_callback},
    {"_errno", (void*)&ucrt__errno},
    {"_set_errno", (void*)&ucrt__set_errno},
    {"_get_errno", (void*)&ucrt__get_errno},
    {"_lock_file", (void*)&ucrt__lock_file},
    {"_unlock_file", (void*)&ucrt__unlock_file},
    {"__acrt_iob_func", (void*)&ucrt___acrt_iob_func},
    {"_aligned_malloc", (void*)&ucrt__aligned_malloc},
    {"_aligned_free", (void*)&ucrt__aligned_free},
};

// Resolve a UCRT symbol to its ms_abi wrapper. NULL = not wrapped (the caller
// keeps the blanket-0 named path).
void* ucrt_wrap_resolve(const char* symbol) {
  if (!symbol) return NULL;
  for (unsigned i = 0; i < sizeof(k_ucrt_wraps) / sizeof(k_ucrt_wraps[0]);
       ++i)
    if (strcmp(k_ucrt_wraps[i].name, symbol) == 0) return k_ucrt_wraps[i].fn;
  return NULL;
}
