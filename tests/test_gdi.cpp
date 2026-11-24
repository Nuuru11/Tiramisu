// tiramisu/tests/test_gdi.cpp
// The gdi32 bridge: a freestanding guest creates a font, selects it into
// a user32 DC, deletes it; the host verifies the record lifecycle through
// the shared log and the guest's return bits.
//
// Sprint 1A (GDI boot path): with no guest exe argument the test resolves
// the boot-path surface straight through the module table and drives it
// from the host via TRT_STDCALL (ms_abi) function pointers — the same
// handlers a game's gdi32! imports bind to.
#include "../src/trt/trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_user32(void);
int trt_bridge_register_gdi(void);
}

namespace {

char g_log[256][256];
int g_log_count = 0;
bool log_contains(const char* needle) {
  for (int i = 0; i < g_log_count; ++i)
    if (strstr(g_log[i], needle)) return true;
  return false;
}
int log_count_of(const char* needle) {
  int n = 0;
  for (int i = 0; i < g_log_count; ++i)
    if (strstr(g_log[i], needle)) ++n;
  return n;
}

struct extent { void* base; size_t len; };
extent g_extents[16];
int g_extent_count = 0;

void* linux_alloc(size_t bytes, u64 preferred_base) {
  void* p = nullptr;
  if (preferred_base) {
    // ASan's shadow gap swallows the guest's usual preferred bases
    // (0x140000000 is INSIDE it — measured "unknown-crash" false positives):
    // under sanitizers, let the kernel pick and let the loader relocate.
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && \
    (defined(__clang__) && __has_feature(address_sanitizer)))
    const uintptr_t gap_lo = 0x00008fff7000ull, gap_hi = 0x02008fff6fffull;
    if (preferred_base >= gap_lo && preferred_base <= gap_hi) preferred_base = 0;
#endif
    if (preferred_base)
      p = mmap((void*)(uintptr_t)preferred_base, bytes, PROT_READ | PROT_WRITE,
               MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  }
  if (!p || p == MAP_FAILED)
    p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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
void linux_release(void* base, size_t) {
  for (int i = 0; i < g_extent_count; ++i)
    if (g_extents[i].base == base) {
      munmap(base, g_extents[i].len);
      g_extents[i].base = nullptr;
      return;
    }
}
void linux_log(const char* line) {
  printf("  [trt] %s\n", line);
  if (g_log_count < 256)
    snprintf(g_log[g_log_count++], sizeof(g_log[0]), "%s", line);
}

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

// Sprint 1A boot-path surface. Typedefs MUST carry TRT_STDCALL
// (ms_abi, src/trt/pe.h): a SysV call through one of these pointers
// enters the Win64 ABI exactly like the win64_call trampoline.
typedef void* (TRT_STDCALL* fn_getdc)(void*);
typedef int (TRT_STDCALL* fn_releasedc)(void*, void*);
typedef void* (TRT_STDCALL* fn_ccdc)(void*);
typedef void* (TRT_STDCALL* fn_ccbmp)(void*, int, int);
typedef void* (TRT_STDCALL* fn_stock)(int);
typedef void* (TRT_STDCALL* fn_select)(void*, void*);
typedef int (TRT_STDCALL* fn_delobj)(void*);
typedef int (TRT_STDCALL* fn_delete_dc)(void*);
typedef int (TRT_STDCALL* fn_blt9)(void*, int, int, int, int, void*, int, int,
                                   u32);
typedef int (TRT_STDCALL* fn_sblt)(void*, int, int, int, int, void*, int, int,
                                   int, int, u32);
typedef int (TRT_STDCALL* fn_caps)(void*, int);
typedef int (TRT_STDCALL* fn_setstretch)(void*, int);
typedef int (TRT_STDCALL* fn_getstretch)(void*);

void* resolve_gdi(const char* name) {
  return trt_resolve_module(nullptr, "gdi32.dll", name, 0);
}

}  // namespace

int main(int argc, char** argv) {
  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &linux_log,
                 nullptr};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();
  trt_bridge_register_gdi();

