// tiramisu/tests/test_ordinal_binds.cpp
//
// Two defects this pins down:
//
// 1. MEASUREMENT. The corpus dry-run confirmed an ordinal import binds with
//    trt_module_query_ordinal(), then classified it by name - and an ordinal
//    has no name, so classify_one() fell through to UNRESOLVED. Every ordinal
//    bind on a BUILTIN module (85 of them across the corpus: 45 oleaut32,
//    38 wsock32, 2 xinput1_3) was reported unresolved while it was in fact
//    binding a registered "#<n>" export. The classifier now classifies the
//    "#<n>" entry; this test asserts the underlying fact it relies on.
//
// 2. SEMANTICS. oleaut32 ordinal 12 was mapped to VariantInit (it zeroed the
//    destination and never converted) and ordinal 15 to SafeArrayCreateVector
//    (three scalars where the caller passes one SAFEARRAYBOUND pointer).
//    VariantChangeType now really converts; SafeArrayCreate honours the
//    descriptor and reports NULL for a dimension it cannot model.
#include "../src/trt/trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
int trt_bridge_register_ole(void);
int trt_bridge_register_ws2(void);
}

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond, what)                        \
  do {                                           \
    if (cond) {                                  \
      ++g_pass;                                  \
      printf("  PASS %s\n", what);               \
    } else {                                     \
      ++g_fail;                                  \
      printf("  FAIL %s\n", what);               \
    }                                            \
  } while (0)

// A VARIANT on x64 is 24 bytes: vt at 0, reserved words, then the 16-byte union
// at offset 8.
struct var24 {
  u16 vt;
  u16 r1, r2, r3;
  unsigned char data[16];
};

constexpr u16 VT_EMPTY_ = 0, VT_I4_ = 3, VT_R8_ = 5, VT_BSTR_ = 8;

using change_fn = i32(TRT_GUEST_ABI*)(void*, const void*, u16, u16);
using create_fn = u16*(TRT_GUEST_ABI*)(u32, u32, const void*);
using destroy_fn = void(TRT_GUEST_ABI*)(u16*);
using dim_fn = u32(TRT_GUEST_ABI*)(u16*);
using bound_fn = i32(TRT_GUEST_ABI*)(u16*, u32, i32*);
using elem_put_fn = i32(TRT_GUEST_ABI*)(u16*, const i32*, const void*);
using elem_get_fn = i32(TRT_GUEST_ABI*)(u16*, const i32*, void*);

void* resolve(const char* mod, const char* sym) {
  return trt_resolve_module(nullptr, mod, sym, 0);
}

// The corpus imports these two by ORDINAL only, which is the path under test:
// the module publishes them as "#12" / "#15", so resolving with a null symbol
// and the ordinal is exactly what the loader does.
void* resolve_ordinal(const char* mod, u16 ordinal) {
  return trt_resolve_module(nullptr, mod, nullptr, ordinal);
}

// A BSTR is UTF-16: the pointer addresses u16 code units, not bytes. Returning
// it as `char*` reads '4', then the NUL byte of '4'\0' and stops - which looks
// like a truncated conversion when the OLEAUT value is perfectly correct.
const char* bstr_text(const var24& v) {
  static char out[128];
  if (v.vt != VT_BSTR_) return nullptr;
  const u16* w = (const u16*)*(void* const*)v.data;  // union starts at data[0]
  if (!w) return nullptr;
  size_t i = 0;
  for (; w[i] && i + 1 < sizeof(out); ++i) out[i] = (char)(w[i] & 0x7f);
  out[i] = 0;
  return out;
}

void set_bstr(var24& v, const char* narrow) {
  memset(&v, 0, sizeof(v));
  v.vt = VT_BSTR_;
  u16* w = (u16*)malloc((strlen(narrow) + 1) * sizeof(u16));
  for (size_t i = 0; i <= strlen(narrow); ++i) w[i] = (u16)(u8)narrow[i];
  *(void**)v.data = w;
}

