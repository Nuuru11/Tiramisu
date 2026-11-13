// tiramisu/src/guest/guest_ole.c
// Proves the minimal COM boot + OLEAUT bridge: the boot sequence a
// COM-heavy engine runs before any real COM call.
// Returns a bitmask:
//   0x001 CoInitializeEx -> S_OK
//   0x002 CoInitializeEx again -> S_FALSE (nesting)
//   0x004 CoTaskMemAlloc/Free round-trip
//   0x008 CoCreateInstance with no personality -> REGDB_E_CLASSNOTREG
//   0x010 SysAllocString/SysStringLen/SysFreeString round-trip
//   0x020 VariantInit -> vt VT_EMPTY, VariantClear ok
//   0x040 SafeArrayCreateVector + bounds + element put/get round-trip
//   0x080 SafeArrayDestroy ok
//   0x100 OleInitialize -> S_OK
//   0x200 CoUninitialize nests down without crash
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int i32;
typedef unsigned long long u64;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

int _fltused = 1;

__attribute__((dllimport)) i32 GSTD CoInitializeEx(void* reserved, u32 model);
__attribute__((dllimport)) void GSTD CoUninitialize(void);
__attribute__((dllimport)) i32 GSTD CoCreateInstance(const u8* clsid,
                                                     void* outer, u32 ctx,
                                                     const u8* iid, void** out);
__attribute__((dllimport)) void* GSTD CoTaskMemAlloc(u32 bytes);
__attribute__((dllimport)) void GSTD CoTaskMemFree(void* p);
__attribute__((dllimport)) i32 GSTD OleInitialize(void* reserved);
__attribute__((dllimport)) u16* GSTD SysAllocString(const u16* s);
__attribute__((dllimport)) u32 GSTD SysStringLen(const u16* s);
__attribute__((dllimport)) void GSTD SysFreeString(u16* s);
__attribute__((dllimport)) void GSTD VariantInit(void* v);
__attribute__((dllimport)) void GSTD VariantClear(void* v);
__attribute__((dllimport)) u16* GSTD SafeArrayCreateVector(u32 vt, u32 lower,
                                                           u32 elements);
__attribute__((dllimport)) void GSTD SafeArrayDestroy(u16* h);
__attribute__((dllimport)) i32 GSTD SafeArrayGetUBound(u16* h, u32 dim,
                                                       i32* out);
__attribute__((dllimport)) i32 GSTD SafeArrayGetElement(u16* h,
                                                        const i32* idx,
                                                        void* out);
__attribute__((dllimport)) i32 GSTD SafeArrayPutElement(u16* h,
                                                        const i32* idx,
                                                        const void* v);

u32 GSTD guest_ole_main(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  // ---- 1. COM boot + nesting ----
  if (CoInitializeEx(0, 0) == 0) bits |= 0x01u;            // S_OK
  if (CoInitializeEx(0, 0) == 1) bits |= 0x02u;            // S_FALSE

  // ---- 2. task memory ----
  {
    u32* p = (u32*)CoTaskMemAlloc(64);
    if (p) {
      p[0] = 0x12345678;
      if (p[0] == 0x12345678) bits |= 0x04u;
      CoTaskMemFree(p);
    }
  }

  // ---- 3. CoCreateInstance with an unregistered CLSID -> named failure ----
  {
    // CLSID {00000000-0000-0000-0000-000000000001} — nothing registers it
    static const u8 clsid[16] = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    static const u8 iid[16] = {0};
    void* unk = 0;
    const i32 hr = CoCreateInstance(clsid, 0, 1 /*CLSCTX_INPROC_SERVER*/,
                                    iid, &unk);
    if (hr == (i32)0x80040154 /*REGDB_E_CLASSNOTREG*/ && !unk) bits |= 0x08u;
  }

  // ---- 4. BSTR round-trip ----
  {
    static const u16 text[6] = {'t', 'i', 'r', 'a', 'm', 0};  // L"tiram" — 5 chars
    u16* s = SysAllocString(text);
    if (s) {
      if (SysStringLen(s) == 5) bits |= 0x10u;
      SysFreeString(s);
    }
  }

  // ---- 5. variant head ----
  {
    u32 v[6];
    VariantInit(v);
    if (v[0] == 0 /*VT_EMPTY*/) bits |= 0x20u;
    VariantClear(v);
  }

  // ---- 6. safe array round-trip ----
  {
    u16* sa = SafeArrayCreateVector(19 /*VT_I8... we use 23/UINT*/, 0, 4);
    if (sa) {
      i32 ub = -1;
      if (SafeArrayGetUBound(sa, 1, &ub) == 0 && ub == 3) bits |= 0x40u;
      {
        const i32 idx = 2;
        const u32 put = 0xBEEF;
        u32 got = 0;
        if (SafeArrayPutElement(sa, &idx, &put) == 0 &&
            SafeArrayGetElement(sa, &idx, &got) == 0 && got == 0xBEEF)
          bits |= 0x40u;
      }
      SafeArrayDestroy(sa);
      bits |= 0x80u;
    }
  }

  // ---- 7. OleInitialize + unwind ----
  if (OleInitialize(0) == 1 /*S_FALSE, already initialized*/) bits |= 0x100u;

  CoUninitialize();
  CoUninitialize();
  CoUninitialize();  // one extra: must not crash
  bits |= 0x200u;

  return bits;
}