  printf("tiramisu gdi32 bridge test\n");

  CHECK(trt_module_route("gdi32.dll") == TRT_ROUTE_BUILTIN,
        "gdi32.dll routes BUILTIN (no fake SYSTEM bind)");
  CHECK(trt_module_route("user32.dll") == TRT_ROUTE_BUILTIN,
        "user32.dll routes BUILTIN (shared DC space)");

  // ---- D3DKMT adapter queries (honest stubs, host-direct) ------------------
  {
    using Dkmt_t = i32(TRT_GUEST_ABI*)(void*);
    auto pOpen = (Dkmt_t)trt_resolve_module(nullptr, "gdi32.dll",
                                            "D3DKMTOpenAdapterFromLuid", 0);
    auto pClose = (Dkmt_t)trt_resolve_module(nullptr, "gdi32.dll",
                                             "D3DKMTCloseAdapter", 0);
    auto pQuery = (Dkmt_t)trt_resolve_module(nullptr, "gdi32.dll",
                                             "D3DKMTQueryAdapterInfo", 0);
    auto pEnum = (Dkmt_t)trt_resolve_module(nullptr, "gdi32.dll",
                                            "D3DKMTEnumAdapters", 0);
    CHECK(pOpen && pClose && pQuery && pEnum,
          "gdi32 exports the D3DKMT adapter-query surface");

    // D3DKMT_OPENADAPTERFROMLUID { LUID(8); ULONG hAdapter; }
    struct { u32 lo, hi, h; } oa{0x1337u, 0, 0u};
    CHECK(pOpen && pOpen(&oa) == 0 && oa.h == 0x44584B31u,
          "D3DKMTOpenAdapterFromLuid -> STATUS_SUCCESS + synthetic hAdapter");

    // D3DKMT_QUERYADAPTERINFO { ULONG hAdapter; ULONG Type; void* d; u32 n; }
    struct { u32 h, type; void* d; u32 n; } qa{0x44584B31u, 35u, nullptr, 0u};
    CHECK(pQuery && pQuery(&qa) == (i32)0xC0000002u,
          "D3DKMTQueryAdapterInfo -> STATUS_NOT_IMPLEMENTED (fail-visible)");

    // D3DKMT_ENUMADAPTERS { UINT NumAdapters; D3DKMT_ADAPTERINFO Adapters[16]; }
    u8 ea[8 + 16 * 24] = {};
    CHECK(pEnum && pEnum(ea) == 0 && *(u32*)ea == 1u &&
              *(u32*)(ea + 8 + 8) == 0x1337u,
          "D3DKMTEnumAdapters -> 1 Xbox APU adapter with the bridge LUID");

    struct { u32 h; } ca{0x44584B31u};
    CHECK(pClose && pClose(&ca) == 0, "D3DKMTCloseAdapter -> STATUS_SUCCESS");
  }

