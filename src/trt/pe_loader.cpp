// tiramisu/src/trt/pe_loader.cpp
// Tiramisu Runtime PE64 loader core. Wine-style in-process mapping:
//   validate -> alloc -> headers+sections -> relocate -> bind imports -> TLS
// Zero RWX: everything maps RW; trt_seal_image() flips code to RX.
// Bounds-checked throughout: a malformed PE must fail with a named error,
// never crash the host process (the host IS the console app).
#include "trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

u16 rd16(const u8* p) { u16 v; memcpy(&v, p, 2); return v; }
u32 rd32(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }
u64 rd64(const u8* p) { u64 v; memcpy(&v, p, 8); return v; }

void logf_be(const trt_backend* be, const char* msg) {
  if (be && be->log) be->log(msg);
}

// fail: record stage detail, release mapping, return error code
int fail(trt_image* img, const trt_backend* be, int code, const char* detail) {
  if (detail && img) {
    strncpy(img->last_detail, detail, sizeof(img->last_detail) - 1);
    img->last_detail[sizeof(img->last_detail) - 1] = 0;
  }
  char line[320];
  snprintf(line, sizeof(line), "trt: FAIL %s: %s", trt_errstr(code),
           detail ? detail : "");
  logf_be(be, line);
  if (img && img->base && be) {
    be->release(img->base, (size_t)img->size_of_image);
    img->base = nullptr;
  }
  return code;
}

// section target protection per PE characteristics (pre-seal everything RW;
// seal flips exec to RX)
unsigned section_prot(bool exec) { return exec ? (TRT_PROT_R | TRT_PROT_X)
                                               : (TRT_PROT_R | TRT_PROT_W); }

} // namespace

extern "C" const char* trt_errstr(int code) {
  switch (code) {
    case TRT_OK: return "OK";
    case TRT_ERR_BAD_DOS: return "BAD_DOS (not MZ / e_lfanew out of file)";
    case TRT_ERR_BAD_PE: return "BAD_PE (no PE signature)";
    case TRT_ERR_NOT_PE64: return "NOT_PE64 (machine/magic mismatch, 32-bit needs the x86 engine, not this path)";
    case TRT_ERR_NO_SECTIONS: return "NO_SECTIONS";
    case TRT_ERR_SIZE_OVERFLOW: return "SIZE_OVERFLOW (SizeOfImage missing or over 2 GB cap)";
    case TRT_ERR_SECTION_BOUNDS: return "SECTION_BOUNDS (section points outside file/image)";
    case TRT_ERR_NO_BASE: return "NO_BASE (fixed base required but unavailable)";
    case TRT_ERR_ALLOC: return "ALLOC (backend allocation failed)";
    case TRT_ERR_RELOC_BADTYPE: return "RELOC_BADTYPE";
    case TRT_ERR_RELOC_BOUNDS: return "RELOC_BOUNDS";
    case TRT_ERR_IMPORT: return "IMPORT (unresolvable symbol)";
    case TRT_ERR_TLS_BOUNDS: return "TLS_BOUNDS";
    case TRT_ERR_SEAL: return "SEAL (final protect failed)";
    case TRT_ERR_DELAY_IMPORT: return "DELAY_IMPORT (delay-load directory not processed in v0)";
    default: return "UNKNOWN";
  }
}

