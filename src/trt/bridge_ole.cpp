// tiramisu/src/trt/bridge_ole.cpp
// Minimal COM boot + OLEAUT. See bridge_ole.h.
//
// CoCreateInstance dispatches to registered CLSID personalities; an
// unregistered CLSID fails REGDB_E_CLASSNOTREG with the GUID in the log —
// the fail-visible rule. That log line is how we discover which COM
// classes a game actually needs on console.
#include "bridge_ole.h"
#include "thunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// unique name, file scope, OUTSIDE extern "C" (the log_line collision rule)
namespace olelog {
static void line(const char* s) { trt_core_log(s); }
}
#define log_line(s_) olelog::line(s_)

extern "C" {

namespace {

// ---- HRESULTs ----
constexpr int S_OK_ = 0;
constexpr int S_FALSE_ = 1;
constexpr int E_NOINTERFACE_ = (int)0x80004002;
constexpr int E_OUTOFMEMORY_ = (int)0x8007000E;
constexpr int E_INVALIDARG_ = (int)0x80070057;
constexpr int REGDB_E_CLASSNOTREG_ = (int)0x80040154;
constexpr int CO_E_ALREADYINITIALIZED_ = (int)0x80040161;

// apartment model: a game passes COINIT_APARTMENTTHREADED (0x2) or
// COINIT_MULTITHREADED (0x0); either is fine single-threaded
constexpr u32 COINIT_APARTMENTTHREADED = 0x2;

int g_init_count = 0;  // CoInitialize nesting

// ---- CLSID personality registry ----
constexpr u32 k_max_classes = 32;
struct com_class {
  u8 clsid[16];
  trt_com_factory factory;
  void* user;
};
com_class g_classes[k_max_classes];
u32 g_class_count = 0;

void clsid_to_str(const u8* g, char* out) {
  // {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}
  static const char hex[] = "0123456789ABCDEF";
  int o = 0;
  out[o++] = '{';
  // fields: u32 LE, u16 LE, u16 LE, 8 bytes BE
  const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
  for (int i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
    out[o++] = hex[g[order[i]] >> 4];
    out[o++] = hex[g[order[i]] & 0xF];
  }
  out[o++] = '}';
  out[o] = 0;
}

}  // namespace

void trt_ole_register_class(const void* clsid, trt_com_factory factory,
                            void* user) {
  if (!clsid || !factory || g_class_count >= k_max_classes) return;
  com_class& c = g_classes[g_class_count++];
  memcpy(c.clsid, clsid, 16);
  c.factory = factory;
  c.user = user;
}

namespace {

// ---- CoInitializeEx / CoInitialize / CoUninitialize ----
int TRT_GUEST_ABI ole_CoInitializeEx(void* reserved, u32 model) {
  (void)reserved; (void)model;
  ++g_init_count;
  char line[96];
  snprintf(line, sizeof(line), "ole: CoInitializeEx(model=%s) count=%d",
           (model & COINIT_APARTMENTTHREADED) ? "STA" : "MTA", g_init_count);
  log_line(line);
  // S_FALSE (1) when already initialized — games accept both
  return g_init_count > 1 ? S_FALSE_ : S_OK_;
}
int TRT_GUEST_ABI ole_CoInitialize(void* reserved) {
  return ole_CoInitializeEx(reserved, COINIT_APARTMENTTHREADED);
}
void TRT_GUEST_ABI ole_CoUninitialize(void) {
  if (g_init_count) --g_init_count;
}
void TRT_GUEST_ABI ole_CoFreeUnusedLibraries(void) {}

// ---- CoCreateInstance: dispatch to personalities, else fail VISIBLY ----
int TRT_GUEST_ABI ole_CoCreateInstance(const void* clsid, void* outer,
                                       u32 context, const void* iid,
                                       void** out) {
  if (!clsid || !iid || !out) return E_INVALIDARG_;
  *out = nullptr;
  if (outer) return E_NOINTERFACE_;  // no aggregation
  const u8* g = (const u8*)clsid;
  for (u32 i = 0; i < g_class_count; ++i) {
    if (memcmp(g_classes[i].clsid, g, 16) == 0) {
      char line[96];
      snprintf(line, sizeof(line), "ole: CoCreateInstance -> personality %u",
               i);
      log_line(line);
      return g_classes[i].factory(iid, out, g_classes[i].user);
    }
  }
  char guid[40];
  clsid_to_str(g, guid);
  char line[160];
  snprintf(line, sizeof(line),
           "ole: CoCreateInstance(%s ctx=%u) -> REGDB_E_CLASSNOTREG "
           "(no personality registered)", guid, context);
  log_line(line);
  return REGDB_E_CLASSNOTREG_;
}
int TRT_GUEST_ABI ole_CoGetClassObject(const void* clsid, u32 context,
                                       void* reserved, const void* iid,
                                       void** out) {
  // same dispatch surface, different entry (class factories)
  return ole_CoCreateInstance(clsid, nullptr, context, iid, out);
}

// ---- task memory (CoTaskMem*) ----
void* TRT_GUEST_ABI ole_CoTaskMemAlloc(u32 bytes) {
  if (!bytes) return nullptr;
  return malloc(bytes);
}
void* TRT_GUEST_ABI ole_CoTaskMemRealloc(void* p, u32 bytes) {
  return realloc(p, bytes);
}
void TRT_GUEST_ABI ole_CoTaskMemFree(void* p) { free(p); }
u32 TRT_GUEST_ABI ole_CoTaskMemSize(void* p) {
  (void)p;
  return 0;  // Windows answers the alloc size; nothing depends on it here
}

// ---- OLEAUT: BSTR ----
// BSTR = [u32 length-in-bytes][string bytes][\0\0], ptr points at the chars
u16* TRT_GUEST_ABI ole_SysAllocString(const u16* s) {
  if (!s) return nullptr;
  u32 chars = 0;
  while (s[chars]) ++chars;
  u8* raw = (u8*)malloc((chars + 1) * 2 + 4);
  if (!raw) return nullptr;
  *(u32*)raw = chars * 2;
  u16* str = (u16*)(raw + 4);
  memcpy(str, s, chars * 2);
  str[chars] = 0;
  return str;
}
u16* TRT_GUEST_ABI ole_SysAllocStringLen(const u16* s, u32 chars) {
  u8* raw = (u8*)malloc((chars + 1) * 2 + 4);
  if (!raw) return nullptr;
  *(u32*)raw = chars * 2;
  u16* str = (u16*)(raw + 4);
  if (s && chars) memcpy(str, s, chars * 2);
  str[chars] = 0;
  return str;
}
void TRT_GUEST_ABI ole_SysFreeString(u16* s) {
  if (!s) return;
  free((u8*)s - 4);
}
u32 TRT_GUEST_ABI ole_SysStringLen(const u16* s) {
  if (!s) return 0;
  const u8* raw = (const u8*)s - 4;
  return *(const u32*)raw / 2;
}
u32 TRT_GUEST_ABI ole_SysStringByteLen(const u16* s) {
  if (!s) return 0;
  return *(const u32*)((const u8*)s - 4);
}
int TRT_GUEST_ABI ole_SysReAllocString(u16** p, const u16* s) {
  if (!p) return 0;
  u16* n = ole_SysAllocString(s);
  if (!n) return 0;
  ole_SysFreeString(*p);
  *p = n;
  return 1;
}

// ---- OLEAUT: variants ----
void TRT_GUEST_ABI ole_VariantInit(void* v) {
  if (v) memset(v, 0, 16);  // VARIANT is 24 bytes on x64 but vt+reserved
                            // head is what Init touches
}
void TRT_GUEST_ABI ole_VariantClear(void* v) {
  // BSTRs in variants leak here — fine for a game boot path, named in log
  if (v) memset((u16*)v, 0, 2);  // vt = VT_EMPTY
}
void TRT_GUEST_ABI ole_VariantCopy(void* dst, const void* src) {
  if (dst && src) memcpy(dst, src, 24);
}
void TRT_GUEST_ABI ole_VariantInitN(void* v) { ole_VariantInit(v); }

// ---- OLEAUT: safe arrays (minimal: 1D, no lock tracking) ----
struct safearray {
  u32 dims;
  u32 features;
  u32 element_bytes;
  u32 locks;
  void* data;
  u32 elements;
  u32 lower;
};
u16* TRT_GUEST_ABI ole_SafeArrayCreateVector(u32 vt, u32 lower, u32 elements) {
  // element sizes for the VTs games actually use
  u32 eb = 0;
  switch (vt) {
    case 2: case 3: case 4: eb = 2; break;   // I2, I4(2b), R4-ish small ints
    case 5: case 6: case 7: eb = 8; break;   // R8/DATE/CY
    case 11: eb = 1; break;                  // UI1
    case 16: eb = 1; break;                  // I1
    case 17: eb = 2; break;                  // UI2
    case 18: eb = 4; break;                  // UI4
    case 19: eb = 8; break;                  // I8
    case 20: eb = 8; break;                  // UI8
    case 22: eb = 4; break;                  // INT
    case 23: eb = 4; break;                  // UINT
    case 8: eb = 4; break;                   // BSTR (pointer-sized on x64)
    default: eb = 4; break;
  }
  safearray* sa = (safearray*)malloc(sizeof(safearray));
  if (!sa) return nullptr;
  sa->dims = 1;
  sa->features = 0;
  sa->element_bytes = eb;
  sa->locks = 0;
  sa->elements = elements;
  sa->lower = lower;
  sa->data = elements ? calloc(elements, eb) : nullptr;
  return (u16*)sa;
}
void TRT_GUEST_ABI ole_SafeArrayDestroy(u16* h) {
  safearray* sa = (safearray*)h;
  if (!sa) return;
  free(sa->data);
  free(sa);
}
int TRT_GUEST_ABI ole_SafeArrayLock(u16* h) {
  safearray* sa = (safearray*)h;
  if (!sa) return E_INVALIDARG_;
  ++sa->locks;
  return S_OK_;
}
int TRT_GUEST_ABI ole_SafeArrayUnlock(u16* h) {
  safearray* sa = (safearray*)h;
  if (!sa) return E_INVALIDARG_;
  if (sa->locks) --sa->locks;
  return S_OK_;
}
int TRT_GUEST_ABI ole_SafeArrayAccessData(u16* h, void** data) {
  safearray* sa = (safearray*)h;
  if (!sa || !data) return E_INVALIDARG_;
  *data = sa->data;
  ++sa->locks;
  return S_OK_;
}
int TRT_GUEST_ABI ole_SafeArrayUnaccessData(u16* h) {
  return ole_SafeArrayUnlock(h);
}
u32 TRT_GUEST_ABI ole_SafeArrayGetDim(u16* h) {
  return h ? 1 : 0;
}
int TRT_GUEST_ABI ole_SafeArrayGetUBound(u16* h, u32 dim, i32* out) {
  safearray* sa = (safearray*)h;
  if (!sa || !out || dim != 1) return E_INVALIDARG_;
  *out = (i32)(sa->lower + sa->elements - 1);
  return S_OK_;
}
int TRT_GUEST_ABI ole_SafeArrayGetLBound(u16* h, u32 dim, i32* out) {
  safearray* sa = (safearray*)h;
  if (!sa || !out || dim != 1) return E_INVALIDARG_;
  *out = (i32)sa->lower;
  return S_OK_;
}
int TRT_GUEST_ABI ole_SafeArrayGetElement(u16* h, const i32* idx,
                                          void* out) {
  safearray* sa = (safearray*)h;
  if (!sa || !idx || !out) return E_INVALIDARG_;
  const u32 i = (u32)(*idx - (i32)sa->lower);
  if (i >= sa->elements) return E_INVALIDARG_;
  memcpy(out, (u8*)sa->data + i * sa->element_bytes, sa->element_bytes);
  return S_OK_;
}
int TRT_GUEST_ABI ole_SafeArrayPutElement(u16* h, const i32* idx,
                                          const void* v) {
  safearray* sa = (safearray*)h;
  if (!sa || !idx || !v) return E_INVALIDARG_;
  const u32 i = (u32)(*idx - (i32)sa->lower);
  if (i >= sa->elements) return E_INVALIDARG_;
  memcpy((u8*)sa->data + i * sa->element_bytes, v, sa->element_bytes);
  return S_OK_;
}

// ---- PropVariant head (PropVariantInit) ----
void TRT_GUEST_ABI ole_PropVariantInit(void* p) {
  if (p) memset(p, 0, 8);
}

// ---- OleInitialize (same surface as CoInitialize for boot) ----
int TRT_GUEST_ABI ole_OleInitialize(void* reserved) {
  return ole_CoInitializeEx(reserved, COINIT_APARTMENTTHREADED);
}
void TRT_GUEST_ABI ole_OleUninitialize(void) { ole_CoUninitialize(); }

}  // namespace

int trt_bridge_register_ole(void) {
  static const trt_named_fn k_ole32[] = {
      {"CoInitialize", (void*)&ole_CoInitialize},
      {"CoInitializeEx", (void*)&ole_CoInitializeEx},
      {"CoUninitialize", (void*)&ole_CoUninitialize},
      {"CoCreateInstance", (void*)&ole_CoCreateInstance},
      {"CoGetClassObject", (void*)&ole_CoGetClassObject},
      {"CoTaskMemAlloc", (void*)&ole_CoTaskMemAlloc},
      {"CoTaskMemRealloc", (void*)&ole_CoTaskMemRealloc},
      {"CoTaskMemFree", (void*)&ole_CoTaskMemFree},
      {"CoTaskMemSize", (void*)&ole_CoTaskMemSize},
      {"CoFreeUnusedLibraries", (void*)&ole_CoFreeUnusedLibraries},
      {"OleInitialize", (void*)&ole_OleInitialize},
      {"OleUninitialize", (void*)&ole_OleUninitialize},
      {"PropVariantInit", (void*)&ole_PropVariantInit},
  };
  static const trt_named_fn k_oleaut[] = {
      {"SysAllocString", (void*)&ole_SysAllocString},
      {"SysAllocStringLen", (void*)&ole_SysAllocStringLen},
      {"SysFreeString", (void*)&ole_SysFreeString},
      {"SysStringLen", (void*)&ole_SysStringLen},
      {"SysStringByteLen", (void*)&ole_SysStringByteLen},
      {"SysReAllocString", (void*)&ole_SysReAllocString},
      {"VariantInit", (void*)&ole_VariantInit},
      {"VariantClear", (void*)&ole_VariantClear},
      {"VariantCopy", (void*)&ole_VariantCopy},
      {"SafeArrayCreateVector", (void*)&ole_SafeArrayCreateVector},
      {"SafeArrayDestroy", (void*)&ole_SafeArrayDestroy},
      {"SafeArrayLock", (void*)&ole_SafeArrayLock},
      {"SafeArrayUnlock", (void*)&ole_SafeArrayUnlock},
      {"SafeArrayAccessData", (void*)&ole_SafeArrayAccessData},
      {"SafeArrayUnaccessData", (void*)&ole_SafeArrayUnaccessData},
      {"SafeArrayGetDim", (void*)&ole_SafeArrayGetDim},
      {"SafeArrayGetUBound", (void*)&ole_SafeArrayGetUBound},
      {"SafeArrayGetLBound", (void*)&ole_SafeArrayGetLBound},
      {"SafeArrayGetElement", (void*)&ole_SafeArrayGetElement},
      {"SafeArrayPutElement", (void*)&ole_SafeArrayPutElement},
  };
  trt_module_register("ole32.dll", TRT_ROUTE_BUILTIN, k_ole32,
                      (int)(sizeof(k_ole32) / sizeof(k_ole32[0])));
  trt_module_register("oleaut32.dll", TRT_ROUTE_BUILTIN, k_oleaut,
                      (int)(sizeof(k_oleaut) / sizeof(k_oleaut[0])));
  char line[96];
  snprintf(line, sizeof(line),
           "ole: ole32 + oleaut32 BUILTIN (%u CLSID personalities)",
           g_class_count);
  log_line(line);
  return 0;
}

}  // extern "C"