  // ---- guest-PE flow (existing coverage; needs a guest exe argument) ----
  if (argc >= 2) {
    size_t size = 0;
    u8* file = read_file(argv[1], &size);
    if (!file) { printf("cannot read %s\n", argv[1]); return 2; }

    trt_resolver rs;
    trt_resolver_default(&rs);
    trt_image* img = nullptr;
    const int rc = trt_map_pe(&be, &rs, file, size, &img);
    CHECK(rc == TRT_OK, "guest PE maps (gdi32 + user32 GetDC bound)");
    if (rc != TRT_OK || !img) {
      printf("  map failed: %s (%s)\n", trt_errstr(rc),
             img ? img->last_detail : "no image");
      free(file);
      printf("RESULT: %d FAIL\n", failures ? failures : 1);
      return 1;
    }
    CHECK(trt_seal_image(&be, img) == TRT_OK, "image sealed (W^X)");

    const u64 ret = win64_call(img->base + img->entry_rva, 0, 0, 0);
    printf("  guest returned 0x%llx (want 0xff)\n", (unsigned long long)ret);

    CHECK((ret & 0x01u) != 0, "GetDC handed out a real DC record");
    CHECK((ret & 0x02u) != 0, "CreateFontIndirectW returned a handle");
    CHECK((ret & 0x04u) != 0, "SelectObject recorded per-DC state");
    CHECK((ret & 0x08u) != 0, "DeleteObject freed the record (select-after fails)");
    CHECK((ret & 0x10u) != 0, "GetStockObject is stable");
    CHECK((ret & 0x20u) != 0, "stock object selected; ReleaseDC released");
    CHECK((ret & 0x40u) != 0, "garbage DC -> SelectObject NULL (fail-visible)");
    CHECK((ret & 0x80u) != 0, "SetBkMode/SetTextColor/SetBkColor recorded");

    // host-side lifecycle evidence in the log
    CHECK(log_contains("gdi32: CreateFontIndirectW h=") &&
              log_contains("lfHeight=-24"),
          "log names the created font with its lfHeight");
    CHECK(log_count_of("gdi32: DeleteObject") == 1,
          "exactly one DeleteObject (the freed font)");
    CHECK(log_contains("not a user32 DC -> NULL"),
          "the garbage-DC select was named");
    CHECK(log_contains("user32: GetDC") == false,  // GetDC itself is silent-ok
          "no misleading GetDC failure noise");

    trt_unmap(&be, img);
    free(file);
  } else {
    printf("  (no guest exe: module-table surface only)\n");
  }

  // ---- Sprint 1A: gdi32 boot path resolved through the module table ----
  CHECK(resolve_gdi("GetDC") != nullptr,
        "gdi32!GetDC resolves (shared user32 handler)");
  CHECK(resolve_gdi("ReleaseDC") != nullptr,
        "gdi32!ReleaseDC resolves (shared user32 handler)");
  CHECK(resolve_gdi("BitBlt") != nullptr, "gdi32!BitBlt resolves");
  CHECK(resolve_gdi("StretchBlt") != nullptr, "gdi32!StretchBlt resolves");
  CHECK(resolve_gdi("SetStretchBltMode") != nullptr,
        "gdi32!SetStretchBltMode resolves (binkw32 load path)");
  CHECK(resolve_gdi("GetStretchBltMode") != nullptr,
        "gdi32!GetStretchBltMode resolves");
  CHECK(resolve_gdi("CreateCompatibleDC") != nullptr,
        "gdi32!CreateCompatibleDC resolves");
  CHECK(resolve_gdi("CreateCompatibleBitmap") != nullptr,
        "gdi32!CreateCompatibleBitmap resolves");
  CHECK(resolve_gdi("GetStockObject") != nullptr,
        "gdi32!GetStockObject resolves");

  const fn_stock stock = (fn_stock)resolve_gdi("GetStockObject");
  if (stock) {
    void* white = stock(0);   // WHITE_BRUSH
    void* black = stock(4);   // BLACK_BRUSH
    void* nullb = stock(5);   // NULL_BRUSH
    void* oemf  = stock(10);  // OEM_FIXED_FONT
    void* pal   = stock(15);  // DEFAULT_PALETTE
    CHECK(white && black && nullb && oemf && pal,
          "GetStockObject WHITE/BLACK/NULL_BRUSH, OEM_FIXED_FONT, "
          "DEFAULT_PALETTE all non-NULL");
    CHECK(white == stock(0), "GetStockObject stable for the same id");
    const fn_delobj delobj = (fn_delobj)resolve_gdi("DeleteObject");
    CHECK(delobj && delobj(nullb) == 1,
          "DeleteObject on a stock handle is a no-op (TRUE)");
  } else {
    CHECK(false, "GetStockObject callable");
  }

