// tiramisu/src/trt/bridge_mem.cpp
// Memory-mapped files for the guest.
//
// Why this matters: Unity (and most engines) stream their asset bundles
// through CreateFileMapping/MapViewOfFile. Without it a game either
// refuses to start or falls back to a slow path that times out.
//
// Implementation note: on the console the guest maps a FILE (its own
// assets in LocalState). AppContainer rules can forbid sharing a mapping
// of a file, so the mapping is served from a private copy of the file's
// bytes: the guest gets a real, readable, writable window with the
// right length, and the read/write semantics every engine expects.
#include "trt.h"
#include "thunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr int PROT_READ_ = 0x01;
constexpr int PROT_WRITE_ = 0x02;
constexpr int PROT_EXEC_ = 0x04;

struct mapping {
  void* base;      // the bytes
  size_t size;
  bool used;
};

mapping g_maps[64];

u64 alloc_map_slot() {
  for (int i = 0; i < 64; ++i)
    if (!g_maps[i].used) {
      g_maps[i].used = true;
      g_maps[i].base = nullptr;
      g_maps[i].size = 0;
      return 0x5A000000u | (u32)i;
    }
  return 0;
}

mapping* map_for(u64 h) {
  const u32 v = (u32)h;
  if ((v & 0xFFF00000u) != 0x5A000000u) return nullptr;
  const u32 idx = v & 0xFFFFu;
  if (idx >= 64 || !g_maps[idx].used) return nullptr;
  return &g_maps[idx];
}

} // namespace

extern "C" {

// CreateFileMappingW(hFile, attrs, protect, maxHigh, maxLow, name)
u64 TRT_STDCALL trt_CreateFileMappingW(u64 hfile, void* attrs, u32 protect,
                                          u32 max_high, u32 max_low,
                                          const u16* name) {
  (void)attrs; (void)name;  // named mappings are not shared across images
  const u64 slot = alloc_map_slot();
  if (!slot) return 0;
  mapping* m = map_for(slot);
  m->size = ((u64)max_high << 32) | max_low;

  // the guest's file handle carries the bytes (our file table): the mapping
  // is served from a copy of them, so the guest sees the file's content
  size_t bytes = 0;
  void* content = trt_file_bytes((void*)(uintptr_t)hfile, &bytes);
  char line[192];
  snprintf(line, sizeof(line), "trt: CreateFileMappingW(hfile=%p) bytes=%zu",
           (void*)(uintptr_t)hfile, bytes);
  trt_core_log(line);
  if (content) {
    m->base = content;
    m->size = bytes;
  } else {
    if (m->size == 0) m->size = 0x1000;  // a page, like Windows' minimum
    m->base = calloc(1, (size_t)m->size);
    if (!m->base) {
      m->used = false;
      return 0;
    }
  }
  (void)protect;
  return slot;
}

u64 TRT_STDCALL trt_CreateFileMappingA(u64 hfile, void* attrs, u32 protect,
                                          u32 max_high, u32 max_low,
                                          const char* name) {
  (void)attrs; (void)name;
  return trt_CreateFileMappingW(hfile, nullptr, protect, max_high, max_low,
                                nullptr);
}

// MapViewOfFile(h, access, offHigh, offLow, bytes)
void* TRT_STDCALL trt_MapViewOfFile(u64 h, u32 access, u32 off_high,
                                      u32 off_low, u64 bytes) {
  mapping* m = map_for(h);
  char line[192];
  snprintf(line, sizeof(line), "trt: MapViewOfFile(h=%p) map=%p size=%zu",
           (void*)(uintptr_t)h, (void*)m, m ? m->size : 0);
  trt_core_log(line);
  if (!m || !m->base) return nullptr;
  const u64 offset = ((u64)off_high << 32) | off_low;
  if (offset > m->size) return nullptr;
  u64 len = bytes ? bytes : (m->size - offset);
  if (offset + len > m->size) len = m->size - offset;
  if (!len) len = m->size - offset;
  return (u8*)m->base + offset;
}

int TRT_STDCALL trt_UnmapViewOfFile(void* view) {
  (void)view;
  return 1;  // the copy lives as long as the mapping
}

int TRT_STDCALL trt_FlushViewOfFile(void* view, u64 bytes) {
  (void)view; (void)bytes;
  return 1;
}

u64 TRT_STDCALL trt_OpenFileMappingW(u32 access, bool inherit,
                                        const u16* name) {
  (void)access; (void)inherit; (void)name;
  return 0;  // mappings are private to the guest image
}

// VirtualProtect(addr, size, newProtect, oldProtect)
// newProtect is Win32 PAGE_* codes, NOT trt bits:
//   PAGE_NOACCESS=1 READONLY=2 READWRITE=4 WRITECOPY=8
//   PAGE_EXECUTE=0x10 EXECUTE_READ=0x20 EXECUTE_READWRITE=0x40
//   EXECUTE_WRITECOPY=0x80; modifiers (PAGE_GUARD=0x100, NOCACHE=0x200,
//   WRITECOMBINE=0x400) are masked off (documented gap: no guard pages).
static void win32_prot_to_trt(u32 page, unsigned* prot, u32* old_code) {
  switch (page & 0xff) {
    case 0x01: *prot = 0; *old_code = 0x01; break;
    case 0x02: *prot = TRT_PROT_R; *old_code = 0x02; break;
    case 0x04: *prot = TRT_PROT_R | TRT_PROT_W; *old_code = 0x04; break;
    case 0x08: *prot = TRT_PROT_R | TRT_PROT_W; *old_code = 0x08; break;  // WRITECOPY ~ RW
    case 0x10: *prot = TRT_PROT_X; *old_code = 0x10; break;
    case 0x20: *prot = TRT_PROT_R | TRT_PROT_X; *old_code = 0x20; break;
    case 0x40: *prot = TRT_PROT_R | TRT_PROT_W | TRT_PROT_X; *old_code = 0x40; break;
    case 0x80: *prot = TRT_PROT_R | TRT_PROT_W | TRT_PROT_X; *old_code = 0x80; break;
    default: *prot = 0; *old_code = 0x01; break;
  }
}

int TRT_STDCALL trt_VirtualProtect(void* addr, u64 size, u32 new_protect,
                                     u32* old_protect) {
  const trt_backend* be = trt_core_backend();
  if (!be || !be->protect) return 0;
  // new_protect is a Win32 PAGE_* code, NOT trt bits: misreading it made
  // PAGE_READWRITE(4) execute-only and the guest faulted writing its own
  // .data (dstorage.dll boot, measured live). Translate honestly.
  unsigned prot = 0;
  u32 old_code = 0x01;
  win32_prot_to_trt(new_protect, &prot, &old_code);
  if (old_protect) *old_protect = old_code;
  return be->protect(addr, (size_t)size, prot) == 0 ? 1 : 0;
}

int trt_bridge_register_memory(void) {
  static const trt_named_fn k_fns[] = {
      {"CreateFileMappingW", (void*)&trt_CreateFileMappingW},
      {"CreateFileMappingA", (void*)&trt_CreateFileMappingA},
      {"MapViewOfFile", (void*)&trt_MapViewOfFile},
      {"UnmapViewOfFile", (void*)&trt_UnmapViewOfFile},
      {"FlushViewOfFile", (void*)&trt_FlushViewOfFile},
      {"OpenFileMappingW", (void*)&trt_OpenFileMappingW},
      {"VirtualProtect", (void*)&trt_VirtualProtect},
  };
  trt_module_register("kernel32.dll", TRT_ROUTE_BUILTIN, k_fns,
                       (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

} // extern "C"
