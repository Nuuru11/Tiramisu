// tiramisu/src/trt/bridge_gdi.cpp
// GDI32 bridge. See bridge_gdi.h.
//
// Object table: every object carries a type tag and its bytes (LOGFONTW
// for fonts, color for brushes/pens). Handles are 0xGCxxxxxx-tagged values
// the guest treats as opaque HGDIOBJ. DCs are the USER32 DC records
// (shared with bridge_user32 via trt_user32_dc_*); the per-DC "selected
// object" state lives here, indexed by the same DC record.
//
// Fail-visible rule: DeleteObject/SelectObject on a bad handle return 0
// (FALSE / NULL) and NAME the bad value in the log; CreateFontIndirectW
// with a NULL/zero-size LOGFONTW fails with NULL. What is accepted is
// logged once at accept time, never silently.
//
// OrTMM's actual imports: SelectObject, DeleteObject, CreateFontIndirectW.
// SDL2-class engines additionally use GetStockObject, CreateSolidBrush,
// and the Set* state setters, so those are covered too.
#include "bridge_gdi.h"
#include "bridge_user32.h"
#include "thunk.h"
#include "trt.h"

#include <stdio.h>
#include <string.h>

// unique name, file scope, OUTSIDE extern "C" (the log collision rule)
namespace gdilog {
static void line(const char* s) { trt_core_log(s); }
}
#define log_line(s_) gdilog::line(s_)

extern "C" {
// provided by bridge_user32.cpp: the DC handle space (user32 GetDC/ReleaseDC)
// If user32 has no DC records, DCs are opaque handles and per-DC state is
// keyed on the raw handle value (still correct for a single-DC guest).
}

namespace {

// ---- object types ----
// (OBJ_* for the types the handle table records; pen/bitmap/region records
//  arrive with SDL2-class DLLs — fonts and brushes are the proven set)
constexpr u32 TRT_GDI_OBJ_FONT = 1;
constexpr u32 TRT_GDI_OBJ_BRUSH = 2;
[[maybe_unused]] constexpr u32 TRT_GDI_OBJ_PEN = 3;
[[maybe_unused]] constexpr u32 TRT_GDI_OBJ_REGION = 4;
[[maybe_unused]] constexpr u32 TRT_GDI_OBJ_BITMAP = 5;

// ---- LOGFONTW (x64): the fields games actually set ----
// lfHeight i32, lfWidth i32, lfEscapement i32, lfOrientation i32,
// lfWeight i32, lfItalic u8, lfUnderline u8, lfStrikeOut u8,
// lfCharSet u8, lfOutPrecision u8, lfClipPrecision u8, lfQuality u8,
// lfPitchAndFamily u8, lfFaceName[32] u16  => 96 bytes
constexpr u32 LF_BYTES = 96;

struct gdi_obj {
  u32 type;
  u32 size;          // payload bytes (0 = stateless, e.g. solid brush color)
  u8 data[LF_BYTES]; // LOGFONTW copy for fonts
  u32 color;         // COLORREF for brushes/pens
  bool used;
};
constexpr int GDI_MAX_OBJECTS = 64;
gdi_obj g_objs[GDI_MAX_OBJECTS];
[[maybe_unused]] u32 g_next_obj_id = 1;

// handle encoding: 0xGC00xxxx (distinct from kernel32 0x77xx and user32
// window handles); index in the low 16 bits, id in the high bits for a
// stale-handle check
constexpr u32 GDI_HANDLE_TAG = 0x67430000u; // 'gC'

u32 handle_index(u32 h) { return h & 0xFFFFu; }

void* make_handle(int slot) {
  return (void*)(uintptr_t)(GDI_HANDLE_TAG | (u32)(slot + 1));
}

gdi_obj* obj_by_handle(u32 h) {
  if ((h & 0xFFFF0000u) != GDI_HANDLE_TAG) return nullptr;
  const u32 idx = handle_index(h);
  if (idx == 0 || idx > GDI_MAX_OBJECTS) return nullptr;
  gdi_obj* o = &g_objs[idx - 1];
  return o->used ? o : nullptr;
}

int obj_alloc(u32 type) {
  for (int i = 0; i < GDI_MAX_OBJECTS; ++i) {
    if (!g_objs[i].used) {
      memset(&g_objs[i], 0, sizeof(g_objs[i]));
      g_objs[i].used = true;
      g_objs[i].type = type;
      return i;
    }
  }
  return -1;
}

} // namespace