  const fn_getdc getdc = (fn_getdc)resolve_gdi("GetDC");
  const fn_releasedc releasedc = (fn_releasedc)resolve_gdi("ReleaseDC");
  const fn_ccdc ccdc = (fn_ccdc)resolve_gdi("CreateCompatibleDC");
  const fn_ccbmp ccbmp = (fn_ccbmp)resolve_gdi("CreateCompatibleBitmap");
  const fn_blt9 bitblt = (fn_blt9)resolve_gdi("BitBlt");
  const fn_sblt sblt = (fn_sblt)resolve_gdi("StretchBlt");
  const fn_select selobj = (fn_select)resolve_gdi("SelectObject");
  const fn_delete_dc deldc = (fn_delete_dc)resolve_gdi("DeleteDC");
  if (getdc && releasedc && ccdc && ccbmp && bitblt && sblt && selobj && deldc) {
    void* hdc = getdc(nullptr);
    CHECK(hdc != nullptr, "gdi32!GetDC(NULL) handed out a DC record");
    const fn_setstretch setstretch =
        (fn_setstretch)resolve_gdi("SetStretchBltMode");
    const fn_getstretch getstretch =
        (fn_getstretch)resolve_gdi("GetStretchBltMode");
    if (setstretch && getstretch) {
      CHECK(getstretch(hdc) == 3,
            "stretch mode defaults to COLORONCOLOR (3)");
      CHECK(setstretch(hdc, 4) == 3,
            "SetStretchBltMode(HALFTONE) -> previous mode 3");
      CHECK(getstretch(hdc) == 4, "stretch mode round-trips (HALFTONE)");
      CHECK(setstretch(hdc, 9) == 0,
            "invalid stretch mode -> 0 (fail-visible)");
      CHECK(getstretch(hdc) == 4, "invalid set left the mode unchanged");
      CHECK(setstretch(nullptr, 3) == 0,
            "SetStretchBltMode on a garbage DC -> 0 (fail-visible)");
    } else {
      CHECK(false, "stretch-mode surface callable");
    }
    void* memdc = ccdc(hdc);
    CHECK(memdc != nullptr, "CreateCompatibleDC off the window DC");
    void* hbmp = ccbmp(memdc, 64, 64);
    CHECK(hbmp != nullptr, "CreateCompatibleBitmap 64x64 on the memory DC");
    CHECK(selobj(memdc, hbmp) != nullptr, "bitmap selected into the memory DC");
    CHECK(bitblt(memdc, 0, 0, 64, 64, hdc, 0, 0, 0x00CC0020u) == 1,
          "BitBlt SRCCOPY -> TRUE");
    CHECK(sblt(memdc, 0, 0, 32, 32, hdc, 0, 0, 64, 64, 0x00CC0020u) == 1,
          "StretchBlt SRCCOPY -> TRUE");
    CHECK(log_contains("gdi32: BitBlt 64x64") &&
              log_contains("gdi32: StretchBlt"),
          "blits name themselves in the log (fail-visible, never silent)");
    const fn_caps caps = (fn_caps)resolve_gdi("GetDeviceCaps");
    CHECK(caps && caps(hdc, 8) == 1920 && caps(hdc, 10) == 1080 &&
              caps(hdc, 12) == 32 && caps(hdc, 14) == 1,
          "GetDeviceCaps HORZRES/VERTRES/BITSPIXEL/PLANES plausible");

    using fn_otm = u32(TRT_GUEST_ABI*)(void*, u32, void*);
    const fn_otm otm_fn = (fn_otm)resolve_gdi("GetOutlineTextMetricsW");
    CHECK(otm_fn != nullptr, "gdi32!GetOutlineTextMetricsW resolves");
    if (otm_fn) {
      CHECK(otm_fn(hdc, 0, nullptr) == 216,
            "GetOutlineTextMetricsW(0, NULL) queries required size (216)");
      u8 otm_buf[256] = {};
      CHECK(otm_fn(hdc, 216, otm_buf) == 216,
            "GetOutlineTextMetricsW fills buffer");
      CHECK(*(u32*)otm_buf == 216,
            "GetOutlineTextMetricsW sets otmSize");
      CHECK(otm_fn(hdc, 2, otm_buf) == 0,
            "GetOutlineTextMetricsW refuses buffer smaller than 4 bytes");
    }

    CHECK(releasedc(nullptr, hdc) == 1, "gdi32!ReleaseDC released the window DC");
    CHECK(deldc(memdc) == 1, "DeleteDC released the memory DC");

    // ---- Palettes & DIB color tables (former gaps -> real state) ---------
    {
      using fn_crpal = void*(TRT_GUEST_ABI*)(const void*);
      using fn_selpal = void*(TRT_GUEST_ABI*)(void*, void*, int);
      using fn_realpal = u32(TRT_GUEST_ABI*)(void*);
      using fn_palent = u32(TRT_GUEST_ABI*)(void*, u32, u32, void*);
      using fn_dibct = u32(TRT_GUEST_ABI*)(void*, u32, u32, const void*);
      using fn_getdibct = u32(TRT_GUEST_ABI*)(void*, u32, u32, void*);
      using fn_crdib = void*(TRT_GUEST_ABI*)(void*, const void*, u32, void**, void*, u32);
      auto p_crpal = (fn_crpal)resolve_gdi("CreatePalette");
      auto p_selpal = (fn_selpal)resolve_gdi("SelectPalette");
      auto p_realpal = (fn_realpal)resolve_gdi("RealizePalette");
      auto p_getent = (fn_palent)resolve_gdi("GetPaletteEntries");
      auto p_setdibct = (fn_dibct)resolve_gdi("SetDIBColorTable");
      auto p_getdibct = (fn_getdibct)resolve_gdi("GetDIBColorTable");
      auto p_crdib = (fn_crdib)resolve_gdi("CreateDIBSection");
      CHECK(p_crpal && p_selpal && p_realpal && p_setdibct, "palette + DIB color table exports resolve");
      if (p_crpal && p_selpal && p_realpal && p_getent && p_setdibct && p_getdibct && p_crdib) {
        u8 logpal[4 + 2 * 4] = {};
        *(u16*)(logpal + 0) = 0x0300;
        *(u16*)(logpal + 2) = 2;
        *(u32*)(logpal + 4) = 0x00FF0000u;
        *(u32*)(logpal + 8) = 0x0000FF00u;
        void* hpal = p_crpal(logpal);
        CHECK(hpal != nullptr, "CreatePalette returns real HPALETTE handle");
        u32 entries[2] = {};
        CHECK(p_getent(hpal, 0, 2, entries) == 2 && entries[0] == 0x00FF0000u,
              "GetPaletteEntries reads stored entries");
        void* pdc = ccdc(nullptr);
        void* prev = p_selpal(pdc, hpal, 0);
        CHECK(prev != nullptr && p_realpal(pdc) == 2,
              "SelectPalette + RealizePalette reports 2 realized entries");
        u8 bmi[40] = {};
        *(u32*)(bmi + 0) = 40;
        *(i32*)(bmi + 4) = 8;
        *(i32*)(bmi + 8) = 8;
        *(u16*)(bmi + 12) = 1;
        *(u16*)(bmi + 14) = 8;
        void* dib = p_crdib(pdc, bmi, 0, nullptr, nullptr, 0);
        selobj(pdc, dib);
        u32 ct_in[2] = {0x11223344u, 0x55667788u};
        u32 ct_out[2] = {};
        CHECK(p_setdibct(pdc, 0, 2, ct_in) == 2 && p_getdibct(pdc, 0, 2, ct_out) == 2 &&
                  ct_out[1] == 0x55667788u,
              "SetDIBColorTable/GetDIBColorTable round-trips");
        deldc(pdc);
        const fn_delobj del = (fn_delobj)resolve_gdi("DeleteObject");
        if (del) { del(dib); del(hpal); }
      }
    }

    const fn_delobj delobj2 = (fn_delobj)resolve_gdi("DeleteObject");
    if (delobj2) delobj2(hbmp);  // free the compatible bitmap record
  } else {
    CHECK(false, "boot-path DC/blit surface callable");
  }

  if (failures) {
    printf("RESULT: %d FAIL\n", failures);
    return 1;
  }
  printf("RESULT: ALL PASS\n");
  return 0;
}