// ---- 1. the ordinal exports the corpus imports really exist --------------
void test_ordinal_registration() {
  // Every oleaut32 ordinal the corpus imports (UnityPlayer, GameAssembly,
  // dstoragecore, sekiro.exe).
  const u16 ole_used[] = {2, 6, 7, 8, 9, 12, 15, 16, 17, 19, 20, 23, 24, 26};
  int ole_ok = 0;
  for (u16 o : ole_used)
    if (trt_module_query_ordinal("OLEAUT32.dll", o) == 1) ++ole_ok;
  char line[128];
  snprintf(line, sizeof(line),
           "all %d oleaut32 ordinals the corpus imports bind",
           (int)(sizeof(ole_used) / sizeof(ole_used[0])));
  CHECK(ole_ok == (int)(sizeof(ole_used) / sizeof(ole_used[0])), line);

  // Every wsock32 ordinal the corpus imports (fmodex64, fmod_event_net64).
  const u16 ws_used[] = {1, 2, 3, 4, 8, 9, 10, 12, 13, 14, 16, 18, 19, 23,
                         103, 108, 111, 115, 116, 151};
  int ws_ok = 0;
  for (u16 o : ws_used)
    if (trt_module_query_ordinal("WSOCK32.dll", o) == 1) ++ws_ok;
  snprintf(line, sizeof(line),
           "all %d wsock32 ordinals the corpus imports bind",
           (int)(sizeof(ws_used) / sizeof(ws_used[0])));
  CHECK(ws_ok == (int)(sizeof(ws_used) / sizeof(ws_used[0])), line);

  // The classifier's new ordinal path: the registered "#<n>" name must be
  // classifiable, which is what stops the dry-run calling these unresolved.
  int cls = -1;
  CHECK(trt_symbol_class("oleaut32.dll", "#12", &cls) == 1 &&
            cls == (int)TRT_IMPL_REAL,
        "classifier resolves oleaut32 #12 to REAL (was reported unresolved)");
  cls = -1;
  CHECK(trt_symbol_class("wsock32.dll", "#16", &cls) == 1 &&
            cls == (int)TRT_IMPL_REAL,
        "classifier resolves wsock32 #16 to REAL");
}

// ---- 2. VariantChangeType really converts --------------------------------
void test_variant_change_type() {
  auto p_change = (change_fn)resolve_ordinal("oleaut32.dll", 12);
  auto p_alloc = (u16 * (TRT_GUEST_ABI*)(const u16*)) resolve(
      "oleaut32.dll", "SysAllocString");
  auto p_free = (void(TRT_GUEST_ABI*)(u16*))resolve("oleaut32.dll",
                                                    "SysFreeString");
  CHECK(p_change != nullptr, "oleaut32 ordinal 12 (VariantChangeType) resolves");
  if (!p_change) return;

  // VT_I4 -> VT_BSTR
  var24 src = {}, dst = {};
  src.vt = VT_I4_;
  *(i32*)src.data = 42;
  i32 hr = p_change(&dst, &src, 0, VT_BSTR_);
  if (hr != 0 || dst.vt != VT_BSTR_ || !bstr_text(dst) ||
      strcmp(bstr_text(dst), "42") != 0)
    printf("  .. I4->BSTR: hr=0x%08x vt=%u text=\"%s\"\n", (unsigned)hr,
           (unsigned)dst.vt, bstr_text(dst) ? bstr_text(dst) : "(null)");
  CHECK(hr == 0 && dst.vt == VT_BSTR_ && bstr_text(dst) &&
            strcmp(bstr_text(dst), "42") == 0,
        "VariantChangeType(VT_I4 42 -> VT_BSTR) yields \"42\"");
  if (dst.vt == VT_BSTR_ && p_free) p_free((u16*)*(void**)dst.data);

  // VT_R8 -> VT_BSTR
  memset(&src, 0, sizeof(src));
  src.vt = VT_R8_;
  *(double*)src.data = 2.5;
  memset(&dst, 0, sizeof(dst));
  hr = p_change(&dst, &src, 0, VT_BSTR_);
  if (hr != 0 || dst.vt != VT_BSTR_ || !bstr_text(dst) ||
      strcmp(bstr_text(dst), "2.5") != 0)
    printf("  .. R8->BSTR: hr=0x%08x vt=%u text=\"%s\"\n", (unsigned)hr,
           (unsigned)dst.vt, bstr_text(dst) ? bstr_text(dst) : "(null)");
  CHECK(hr == 0 && bstr_text(dst) && strcmp(bstr_text(dst), "2.5") == 0,
        "VariantChangeType(VT_R8 2.5 -> VT_BSTR) yields \"2.5\"");
  if (dst.vt == VT_BSTR_ && p_free) p_free((u16*)*(void**)dst.data);

  // VT_BSTR -> VT_I4 (a real parse, not a pointer copy)
  set_bstr(src, "123");
  memset(&dst, 0, sizeof(dst));
  hr = p_change(&dst, &src, 0, VT_I4_);
  CHECK(hr == 0 && dst.vt == VT_I4_ && *(i32*)dst.data == 123,
        "VariantChangeType(\"123\" -> VT_I4) parses to 123");
  free(*(void**)src.data);  // test-owned buffer: SysFreeString wants its own block

  // VT_BSTR -> VT_R8 (fractional text must survive: strtod, not atoi)
  set_bstr(src, "1.5");
  memset(&dst, 0, sizeof(dst));
  hr = p_change(&dst, &src, 0, VT_R8_);
  CHECK(hr == 0 && dst.vt == VT_R8_ && *(double*)dst.data == 1.5,
        "VariantChangeType(\"1.5\" -> VT_R8) parses to 1.5");
  free(*(void**)src.data);  // test-owned buffer: SysFreeString wants its own block

  // Non-numeric text must fail, not silently become 0.
  set_bstr(src, "not-a-number");
  memset(&dst, 0, sizeof(dst));
  hr = p_change(&dst, &src, 0, VT_I4_);
  CHECK(hr != 0, "VariantChangeType(\"not-a-number\" -> VT_I4) -> DISP_E_TYPEMISMATCH");
  free(*(void**)src.data);  // test-owned buffer: SysFreeString wants its own block

  // Same type copies.
  memset(&src, 0, sizeof(src));
  src.vt = VT_I4_;
  *(i32*)src.data = -7;
  memset(&dst, 0, sizeof(dst));
  dst.vt = VT_I4_;
  *(i32*)dst.data = 99;
  hr = p_change(&dst, &src, 0, VT_I4_);
  CHECK(hr == 0 && *(i32*)dst.data == -7,
        "VariantChangeType(VT_I4 -> VT_I4) copies the value");

  // VT_EMPTY -> VT_BSTR is the documented empty string, not an error.
  memset(&src, 0, sizeof(src));
  src.vt = VT_EMPTY_;
  memset(&dst, 0, sizeof(dst));
  hr = p_change(&dst, &src, 0, VT_BSTR_);
  CHECK(hr == 0 && dst.vt == VT_BSTR_ && bstr_text(dst) && bstr_text(dst)[0] == 0,
        "VariantChangeType(VT_EMPTY -> VT_BSTR) yields the empty BSTR");
  if (dst.vt == VT_BSTR_ && p_free) p_free((u16*)*(void**)dst.data);

  (void)p_alloc;
}