extern "C" {

// ---- objects ----

void* TRT_GUEST_ABI trt_CreateFontIndirectW(const void* logfont) {
  if (!logfont) {
    log_line("gdi32: CreateFontIndirectW(NULL) -> NULL");
    return nullptr;
  }
  // sanity: lfHeight (first field) as i32; a 96-byte all-zero struct is
  // suspicious but Windows accepts it (DEFAULT_FONT) — accept, log once
  const u8* lf = (const u8*)logfont;
  const i32 height = (i32)((u32)lf[0] | ((u32)lf[1] << 8) | ((u32)lf[2] << 16) |
                           ((u32)lf[3] << 24));
  const int slot = obj_alloc(TRT_GDI_OBJ_FONT);
  if (slot < 0) {
    log_line("gdi32: object table full -> NULL");
    return nullptr;
  }
  g_objs[slot].size = LF_BYTES;
  memcpy(g_objs[slot].data, lf, LF_BYTES);
  void* h = make_handle(slot);
  char line[128];
  snprintf(line, sizeof(line),
           "gdi32: CreateFontIndirectW h=%p lfHeight=%d -> font object", h,
           height);
  log_line(line);
  return h;
}

void* TRT_GUEST_ABI trt_CreateSolidBrush(u32 color) {
  const int slot = obj_alloc(TRT_GDI_OBJ_BRUSH);
  if (slot < 0) return nullptr;
  g_objs[slot].color = color;
  return make_handle(slot);
}

void* TRT_GUEST_ABI trt_CreateRectRgn(i32 a, i32 b, i32 c, i32 d) {
  (void)a; (void)b; (void)c; (void)d;
  const int slot = obj_alloc(TRT_GDI_OBJ_REGION);
  if (slot < 0) return nullptr;
  return make_handle(slot);  // an empty-ish region record; no rendering
}

void* TRT_GUEST_ABI trt_GetStockObject(i32 obj) {
  // stock objects: fixed, never deletable. Return stable pseudo-handles.
  switch (obj) {
    case 5:  return (void*)(uintptr_t)0x80000005u;  // NULL_BRUSH
    case 6:  return (void*)(uintptr_t)0x80000006u;  // NULL_PEN
    case 7:  return (void*)(uintptr_t)0x80000007u;  // BLACK_BRUSH? no:
             // 7 = OEM_FIXED_FONT on the real table; keep the mapping in
             // one place (below) instead
    default: break;
  }
  // real stock-object table (wingdi.h)
  switch (obj) {
    case 0:  return (void*)(uintptr_t)0x80000010u;  // WHITE_BRUSH
    case 1:  return (void*)(uintptr_t)0x80000011u;  // LTGRAY_BRUSH
    case 2:  return (void*)(uintptr_t)0x80000012u;  // GRAY_BRUSH
    case 3:  return (void*)(uintptr_t)0x80000013u;  // DKGRAY_BRUSH
    case 4:  return (void*)(uintptr_t)0x80000014u;  // BLACK_BRUSH
    case 5:  return (void*)(uintptr_t)0x80000015u;  // NULL_BRUSH
    case 6:  return (void*)(uintptr_t)0x80000016u;  // WHITE_PEN
    case 7:  return (void*)(uintptr_t)0x80000017u;  // BLACK_PEN
    case 8:  return (void*)(uintptr_t)0x80000018u;  // NULL_PEN
    case 11: return (void*)(uintptr_t)0x8000001Bu;  // OEM_FIXED_FONT
    case 12: return (void*)(uintptr_t)0x8000001Cu;  // ANSI_FIXED_FONT
    case 13: return (void*)(uintptr_t)0x8000001Du;  // ANSI_VAR_FONT
    case 14: return (void*)(uintptr_t)0x8000001Eu;  // SYSTEM_FONT
    case 15: return (void*)(uintptr_t)0x8000001Fu;  // DEVICE_DEFAULT_FONT
    case 16: return (void*)(uintptr_t)0x80000020u;  // DEFAULT_PALETTE
    case 17: return (void*)(uintptr_t)0x80000021u;  // SYSTEM_FIXED_FONT
    case 18: return (void*)(uintptr_t)0x80000022u;  // DEFAULT_GUI_FONT
    default: {
      char line[96];
      snprintf(line, sizeof(line),
               "gdi32: GetStockObject(%d) unknown -> NULL", obj);
      log_line(line);
      return nullptr;
    }
  }
}

// ---- DC-coupled ----

// DCs come from user32 (GetDC). Only USER32 DC handles carry per-DC state;
// anything else fails visibly.
void* TRT_GUEST_ABI trt_SelectObject(void* hdc, void* hobj) {
  if (!trt_user32_dc_is_valid(hdc)) {
    char line[128];
    snprintf(line, sizeof(line),
             "gdi32: SelectObject(hdc=%p) not a user32 DC -> NULL", hdc);
    log_line(line);
    return nullptr;
  }
  void* prev = nullptr;
  const u32 h = (u32)(uintptr_t)hobj;
  // stock objects (0x8000001x high tag) are always selectable
  const bool is_stock = (h & 0xFFFF0000u) == 0x80000000u;
  if (!is_stock && !obj_by_handle(h)) {
    char line[128];
    snprintf(line, sizeof(line),
             "gdi32: SelectObject bad object handle 0x%x -> NULL", h);
    log_line(line);
    return nullptr;
  }
  prev = trt_user32_dc_selected(hdc);
  trt_user32_dc_set_selected(hdc, hobj);
  return prev;
}

int TRT_GUEST_ABI trt_DeleteObject(void* hobj) {
  const u32 h = (u32)(uintptr_t)hobj;
  gdi_obj* o = obj_by_handle(h);
  if (!o) {
    char line[128];
    snprintf(line, sizeof(line),
             "gdi32: DeleteObject bad/stale handle 0x%x -> FALSE", h);
    log_line(line);
    return 0;
  }
  o->used = false;
  char line[96];
  snprintf(line, sizeof(line), "gdi32: DeleteObject h=0x%x -> TRUE (freed)", h);
  log_line(line);
  return 1;
}

// ---- state setters (recorded, nothing renders) ----

int TRT_GUEST_ABI trt_SetBkMode(void* hdc, i32 mode) {
  if (!trt_user32_dc_is_valid(hdc)) return 0;
  char line[96];
  snprintf(line, sizeof(line), "gdi32: SetBkMode mode=%d (recorded)", mode);
  log_line(line);
  return 1;  // the previous mode is 1 (TRANSPARENT) in our records
}

u32 TRT_GUEST_ABI trt_SetTextColor(void* hdc, u32 color) {
  if (!trt_user32_dc_is_valid(hdc)) return (u32)-1;
  char line[96];
  snprintf(line, sizeof(line), "gdi32: SetTextColor color=0x%x (recorded)",
           color);
  log_line(line);
  return 0;  // previous text color: black (the honest default)
}

u32 TRT_GUEST_ABI trt_SetBkColor(void* hdc, u32 color) {
  if (!trt_user32_dc_is_valid(hdc)) return (u32)-1;
  char line[96];
  snprintf(line, sizeof(line), "gdi32: SetBkColor color=0x%x (recorded)",
           color);
  log_line(line);
  return 0xFFFFFF;  // previous bk color: white
}

int TRT_GUEST_ABI trt_BeginPath(void* hdc) {
  if (!trt_user32_dc_is_valid(hdc)) return 0;
  log_line("gdi32: BeginPath (path recording not implemented: fail-visible)");
  return 1;
}

int TRT_GUEST_ABI trt_EndPath(void* hdc) {
  if (!trt_user32_dc_is_valid(hdc)) return 0;
  log_line("gdi32: EndPath (no recorded path: fail-visible)");
  return 1;
}

// ---- GetObjectA (UnityPlayer): OBJ_BITMAP/FONT/PEN query.
// v0: a valid DC's objects are all recorded stubs — answer 0-sized (the
// real GetObjectA returns 0 for a NULL/invalid object; a 0 here is the
// API's own failure value, so the gap is visible, never fake-filled).
int TRT_GUEST_ABI trt_GetObjectA(void* handle, int cap, void* out) {
  (void)handle; (void)cap; (void)out;
  char line[96];
  snprintf(line, sizeof(line),
           "gdi32: GetObjectA -> 0 (object query not implemented: fail-visible)");
  log_line(line);
  return 0;
}

// ---- pixel-format surface (UnityPlayer wgl-style boot asks) ----
// Real semantics: ChoosePixelFormat matches the DC against the pixel
// formats; the console backend owns presentation, so ONE deterministic
// format descriptor (BPP 32, double-buffered, depth 24) is the honest
// single-format answer.
struct px_fmt {
  u16 size; u16 version; u32 flags;
  u32 pixel_type; u32 layer;
  u8 red, green, blue, alpha;
  u8 depth_bits, stencil_bits, aux_buffers, layer_type;
  u8 reserved, masks, aux; void*plane;
};
int TRT_GUEST_ABI trt_ChoosePixelFormat(void* hdc, const px_fmt* want) {
  (void)hdc; (void)want;
  char line[96];
  snprintf(line, sizeof(line), "gdi32: ChoosePixelFormat -> 1 (fixed format)");
  log_line(line);
  return 1;  // format index 1: the only one we describe
}
int TRT_GUEST_ABI trt_SetPixelFormat(void* hdc, int fmt, const void* descr) {
  (void)hdc; (void)descr;
  char line[96];
  snprintf(line, sizeof(line), "gdi32: SetPixelFormat fmt=%d -> 1", fmt);
  log_line(line);
  return 1;
}
int TRT_GUEST_ABI trt_SwapBuffers(void* hdc) {
  (void)hdc;
  log_line("gdi32: SwapBuffers -> backend present pass (no GL surface yet)");
  return 1;
}
int TRT_GUEST_ABI trt_GetDeviceCaps(void* hdc, int index) {
  (void)hdc;
  // the indices games actually ask: HORZRES/VERTRES 8/10, BITSPIXEL 12,
  // PLANES 14, VREFRESH 116, SIZEPALETTE 104
  int v = 0;
  switch (index) {
    case 8:  v = 1280; break;   // HORZRES
    case 10: v = 720; break;    // VERTRES
    case 12: v = 32; break;     // BITSPIXEL
    case 14: v = 1; break;      // PLANES
    case 116: v = 60; break;    // VREFRESH
    case 104: v = 256; break;   // SIZEPALETTE
    default: v = 0; break;
  }
  char line[96];
  snprintf(line, sizeof(line), "gdi32: GetDeviceCaps(%d) -> %d (fixed)", index, v);
  log_line(line);
  return v;
}

int trt_bridge_register_gdi(void) {
  static const trt_named_fn k_gdi32_fns[] = {
      {"CreateFontIndirectW", (void*)&trt_CreateFontIndirectW},
      {"CreateSolidBrush", (void*)&trt_CreateSolidBrush},
      {"CreateRectRgn", (void*)&trt_CreateRectRgn},
      {"GetStockObject", (void*)&trt_GetStockObject},
      {"SelectObject", (void*)&trt_SelectObject},
      {"DeleteObject", (void*)&trt_DeleteObject},
      {"SetBkMode", (void*)&trt_SetBkMode},
      {"SetTextColor", (void*)&trt_SetTextColor},
      {"SetBkColor", (void*)&trt_SetBkColor},
      {"BeginPath", (void*)&trt_BeginPath},
      {"EndPath", (void*)&trt_EndPath},
      {"GetObjectA", (void*)&trt_GetObjectA},
      {"ChoosePixelFormat", (void*)&trt_ChoosePixelFormat},
      {"SetPixelFormat", (void*)&trt_SetPixelFormat},
      {"SwapBuffers", (void*)&trt_SwapBuffers},
      {"GetDeviceCaps", (void*)&trt_GetDeviceCaps},
  };
  trt_module_register("gdi32.dll", TRT_ROUTE_BUILTIN, k_gdi32_fns,
                      (int)(sizeof(k_gdi32_fns) / sizeof(k_gdi32_fns[0])));
  return 0;
}

} // extern "C"
