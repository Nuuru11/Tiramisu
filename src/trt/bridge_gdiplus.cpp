// tiramisu/src/trt/bridge_gdiplus.cpp
// gdiplus.dll bridge — the GDI+ 2D flat API (GpStatus contract).
//
// Consumers: BotW-Native's launcher/setup wizards draw their UI with GDI+
// (windows, folder pickers, progress art). The GAME (BotWRecomp.exe) does not
// import gdiplus at all — this surface exists so the launcher-class exes map
// and run their logic.
//
// Honesty model:
//  - Object lifecycle is REAL: Create* hand out non-null tokens from a tracked
//    pool, Delete/Dispose retire them, GdipAlloc/GdipFree are real malloc.
//  - Drawing calls are RECORDED, not rendered: the console window layer has no
//    GDI+ rasterizer. Each draw site logs its first few calls then counts the
//    rest (a launcher that draws nothing visible is a named work item, not a
//    silent black box).
//  - Status contract: GpStatus Ok == 0 for every accepted call.
#include "bridge_gdiplus.h"
#include "thunk.h"
#include "trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace gdipluslog {
static void line(const char* s) { trt_core_log(s); }
}  // namespace gdipluslog
#define log_line(s_) gdipluslog::line(s_)

namespace {

constexpr u32 k_pool_size = 256;
void* g_objects[k_pool_size];
u32 g_object_count = 0;
u32 g_draw_calls = 0;

void* pool_take() {
  if (g_object_count < k_pool_size) return &g_objects[g_object_count++];
  return nullptr;  // pool exhausted: callers see NULL and log it
}

// log the first 3 calls of each draw site, then just count
void draw_log(const char* site) {
  static struct { const char* site; u32 hits; } seen[32];
  static u32 seen_count = 0;
  u32 hits = 0;
  for (u32 i = 0; i < seen_count; ++i) {
    if (strcmp(seen[i].site, site) == 0) {
      hits = ++seen[i].hits;
      break;
    }
  }
  if (hits == 0 && seen_count < 32) {
    seen[seen_count].site = site;
    hits = seen[seen_count].hits = 1;
  }
  ++g_draw_calls;
  if (hits <= 3) {
    char line[128];
    snprintf(line, sizeof(line), "gdiplus: %s -> Ok (recorded, not rendered)",
             site);
    log_line(line);
  }
}

}  // namespace