extern "C" int trt_map_pe(const trt_backend* be, const trt_resolver* rs,
                           const u8* file, size_t file_size, trt_image** out) {
  if (!be || !rs || !file || !out || file_size < sizeof(trt_dos_header))
    return TRT_ERR_BAD_ARGS;
  *out = nullptr;

  trt_image img;
  memset(&img, 0, sizeof(img));

  // ---- 1. DOS header ----
  const trt_dos_header* dos = (const trt_dos_header*)file;
  if (rd16(file) != 0x5A4D) return fail(&img, be, TRT_ERR_BAD_DOS, "no MZ");
  u64 nt_off = dos->e_lfanew;
  if (nt_off < sizeof(trt_dos_header) || nt_off + sizeof(trt_nt_headers64) > file_size)
    return fail(&img, be, TRT_ERR_BAD_DOS, "e_lfanew out of file");

  // ---- 2. NT headers ----
  const trt_nt_headers64* nt = (const trt_nt_headers64*)(file + nt_off);
  if (rd32((const u8*)&nt->signature) != 0x00004550)
    return fail(&img, be, TRT_ERR_BAD_PE, "no PE\\0\\0");
  if (rd16((const u8*)&nt->file_header.machine) != TRT_PE_MACHINE_X64 ||
      rd16((const u8*)&nt->optional_header.magic) != TRT_PE_OPT64_MAGIC)
    return fail(&img, be, TRT_ERR_NOT_PE64, "only PE64 (0x8664/0x020b) on this path");
  u32 nsec = rd16((const u8*)&nt->file_header.number_of_sections);
  if (nsec == 0 || nsec > 96)
    return fail(&img, be, TRT_ERR_NO_SECTIONS, "section count 0 or >96");
  u64 size_of_image = rd32((const u8*)&nt->optional_header.size_of_image);
  u64 section_alignment = rd32((const u8*)&nt->optional_header.section_alignment);
  if (size_of_image == 0 || size_of_image > TRT_PE_MAX_IMAGE_BYTES ||
      section_alignment < 4096)
    return fail(&img, be, TRT_ERR_SIZE_OVERFLOW, "SizeOfImage/alignment insane");

  const u64 preferred = rd64((const u8*)&nt->optional_header.image_base);
  const u64 entry_rva = rd32((const u8*)&nt->optional_header.address_of_entry_point);
  const u64 size_headers = rd32((const u8*)&nt->optional_header.size_of_headers);
  const trt_data_directory* dirs = nt->optional_header.directories;

  // section headers live right after optional header, in the file
  const u8* sec_base = (const u8*)&nt->optional_header +
                       rd16((const u8*)&nt->file_header.size_of_optional_header);
  if ((size_t)(sec_base - file) + (size_t)nsec * sizeof(trt_section_header) > file_size)
    return fail(&img, be, TRT_ERR_SECTION_BOUNDS, "section headers out of file");

  // ---- 3. allocate ----
  const bool has_relocs = dirs[TRT_PE_DIR_RELOC].virtual_address != 0 &&
                          dirs[TRT_PE_DIR_RELOC].size != 0;
  void* base = be->alloc((size_t)size_of_image, preferred);
  if (!base && has_relocs) {           // retry anywhere; relocs will fix it up
    base = be->alloc((size_t)size_of_image, 0);
    if (base) logf_be(be, "trt: preferred base unavailable, relocated");
  }
  if (!base)
    return fail(&img, be, has_relocs ? TRT_ERR_ALLOC : TRT_ERR_NO_BASE,
                "VirtualAlloc/map failed");
  memset(base, 0, (size_t)size_of_image);

  img.base = (u8*)base;
  img.preferred_base = preferred;
  img.size_of_image = size_of_image;
  img.entry_rva = entry_rva;
  img.section_count = nsec;

  // ---- 4. headers + sections ----
  if (size_headers > size_of_image || size_headers > file_size)
    return fail(&img, be, TRT_ERR_SECTION_BOUNDS, "SizeOfHeaders out of bounds");
  memcpy(img.base, file, (size_t)size_headers);

  u64 max_end = size_headers;
  for (u32 i = 0; i < nsec; ++i) {
    const trt_section_header* sh =
        (const trt_section_header*)(sec_base + (size_t)i * sizeof(trt_section_header));
    const u64 va = rd32((const u8*)&sh->virtual_address);
    const u64 vsize = rd32((const u8*)&sh->virtual_size);
    const u64 rsize = rd32((const u8*)&sh->size_of_raw_data);
    const u64 roff = rd32((const u8*)&sh->pointer_to_raw_data);
    if (va + vsize > size_of_image)
      return fail(&img, be, TRT_ERR_SECTION_BOUNDS, "section VA+VSize > SizeOfImage");
    if (rsize) {
      if (roff + rsize > file_size)
        return fail(&img, be, TRT_ERR_SECTION_BOUNDS, "section raw data out of file");
      memcpy(img.base + va, file + roff, (size_t)rsize);
    }
    // BSS tail already zero from memset
    if (va + vsize > max_end) max_end = va + vsize;
    char line[96];
    snprintf(line, sizeof(line), "trt: sec[%u] %.8s va=%llx sz=%llx%s",
             i, (const char*)sh->name, (unsigned long long)va,
             (unsigned long long)vsize, (rd32((const u8*)&sh->characteristics) & TRT_SCN_MEM_EXEC) ? " exec" : "");
    logf_be(be, line);
  }
  if (max_end > size_of_image)
    return fail(&img, be, TRT_ERR_SECTION_BOUNDS, "sections exceed SizeOfImage");

  const u64 delta = (u64)(uintptr_t)img.base - preferred;

  // ---- 5. relocations (only when loaded off preferred base) ----
  if (delta != 0) {
    if (!has_relocs)
      return fail(&img, be, TRT_ERR_NO_BASE, "delta != 0 and no reloc directory");
    const u64 dir_va = dirs[TRT_PE_DIR_RELOC].virtual_address;
    const u64 dir_sz = dirs[TRT_PE_DIR_RELOC].size;
    if (dir_va + dir_sz > size_of_image)
      return fail(&img, be, TRT_ERR_RELOC_BOUNDS, "reloc dir out of image");
    u64 off = 0;
    u32 n_fixed = 0;
    while (off + 8 <= dir_sz) {
      const u8* blk = img.base + dir_va + off;
      const u32 page_rva = rd32(blk);
      const u32 block_size = rd32(blk + 4);
      if (block_size < 8 || off + block_size > dir_sz)
        return fail(&img, be, TRT_ERR_RELOC_BOUNDS, "reloc block size corrupt");
      const u32 n_entries = (block_size - 8) / 2;
      for (u32 e = 0; e < n_entries; ++e) {
        const u16 entry = rd16(blk + 8 + (size_t)e * 2);
        const u32 type = entry >> 12;
        const u32 fo = entry & 0x0FFF;
        if (type == TRT_RELOC_ABSOLUTE) continue;
        const u64 tgt_rva = page_rva + fo;
        if (tgt_rva + 8 > size_of_image)
          return fail(&img, be, TRT_ERR_RELOC_BOUNDS, "reloc target out of image");
        u8* p = img.base + tgt_rva;
        if (type == TRT_RELOC_DIR64) {
          u64 v = rd64(p); v += delta; memcpy(p, &v, 8);
        } else if (type == TRT_RELOC_HIGHLOW) {
          if (tgt_rva + 4 > size_of_image)
            return fail(&img, be, TRT_ERR_RELOC_BOUNDS, "HIGHLOW out of image");
          u32 v = rd32(p); v += (u32)delta; memcpy(p, &v, 4);
        } else {
          char d[64]; snprintf(d, sizeof(d), "reloc type %u unsupported", type);
          return fail(&img, be, TRT_ERR_RELOC_BADTYPE, d);
        }
        ++n_fixed;
      }
      off += block_size;
    }
    char line[96];
    snprintf(line, sizeof(line), "trt: reloc delta=%llx fixed=%u",
             (unsigned long long)delta, n_fixed);
    logf_be(be, line);
  } else {
    logf_be(be, "trt: loaded at preferred base, no reloc pass");
  }

  // ---- 6. imports ----
  const u64 imp_va = dirs[TRT_PE_DIR_IMPORT].virtual_address;
  const u64 imp_sz = dirs[TRT_PE_DIR_IMPORT].size;
  u32 n_modules = 0, n_symbols = 0;
  if (imp_va && imp_sz) {
    if (imp_va + imp_sz > size_of_image)
      return fail(&img, be, TRT_ERR_IMPORT, "import dir out of image");
    for (u64 d = 0; d + sizeof(trt_import_descriptor) <= imp_sz;
         d += sizeof(trt_import_descriptor)) {
      const trt_import_descriptor* desc =
          (const trt_import_descriptor*)(img.base + imp_va + d);
      if (!desc->first_thunk && !desc->name) break;  // terminator
      if (desc->name >= size_of_image || desc->first_thunk >= size_of_image)
        return fail(&img, be, TRT_ERR_IMPORT, "import descriptor out of image");
      const char* mod_name = (const char*)(img.base + desc->name);
      const u64 iat_rva = desc->first_thunk;
      u32 bound = 0;
      for (u64 t = iat_rva;; t += 8) {
        if (t + 8 > size_of_image)
          return fail(&img, be, TRT_ERR_IMPORT, "IAT runs off image");
        const u64 thunk = rd64(img.base + t);
        if (thunk == 0) break;  // terminator
        const char* sym_name = nullptr;
        u16 ordinal = 0;
        if (thunk >> 63) {
          ordinal = (u16)(thunk & 0xFFFF);
        } else {
          if (thunk + 2 >= size_of_image)
            return fail(&img, be, TRT_ERR_IMPORT, "hint/name out of image");
          sym_name = (const char*)(img.base + thunk + 2);
          if (strnlen(sym_name, 256) == 256)
            return fail(&img, be, TRT_ERR_IMPORT, "symbol name not terminated");
        }
        void* fn = rs->resolve(rs->user, mod_name, sym_name, ordinal);
        if (!fn) {
          char det[224];
          snprintf(det, sizeof(det), "%s!%s", mod_name,
                   sym_name ? sym_name : "#ordinal");
          return fail(&img, be, TRT_ERR_IMPORT, det);
        }
        memcpy(img.base + t, &fn, 8);  // bind IAT
        ++bound; ++n_symbols;
      }
      char line[160];
      snprintf(line, sizeof(line), "trt: import %s bound=%u", mod_name, bound);
      logf_be(be, line);
      ++n_modules;
    }
  } else {
    logf_be(be, "trt: no import directory");
  }
  (void)n_modules; (void)n_symbols;

  // ---- 7. TLS callbacks (collected; caller runs them after seal) ----
  const u64 tls_va = dirs[TRT_PE_DIR_TLS].virtual_address;
  if (tls_va) {
    if (tls_va + sizeof(trt_tls_directory64) > size_of_image)
      return fail(&img, be, TRT_ERR_TLS_BOUNDS, "tls dir out of image");
    const trt_tls_directory64* tls = (const trt_tls_directory64*)(img.base + tls_va);
    // callbacks array is addressed by VA; after relocation it points at the
    // runtime image, so translate through the actual base.
    const u64 arr_rva = (u64)tls->address_of_callbacks - preferred;
    if (arr_rva + 8 > size_of_image)
      return fail(&img, be, TRT_ERR_TLS_BOUNDS, "tls callback array out of image");
    for (u64 c = arr_rva; c + 8 <= size_of_image &&
                          img.tls_callback_count < TRT_MAX_TLS_CALLBACKS; c += 8) {
      const u64 cb = rd64(img.base + c);
      if (cb == 0) break;
      const u64 cb_rva = cb - (u64)(uintptr_t)img.base;
      if (cb_rva >= size_of_image)
        return fail(&img, be, TRT_ERR_TLS_BOUNDS, "tls callback out of image");
      img.tls_callback_rvas[img.tls_callback_count++] = cb_rva;
    }
    char line[96];
    snprintf(line, sizeof(line), "trt: tls callbacks=%d", img.tls_callback_count);
    logf_be(be, line);
  }

  // ---- 8. delay imports: fail-visible until v0.2 ----
  if (dirs[TRT_PE_DIR_DELAY_IMPORT].virtual_address)
    logf_be(be, "trt: NOTE delay-import directory present (not processed in v0)");

  *out = (trt_image*)malloc(sizeof(trt_image));
  if (!*out) return fail(&img, be, TRT_ERR_ALLOC, "image struct alloc");
  memcpy(*out, &img, sizeof(img));
  char line[160];
  snprintf(line, sizeof(line),
           "trt: MAPPED base=%llx preferred=%llx size=%llx entry=%llx",
           (unsigned long long)(uintptr_t)img.base,
           (unsigned long long)preferred,
           (unsigned long long)size_of_image, (unsigned long long)entry_rva);
  logf_be(be, line);
  return TRT_OK;
}