// ---- 3. SafeArrayCreate honours the descriptor --------------------------
void test_safe_array_create() {
  auto p_create = (create_fn)resolve_ordinal("oleaut32.dll", 15);
  auto p_destroy = (destroy_fn)resolve("oleaut32.dll", "SafeArrayDestroy");
  auto p_dim = (dim_fn)resolve("oleaut32.dll", "SafeArrayGetDim");
  auto p_lbound = (bound_fn)resolve("oleaut32.dll", "SafeArrayGetLBound");
  auto p_ubound = (bound_fn)resolve("oleaut32.dll", "SafeArrayGetUBound");
  auto p_put = (elem_put_fn)resolve("oleaut32.dll", "SafeArrayPutElement");
  auto p_get = (elem_get_fn)resolve("oleaut32.dll", "SafeArrayGetElement");
  CHECK(p_create && p_destroy && p_dim && p_lbound && p_ubound && p_put && p_get,
        "oleaut32 ordinal 15 (SafeArrayCreate) plus the SafeArray API resolve");
  if (!p_create) return;

  // SAFEARRAYBOUND { cElements = 5; lLbound = 1; } for a VT_I4 array.
  struct { u32 elements; i32 lower; } bound = {5, 1};
  u16* sa = p_create(3 /* VT_I4 */, 1, &bound);
  CHECK(sa != nullptr, "SafeArrayCreate(VT_I4, 1 dim, {5, lbound=1}) -> array");
  if (!sa) return;
  CHECK(p_dim(sa) == 1, "SafeArrayGetDim -> 1");
  i32 lb = -99, ub = -99;
  CHECK(p_lbound(sa, 1, &lb) == 0 && lb == 1,
        "SafeArrayGetLBound -> the descriptor's lLbound (1)");
  CHECK(p_ubound(sa, 1, &ub) == 0 && ub == 5,
        "SafeArrayGetUBound -> lLbound + cElements - 1 (5)");

  const i32 idx = 3;
  const i32 value = 0x1234;
  CHECK(p_put(sa, &idx, &value) == 0, "SafeArrayPutElement(index 3)");
  i32 back = 0;
  CHECK(p_get(sa, &idx, &back) == 0 && back == 0x1234,
        "SafeArrayGetElement(index 3) returns what was stored");

  // A dimension this bridge cannot model must fail visibly.
  struct { u32 elements; i32 lower; } multi[2] = {{2, 0}, {2, 0}};
  CHECK(p_create(3, 2, multi) == nullptr,
        "SafeArrayCreate(2 dims) -> NULL (1-D only, fails visibly)");
  p_destroy(sa);
}

}  // namespace

int main() {
  trt_bridge_register_ole();
  trt_bridge_register_ws2();

  printf("-- ordinal exports the corpus imports\n");
  test_ordinal_registration();
  printf("-- VariantChangeType (oleaut32 ordinal 12)\n");
  test_variant_change_type();
  printf("-- SafeArrayCreate (oleaut32 ordinal 15)\n");
  test_safe_array_create();

  printf("RESULT: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