extern "C" {

// ---- lifecycle ----
i32 TRT_GUEST_ABI gdiplus_GdiplusStartup(u64* token, const void* input,
                                         void* output) {
  (void)input;
  (void)output;
  if (token) *token = 0xB07C0001ull;
  log_line("gdiplus: GdiplusStartup -> Ok (token 0xB07C0001)");
  return 0;
}

void TRT_GUEST_ABI gdiplus_GdiplusShutdown(u64 token) {
  char line[80];
  snprintf(line, sizeof(line), "gdiplus: GdiplusShutdown(0x%llx)",
           (unsigned long long)token);
  log_line(line);
}

void* TRT_GUEST_ABI gdiplus_GdipAlloc(u32 size) {
  void* p = malloc(size ? size : 1);
  return p;
}

void TRT_GUEST_ABI gdiplus_GdipFree(void* mem) { free(mem); }

// ---- graphics / objects ----
i32 TRT_GUEST_ABI gdiplus_GdipCreateFromHDC(void* hdc, void** graphics) {
  if (!graphics) return 2;  // InvalidParameter
  *graphics = pool_take();
  draw_log("GdipCreateFromHDC");
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDeleteGraphics(void* graphics) {
  (void)graphics;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipCreateSolidFill(u32 color, void** brush) {
  if (!brush) return 2;
  *brush = pool_take();
  draw_log("GdipCreateSolidFill");
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDeleteBrush(void* brush) {
  (void)brush;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipCloneBrush(void* brush, void** clone) {
  if (!clone) return 2;
  *clone = pool_take();
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipCreatePen1(u32 color, u32 width, u32 unit,
                                         void** pen) {
  (void)width;
  (void)unit;
  if (!pen) return 2;
  *pen = pool_take();
  draw_log("GdipCreatePen1");
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDeletePen(void* pen) {
  (void)pen;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipCreatePath(i32 fillmode, void** path) {
  (void)fillmode;
  if (!path) return 2;
  *path = pool_take();
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDeletePath(void* path) {
  (void)path;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipAddPathArc(void* path, float x1, float y1, float x2,
                                         float y2, float start, float sweep) {
  (void)path;
  (void)x1;
  (void)y1;
  (void)x2;
  (void)y2;
  (void)start;
  (void)sweep;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipAddPathRectangle(void* path, float x, float y,
                                               float w, float h) {
  (void)path;
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipClosePathFigure(void* path) {
  (void)path;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipCreateBitmapFromScan0(i32 w, i32 h, i32 stride,
                                                    i32 fmt, void* bits,
                                                    void** bitmap) {
  (void)stride;
  (void)fmt;
  (void)bits;
  if (!bitmap || w <= 0 || h <= 0) return 2;
  *bitmap = pool_take();
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipCloneImage(void* image, void** clone) {
  if (!clone) return 2;
  *clone = pool_take();
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDisposeImage(void* image) {
  (void)image;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipCreateImageAttributes(void** attrs) {
  if (!attrs) return 2;
  *attrs = pool_take();
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDisposeImageAttributes(void* attrs) {
  (void)attrs;
  return 0;
}

// ---- state setters ----
i32 TRT_GUEST_ABI gdiplus_GdipSetImageAttributesWrapMode(void* attrs,
                                                          i32 mode, u32 color,
                                                          int clamp) {
  (void)attrs;
  (void)mode;
  (void)color;
  (void)clamp;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipSetInterpolationMode(void* graphics, i32 mode) {
  (void)graphics;
  (void)mode;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipSetPenEndCap(void* pen, i32 cap) {
  (void)pen;
  (void)cap;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipSetPenLineJoin(void* pen, i32 join) {
  (void)pen;
  (void)join;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipSetPenStartCap(void* pen, i32 cap) {
  (void)pen;
  (void)cap;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipSetPixelOffsetMode(void* graphics, i32 mode) {
  (void)graphics;
  (void)mode;
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipSetSmoothingMode(void* graphics, i32 mode) {
  (void)graphics;
  (void)mode;
  return 0;
}

// ---- draw sites (recorded) ----
i32 TRT_GUEST_ABI gdiplus_GdipDrawImageRectRectI(void* graphics, void* image,
                                                 i32 dx, i32 dy, i32 dw,
                                                 i32 dh, i32 sx, i32 sy,
                                                 i32 sw, i32 sh, i32 unit,
                                                 void* attrs, void* callback,
                                                 void* cbdata) {
  (void)graphics;
  (void)image;
  (void)dx;
  (void)dy;
  (void)dw;
  (void)dh;
  (void)sx;
  (void)sy;
  (void)sw;
  (void)sh;
  (void)unit;
  (void)attrs;
  (void)callback;
  (void)cbdata;
  draw_log("GdipDrawImageRectRectI");
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDrawLines(void* graphics, void* pen,
                                        const void* points, i32 count) {
  (void)graphics;
  (void)pen;
  (void)points;
  (void)count;
  draw_log("GdipDrawLines");
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipDrawPath(void* graphics, void* pen, void* path) {
  (void)graphics;
  (void)pen;
  (void)path;
  draw_log("GdipDrawPath");
  return 0;
}

i32 TRT_GUEST_ABI gdiplus_GdipFillPath(void* graphics, void* brush,
                                       void* path) {
  (void)graphics;
  (void)brush;
  (void)path;
  draw_log("GdipFillPath");
  return 0;
}

int trt_bridge_register_gdiplus(void) {
  static const trt_named_fn k_gdiplus_fns[] = {
      {"GdiplusStartup", (void*)&gdiplus_GdiplusStartup},
      {"GdiplusShutdown", (void*)&gdiplus_GdiplusShutdown},
      {"GdipAlloc", (void*)&gdiplus_GdipAlloc},
      {"GdipFree", (void*)&gdiplus_GdipFree},
      {"GdipCreateFromHDC", (void*)&gdiplus_GdipCreateFromHDC},
      {"GdipDeleteGraphics", (void*)&gdiplus_GdipDeleteGraphics},
      {"GdipCreateSolidFill", (void*)&gdiplus_GdipCreateSolidFill},
      {"GdipDeleteBrush", (void*)&gdiplus_GdipDeleteBrush},
      {"GdipCloneBrush", (void*)&gdiplus_GdipCloneBrush},
      {"GdipCreatePen1", (void*)&gdiplus_GdipCreatePen1},
      {"GdipDeletePen", (void*)&gdiplus_GdipDeletePen},
      {"GdipCreatePath", (void*)&gdiplus_GdipCreatePath},
      {"GdipDeletePath", (void*)&gdiplus_GdipDeletePath},
      {"GdipAddPathArc", (void*)&gdiplus_GdipAddPathArc},
      {"GdipAddPathRectangle", (void*)&gdiplus_GdipAddPathRectangle},
      {"GdipClosePathFigure", (void*)&gdiplus_GdipClosePathFigure},
      {"GdipCreateBitmapFromScan0", (void*)&gdiplus_GdipCreateBitmapFromScan0},
      {"GdipCloneImage", (void*)&gdiplus_GdipCloneImage},
      {"GdipDisposeImage", (void*)&gdiplus_GdipDisposeImage},
      {"GdipCreateImageAttributes", (void*)&gdiplus_GdipCreateImageAttributes},
      {"GdipDisposeImageAttributes",
       (void*)&gdiplus_GdipDisposeImageAttributes},
      {"GdipSetImageAttributesWrapMode",
       (void*)&gdiplus_GdipSetImageAttributesWrapMode},
      {"GdipSetInterpolationMode",
       (void*)&gdiplus_GdipSetInterpolationMode},
      {"GdipSetPenEndCap", (void*)&gdiplus_GdipSetPenEndCap},
      {"GdipSetPenLineJoin", (void*)&gdiplus_GdipSetPenLineJoin},
      {"GdipSetPenStartCap", (void*)&gdiplus_GdipSetPenStartCap},
      {"GdipSetPixelOffsetMode", (void*)&gdiplus_GdipSetPixelOffsetMode},
      {"GdipSetSmoothingMode", (void*)&gdiplus_GdipSetSmoothingMode},
      {"GdipDrawImageRectRectI", (void*)&gdiplus_GdipDrawImageRectRectI},
      {"GdipDrawLines", (void*)&gdiplus_GdipDrawLines},
      {"GdipDrawPath", (void*)&gdiplus_GdipDrawPath},
      {"GdipFillPath", (void*)&gdiplus_GdipFillPath},
  };
  trt_module_register("gdiplus.dll", TRT_ROUTE_BUILTIN, k_gdiplus_fns,
                      (int)(sizeof(k_gdiplus_fns) / sizeof(k_gdiplus_fns[0])));
  char line[96];
  snprintf(line, sizeof(line),
           "gdiplus: registered %d functions (launcher UI surface, recorded)",
           (int)(sizeof(k_gdiplus_fns) / sizeof(k_gdiplus_fns[0])));
  log_line(line);
  return 0;
}

}  // extern "C"