extern "C" int trt_seal_image(const trt_backend* be, trt_image* img) {
  if (!be || !img || !img->base) return TRT_ERR_BAD_ARGS;
  // walk section table in the mapped copy of the headers (NT headers sit at
  // e_lfanew, not at offset 0 — the DOS header occupies the page start)
  const trt_dos_header* dos = (const trt_dos_header*)img->base;
  const trt_nt_headers64* nt = (const trt_nt_headers64*)(img->base + dos->e_lfanew);
  const u8* sec_base = (const u8*)&nt->optional_header +
                       rd16((const u8*)&nt->file_header.size_of_optional_header);
  for (u32 i = 0; i < img->section_count; ++i) {
    const trt_section_header* sh =
        (const trt_section_header*)(sec_base + (size_t)i * sizeof(trt_section_header));
    const u32 ch = rd32((const u8*)&sh->characteristics);
    const u64 va = rd32((const u8*)&sh->virtual_address);
    const u64 vsize = rd32((const u8*)&sh->virtual_size);
    const bool exec = (ch & TRT_SCN_MEM_EXEC) != 0;
    const unsigned prot = section_prot(exec);
    if (be->protect(img->base + va, (size_t)vsize ? (size_t)vsize : 4096, prot)) {
      char d[96];
      snprintf(d, sizeof(d), "protect failed on section %u (prot=%u)", i, prot);
      return fail(img, be, TRT_ERR_SEAL, d);
    }
  }
  logf_be(be, "trt: SEALED (W^X: code pages now RX, no RWX ever)");
  return TRT_OK;
}

extern "C" void trt_unmap(const trt_backend* be, trt_image* img) {
  if (!be || !img) return;
  if (img->base) be->release(img->base, (size_t)img->size_of_image);
  free(img);
}
