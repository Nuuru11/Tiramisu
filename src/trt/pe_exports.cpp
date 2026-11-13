// tiramisu/src/trt/pe_exports.cpp
// PE64 export directory parsing.
//
// Two callers need this:
//   1. GetProcAddress on a DLL the guest loaded itself
//   2. binding an import from another guest DLL (engine.dll -> utils.dll)
//
// Structure (IMAGE_EXPORT_DIRECTORY, 40 bytes):
//   +0  Characteristics          +12 Name              (RVA of the DLL name)
//   +4  TimeDateStamp            +16 Base              (ordinal base)
//   +8  Major/MinorVersion       +20 NumberOfFunctions
//   +10 (pad)                    +24 NumberOfNames
//   +28 AddressOfFunctions       +32 AddressOfNames
//   +36 AddressOfNameOrdinals
// A function RVA that points back into the export directory is a
// FORWARDER ("KERNEL32.Sleep"): we resolve it through the normal resolver.
#include "pe_exports.h"

#include <stdio.h>
#include <string.h>

namespace {

u32 rd32(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }
u16 rd16(const u8* p) { u16 v; memcpy(&v, p, 2); return v; }

struct export_dir {
  u32 rva;
  u32 size;
  u32 base;
  u32 num_functions;
  u32 num_names;
  u32 addr_functions;
  u32 addr_names;
  u32 addr_name_ordinals;
};

bool read_export_dir(const trt_image* img, export_dir* out) {
  if (!img || !img->base) return false;
  const u8* base = img->base;
  const u32 lfanew = rd32(base + 0x3C);
  if (lfanew + sizeof(trt_nt_headers64) > img->size_of_image) return false;
  const trt_nt_headers64* nt = (const trt_nt_headers64*)(base + lfanew);
  const trt_data_directory* dir = &nt->optional_header.directories[TRT_PE_DIR_EXPORT];
  if (!dir->virtual_address || !dir->size) return false;
  if (dir->virtual_address + dir->size > img->size_of_image) return false;
  const u8* ed = base + dir->virtual_address;
  out->rva = dir->virtual_address;
  out->size = dir->size;
  out->base = rd32(ed + 16);
  out->num_functions = rd32(ed + 20);
  out->num_names = rd32(ed + 24);
  out->addr_functions = rd32(ed + 28);
  out->addr_names = rd32(ed + 32);
  out->addr_name_ordinals = rd32(ed + 36);
  return out->num_functions <= (1u << 20) && out->num_names <= (1u << 20);
}

} // namespace

extern "C" {

int trt_pe_has_export(const trt_image* img, const char* name) {
  export_dir ed;
  if (!name || !read_export_dir(img, &ed)) return 0;
  const u8* base = img->base;
  for (u32 i = 0; i < ed.num_names; ++i) {
    const u32 name_rva = rd32(base + ed.addr_names + i * 4);
    if (name_rva >= img->size_of_image) continue;
    if (strcmp((const char*)(base + name_rva), name) == 0) return 1;
  }
  return 0;
}

void* trt_pe_find_export(const trt_backend* be, const trt_resolver* rs,
                         const trt_image* img, const char* name, u16 ordinal) {
  (void)be;
  export_dir ed;
  if (!read_export_dir(img, &ed)) return nullptr;
  const u8* base = img->base;

  u32 index = 0;
  bool have_index = false;

  if (name && *name) {
    // the name table is sorted: binary search
    u32 lo = 0, hi = ed.num_names;
    while (lo < hi) {
      const u32 mid = lo + (hi - lo) / 2;
      const u32 name_rva = rd32(base + ed.addr_names + mid * 4);
      if (name_rva >= img->size_of_image) { hi = mid; continue; }
      const int cmp = strcmp((const char*)(base + name_rva), name);
      if (cmp == 0) {
        const u16 ord = rd16(base + ed.addr_name_ordinals + mid * 2);
        index = ord;
        have_index = true;
        break;
      }
      if (cmp < 0) lo = mid + 1; else hi = mid;
    }
    if (!have_index) return nullptr;
  } else {
    if (ordinal < ed.base) return nullptr;
    index = (u32)ordinal - ed.base;
    have_index = true;
  }
  if (!have_index || index >= ed.num_functions) return nullptr;

  const u32 func_rva = rd32(base + ed.addr_functions + index * 4);
  if (func_rva == 0) return nullptr;
  if (func_rva >= img->size_of_image) return nullptr;

  // forwarder? (the RVA points into the export directory itself)
  if (func_rva >= ed.rva && func_rva < ed.rva + ed.size) {
    const char* fwd = (const char*)(base + func_rva);
    const char* dot = strchr(fwd, '.');
    if (!dot || !rs || !rs->resolve) return nullptr;
    char mod[128];
    size_t mod_len = (size_t)(dot - fwd);
    if (mod_len >= sizeof(mod)) return nullptr;
    memcpy(mod, fwd, mod_len);
    mod[mod_len] = 0;
    return rs->resolve(rs->user, mod, dot + 1, 0);
  }

  return (void*)(base + func_rva);
}

} // extern "C"
