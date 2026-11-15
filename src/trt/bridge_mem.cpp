// tiramisu/src/trt/bridge_mem.cpp
// Streaming windowed memory-mapped files for the guest.
//
// Commercial games map multi-GB asset archives (Unity bundles, Unreal .pak,
// custom containers) and inspect rolling windows via CreateFileMapping +
// MapViewOfFile(offset, bytes) + UnmapViewOfFile. On console AppContainer,
// cross-process file-mapping permissions can be restricted, so each view is
// backed by a private allocation populated exclusively for [offset, offset+len)
// via 64-bit positional I/O and released immediately on UnmapViewOfFile.
#include "trt.h"
#include "thunk.h"
#include "bridge_mem.h"

#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <vector>

extern "C" void TRT_STDCALL trt_SetLastError(u32 e);

namespace {

constexpr int PROT_READ_ = 0x01;
constexpr int PROT_WRITE_ = 0x02;
constexpr int PROT_EXEC_ = 0x04;
constexpr u32 k_map_handle_tag = 0x5A000000u;
constexpr u32 k_map_handle_mask = 0xFFF00000u;
constexpr u32 k_err_file_not_found = 2;
constexpr u32 k_err_invalid_handle = 6;
constexpr u32 k_err_invalid_parameter = 87;
constexpr u32 k_err_already_exists = 183;
constexpr u32 k_err_file_invalid = 1006;

struct mapping_core {
  void* file_handle = nullptr;  // retained VFS handle, or nullptr for anon
  u8* anon_buf = nullptr;
  u64 anon_cap = 0;
  u64 size = 0;
  u32 protect = 0x02;           // Win32 PAGE_*
  u32 handle_refs = 0;
  u32 view_refs = 0;
  std::string name;
};

struct mapping_handle {
  mapping_core* core = nullptr;
  bool used = false;
};

struct mapped_view {
  void* ptr = nullptr;
  size_t bytes = 0;
  u64 offset = 0;
  mapping_core* core = nullptr;
  bool flush_on_unmap = false;
  bool from_be = false;
};

std::mutex g_mem_mu;
std::vector<mapping_handle> g_handles;
std::vector<mapping_core*> g_cores;
std::vector<mapped_view> g_views;

std::string normalize_map_name_u16(const u16* w) {
  if (!w || !w[0]) return {};
  char utf8[512] = {};
  if (trt_utf16_to_utf8(w, utf8, sizeof(utf8)) != 0 || !utf8[0]) return {};
  for (char* p = utf8; *p; ++p) {
    if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
  }
  return std::string(utf8);
}

std::string normalize_map_name_a(const char* s) {
  if (!s || !s[0]) return {};
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  }
  return out;
}

mapping_core* find_named_core_locked(const std::string& name) {
  if (name.empty()) return nullptr;
  for (mapping_core* c : g_cores) {
    if (c && c->name == name) return c;
  }
  return nullptr;
}

u64 alloc_handle_locked(mapping_core* core) {
  for (size_t i = 0; i < g_handles.size(); ++i) {
    if (!g_handles[i].used) {
      g_handles[i].used = true;
      g_handles[i].core = core;
      ++core->handle_refs;
      return k_map_handle_tag | (u32)i;
    }
  }
  if (g_handles.size() >= 0xFFFFFu) return 0;
  mapping_handle h{};
  h.used = true;
  h.core = core;
  ++core->handle_refs;
  const u32 idx = (u32)g_handles.size();
  g_handles.push_back(h);
  return k_map_handle_tag | idx;
}

void maybe_destroy_core_locked(mapping_core* core) {
  if (!core || core->handle_refs > 0 || core->view_refs > 0) return;
  if (core->file_handle) {
    trt_file_release(core->file_handle);
    core->file_handle = nullptr;
  }
  if (core->anon_buf) {
    free(core->anon_buf);
    core->anon_buf = nullptr;
  }
  for (size_t i = 0; i < g_cores.size(); ++i) {
    if (g_cores[i] == core) {
      g_cores.erase(g_cores.begin() + (ptrdiff_t)i);
      break;
    }
  }
  delete core;
}

mapping_core* core_from_handle_locked(u64 h, u32* idx_out = nullptr) {
  if ((h & k_map_handle_mask) != k_map_handle_tag) return nullptr;
  const u32 idx = (u32)(h & 0xFFFFFu);
  if (idx >= g_handles.size() || !g_handles[idx].used) return nullptr;
  if (idx_out) *idx_out = idx;
  return g_handles[idx].core;
}

int map_access_to_prot_bits(u32 access, u32 sec_protect) {
  int bits = PROT_READ_;
  const bool want_write = (access & 0x02u) != 0 || (access == 0xF001Fu) ||
                          (access & 0x01u) != 0;
  const bool want_exec = (access & 0x20u) != 0 ||
                         ((sec_protect & 0xF0u) != 0);
  if (want_write) bits |= PROT_WRITE_;
  if (want_exec) bits |= PROT_EXEC_;
  return bits;
}

unsigned prot_bits_to_trt(int bits) {
  unsigned p = 0;
  if (bits & PROT_READ_) p |= TRT_PROT_R;
  if (bits & PROT_WRITE_) p |= TRT_PROT_W;
  if (bits & PROT_EXEC_) p |= TRT_PROT_X;
  return p ? p : TRT_PROT_R;
}

void* alloc_view_buf(size_t bytes, bool* out_from_be) {
  const trt_backend* be = trt_core_backend();
  if (be && be->alloc && be->release) {
    void* p = be->alloc(bytes, 0);
    if (p) {
      *out_from_be = true;
      return p;
    }
  }
  *out_from_be = false;
  return calloc(1, bytes);
}

void free_view_buf(void* p, size_t bytes, bool from_be) {
  if (!p) return;
  const trt_backend* be = trt_core_backend();
  if (from_be && be && be->release) {
    be->release(p, bytes);
    return;
  }
  free(p);
}

bool flush_view_locked(const mapped_view& v, size_t sub_off, size_t len) {
  if (!v.flush_on_unmap || !v.core || len == 0) return true;
  const u8* src = (const u8*)v.ptr + sub_off;
  const u64 core_off = v.offset + sub_off;
  if (v.core->file_handle) {
    size_t put = 0;
    return trt_file_pwrite(v.core->file_handle, core_off, src, len, &put) != 0;
  }
  if (v.core->anon_buf && core_off < v.core->anon_cap) {
    size_t copy_len = len;
    if (core_off + copy_len > v.core->anon_cap) {
      copy_len = (size_t)(v.core->anon_cap - core_off);
    }
    memcpy(v.core->anon_buf + core_off, src, copy_len);
  }
  return true;
}

u64 create_mapping_common(u64 hfile, u32 protect, u64 max_size,
                          const std::string& norm_name) {
  std::lock_guard<std::mutex> lk(g_mem_mu);
  if (!norm_name.empty()) {
    if (mapping_core* existing = find_named_core_locked(norm_name)) {
      const u64 h = alloc_handle_locked(existing);
      trt_SetLastError(h ? k_err_already_exists : k_err_invalid_parameter);
      return h;
    }
  }

  const bool is_anon = (hfile == (u64)-1 || hfile == 0);
  u64 file_sz = 0;
  if (!is_anon) {
    void* fh = (void*)(uintptr_t)hfile;
    if (!trt_file_stat_size(fh, &file_sz)) {
      trt_SetLastError(k_err_invalid_handle);
      return 0;
    }
    if (file_sz == 0 && max_size == 0) {
      trt_SetLastError(k_err_file_invalid);
      return 0;
    }
    if (!trt_file_retain(fh)) {
      trt_SetLastError(k_err_invalid_handle);
      return 0;
    }
  }

  mapping_core* core = new mapping_core();
  core->file_handle = is_anon ? nullptr : (void*)(uintptr_t)hfile;
  core->size = max_size ? max_size : (is_anon ? 0x1000ull : file_sz);
  core->protect = protect;
  core->name = norm_name;
  g_cores.push_back(core);

  const u64 h = alloc_handle_locked(core);
  if (!h) {
    maybe_destroy_core_locked(core);
    trt_SetLastError(k_err_invalid_parameter);
    return 0;
  }
  trt_SetLastError(0);

  char line[192];
  snprintf(line, sizeof(line),
           "trt: CreateFileMapping%s(hfile=%p) streaming size=%llu",
           norm_name.empty() ? "" : "Named",
           (void*)(uintptr_t)hfile, (unsigned long long)core->size);
  trt_core_log(line);
  return h;
}

}  // namespace

extern "C" {

int trt_mem_close_handle(void* h) {
  std::lock_guard<std::mutex> lk(g_mem_mu);
  u32 idx = 0;
  mapping_core* core = core_from_handle_locked((u64)(uintptr_t)h, &idx);
  if (!core) return 0;
  g_handles[idx].used = false;
  g_handles[idx].core = nullptr;
  if (core->handle_refs > 0) --core->handle_refs;
  maybe_destroy_core_locked(core);
  return 1;
}

void trt_mem_stats(u32* active_mappings, u32* active_views,
                   u64* total_view_bytes) {
  std::lock_guard<std::mutex> lk(g_mem_mu);
  if (active_mappings) *active_mappings = (u32)g_cores.size();
  if (active_views) *active_views = (u32)g_views.size();
  if (total_view_bytes) {
    u64 sum = 0;
    for (const mapped_view& v : g_views) sum += v.bytes;
    *total_view_bytes = sum;
  }
}

u64 TRT_STDCALL trt_CreateFileMappingW(u64 hfile, void* attrs, u32 protect,
                                       u32 max_high, u32 max_low,
                                       const u16* name) {
  (void)attrs;
  const u64 max_size = ((u64)max_high << 32) | max_low;
  return create_mapping_common(hfile, protect, max_size,
                               normalize_map_name_u16(name));
}

u64 TRT_STDCALL trt_CreateFileMappingA(u64 hfile, void* attrs, u32 protect,
                                       u32 max_high, u32 max_low,
                                       const char* name) {
  (void)attrs;
  const u64 max_size = ((u64)max_high << 32) | max_low;
  return create_mapping_common(hfile, protect, max_size,
                               normalize_map_name_a(name));
}

u64 TRT_STDCALL trt_CreateFileMappingNumaW(u64 hfile, void* attrs, u32 protect,
                                           u32 max_high, u32 max_low,
                                           const u16* name, u32 preferred) {
  (void)preferred;
  return trt_CreateFileMappingW(hfile, attrs, protect, max_high, max_low, name);
}

u64 TRT_STDCALL trt_OpenFileMappingW(u32 access, int inherit, const u16* name) {
  (void)access;
  (void)inherit;
  const std::string norm = normalize_map_name_u16(name);
  std::lock_guard<std::mutex> lk(g_mem_mu);
  mapping_core* core = find_named_core_locked(norm);
  if (!core) {
    trt_SetLastError(k_err_file_not_found);
    return 0;
  }
  trt_SetLastError(0);
  return alloc_handle_locked(core);
}

u64 TRT_STDCALL trt_OpenFileMappingA(u32 access, int inherit,
                                     const char* name) {
  (void)access;
  (void)inherit;
  const std::string norm = normalize_map_name_a(name);
  std::lock_guard<std::mutex> lk(g_mem_mu);
  mapping_core* core = find_named_core_locked(norm);
  if (!core) {
    trt_SetLastError(k_err_file_not_found);
    return 0;
  }
  trt_SetLastError(0);
  return alloc_handle_locked(core);
}

void* TRT_STDCALL trt_MapViewOfFileEx(u64 h, u32 access, u32 off_high,
                                      u32 off_low, u64 bytes, void* base_addr) {
  (void)base_addr;
  std::lock_guard<std::mutex> lk(g_mem_mu);
  mapping_core* core = core_from_handle_locked(h);
  if (!core) {
    trt_SetLastError(k_err_invalid_handle);
    return nullptr;
  }
  const u64 offset = ((u64)off_high << 32) | off_low;
  if (core->size > 0 && offset >= core->size) {
    trt_SetLastError(k_err_invalid_parameter);
    return nullptr;
  }
  u64 req_len = bytes ? bytes : (core->size > offset ? core->size - offset : 0);
  if (core->size > 0 && offset + req_len > core->size) {
    req_len = core->size - offset;
  }
  if (req_len == 0) req_len = 0x1000ull;

  bool from_be = false;
  void* view_ptr = alloc_view_buf((size_t)req_len, &from_be);
  if (!view_ptr) {
    trt_SetLastError(8 /*ERROR_NOT_ENOUGH_MEMORY*/);
    return nullptr;
  }

  if (core->file_handle) {
    size_t got = 0;
    if (!trt_file_pread(core->file_handle, offset, view_ptr, (size_t)req_len,
                        &got)) {
      free_view_buf(view_ptr, (size_t)req_len, from_be);
      trt_SetLastError(k_err_invalid_handle);
      return nullptr;
    }
    if (got < (size_t)req_len) {
      memset((u8*)view_ptr + got, 0, (size_t)req_len - got);
    }
  } else {
    // Lazily allocate shared pagefile backing for reasonable anonymous mappings
    if (!core->anon_buf && core->size <= (128ull << 20)) {
      core->anon_buf = (u8*)calloc(1, (size_t)core->size);
      if (core->anon_buf) core->anon_cap = core->size;
    }
    if (core->anon_buf && offset < core->anon_cap) {
      size_t cp = (size_t)req_len;
      if (offset + cp > core->anon_cap) cp = (size_t)(core->anon_cap - offset);
      memcpy(view_ptr, core->anon_buf + offset, cp);
    }
  }

  const int prot_bits = map_access_to_prot_bits(access, core->protect);
  const trt_backend* be = trt_core_backend();
  if (be && be->protect && prot_bits != (PROT_READ_ | PROT_WRITE_)) {
    be->protect(view_ptr, (size_t)req_len, prot_bits_to_trt(prot_bits));
  }

  // FILE_MAP_WRITE (0x02) or ALL_ACCESS (0xF001F), excluding pure COPY (0x01)
  const bool flush_back =
      ((access & 0x02u) != 0 || access == 0xF001Fu) && (access != 0x01u);

  mapped_view mv{};
  mv.ptr = view_ptr;
  mv.bytes = (size_t)req_len;
  mv.offset = offset;
  mv.core = core;
  mv.flush_on_unmap = flush_back;
  mv.from_be = from_be;
  ++core->view_refs;
  g_views.push_back(mv);

  char line[192];
  snprintf(line, sizeof(line),
           "trt: MapViewOfFile(h=%p off=0x%llx len=%llu) -> %p",
           (void*)(uintptr_t)h, (unsigned long long)offset,
           (unsigned long long)req_len, view_ptr);
  trt_core_log(line);
  trt_SetLastError(0);
  return view_ptr;
}

void* TRT_STDCALL trt_MapViewOfFile(u64 h, u32 access, u32 off_high,
                                    u32 off_low, u64 bytes) {
  return trt_MapViewOfFileEx(h, access, off_high, off_low, bytes, nullptr);
}

int TRT_STDCALL trt_FlushViewOfFile(void* view, u64 bytes) {
  if (!view) return 0;
  std::lock_guard<std::mutex> lk(g_mem_mu);
  const uintptr_t va = (uintptr_t)view;
  for (const mapped_view& mv : g_views) {
    const uintptr_t base = (uintptr_t)mv.ptr;
    if (va >= base && va < base + mv.bytes) {
      const size_t sub = (size_t)(va - base);
      size_t len = bytes ? (size_t)bytes : (mv.bytes - sub);
      if (sub + len > mv.bytes) len = mv.bytes - sub;
      return flush_view_locked(mv, sub, len) ? 1 : 0;
    }
  }
  return 0;
}

int TRT_STDCALL trt_UnmapViewOfFile(void* view) {
  if (!view) return 0;
  std::lock_guard<std::mutex> lk(g_mem_mu);
  for (size_t i = 0; i < g_views.size(); ++i) {
    if (g_views[i].ptr == view) {
      const mapped_view mv = g_views[i];
      g_views.erase(g_views.begin() + (ptrdiff_t)i);
      if (mv.flush_on_unmap) flush_view_locked(mv, 0, mv.bytes);
      free_view_buf(mv.ptr, mv.bytes, mv.from_be);
      if (mv.core && mv.core->view_refs > 0) --mv.core->view_refs;
      maybe_destroy_core_locked(mv.core);
      return 1;
    }
  }
  return 0;
}

static void win32_prot_to_trt(u32 page, unsigned* prot, u32* old_code) {
  switch (page & 0xff) {
    case 0x01: *prot = 0; *old_code = 0x01; break;
    case 0x02: *prot = TRT_PROT_R; *old_code = 0x02; break;
    case 0x04: *prot = TRT_PROT_R | TRT_PROT_W; *old_code = 0x04; break;
    case 0x08: *prot = TRT_PROT_R | TRT_PROT_W; *old_code = 0x08; break;
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
  if (!addr || size == 0) {
    trt_SetLastError(k_err_invalid_parameter);
    return 0;
  }
  const uintptr_t raw = (uintptr_t)addr;
  const uintptr_t page = raw & ~(uintptr_t)4095ULL;
  const size_t span = (size_t)((raw - page) + (size_t)size);
  const size_t aligned = (span + 4095ULL) & ~(size_t)4095ULL;
  unsigned prot = 0;
  u32 old_code = 0x04;
  win32_prot_to_trt(new_protect, &prot, &old_code);
  const int rc = be->protect((void*)page, aligned, prot);
  if (rc != 0) {
    // W^X two-phase JIT fallback: when host denies RWX (0x40/0x80), arm as RW
    // and promote to RX on FlushInstructionCache or first NX fault.
    if ((new_protect & 0xFFu) == 0x40u || (new_protect & 0xFFu) == 0x80u) {
      if (be->protect((void*)page, aligned, TRT_PROT_R | TRT_PROT_W) == 0) {
        u32 prev = old_code;
        if (trt_va_mark_pending_rx((void*)page, aligned, &prev)) {
          trt_core_log("kernel32: VirtualProtect(RWX) -> W^X two-phase JIT (armed RW, flips RX on FlushInstructionCache)");
          if (old_protect) *old_protect = prev;
          return 1;
        }
      }
    }
    if (new_protect & 0xF0u) {
      trt_core_log("kernel32: VirtualProtect(EXEC) rejected by host backend");
    }
    trt_SetLastError(87);
    return 0;
  }
  u32 prev = old_code;
  trt_va_notify_protect((void*)page, aligned, new_protect & 0xFFu, &prev);
  if (old_protect) *old_protect = prev;
  return 1;
}

static bool is_current_proc_handle(void* proc) {
  const uintptr_t v = (uintptr_t)proc;
  return proc == nullptr || v == (uintptr_t)(intptr_t)-1 || v == 0xFFFFFFFFULL;
}

int TRT_STDCALL trt_VirtualProtectEx(void* proc, void* addr, u64 size,
                                     u32 new_protect, u32* old_protect) {
  if (!is_current_proc_handle(proc)) {
    trt_SetLastError(5);  // ERROR_ACCESS_DENIED
    return 0;
  }
  return trt_VirtualProtect(addr, size, new_protect, old_protect);
}

extern "C" void* TRT_STDCALL trt_VirtualAlloc(void* addr, u64 size, u32 type, u32 prot);
extern "C" int TRT_STDCALL trt_VirtualFree(void* addr, u64 bytes, u32 type);
extern "C" u64 TRT_STDCALL trt_VirtualQuery(const void* addr, void* mbi_out, u64 len);

void* TRT_STDCALL trt_VirtualAllocEx(void* proc, void* addr, u64 size,
                                     u32 type, u32 prot) {
  if (!is_current_proc_handle(proc)) {
    trt_SetLastError(5);
    return nullptr;
  }
  return trt_VirtualAlloc(addr, size, type, prot);
}

void* TRT_STDCALL trt_VirtualAlloc2(void* proc, void* base_addr, u64 size,
                                    u32 type, u32 prot, void* ext_params,
                                    u32 param_count) {
  (void)ext_params;
  (void)param_count;
  return trt_VirtualAllocEx(proc, base_addr, size, type, prot);
}

void* TRT_STDCALL trt_VirtualAlloc2FromApp(void* proc, void* base_addr, u64 size,
                                           u32 type, u32 prot, void* ext_params,
                                           u32 param_count) {
  (void)ext_params;
  (void)param_count;
  return trt_VirtualAllocEx(proc, base_addr, size, type, prot);
}

int TRT_STDCALL trt_VirtualFreeEx(void* proc, void* addr, u64 bytes, u32 type) {
  if (!is_current_proc_handle(proc)) {
    trt_SetLastError(5);
    return 0;
  }
  return trt_VirtualFree(addr, bytes, type);
}

u64 TRT_STDCALL trt_VirtualQueryEx(void* proc, const void* addr, void* mbi_out,
                                   u64 len) {
  if (!is_current_proc_handle(proc)) {
    trt_SetLastError(5);
    return 0;
  }
  return trt_VirtualQuery(addr, mbi_out, len);
}

int trt_bridge_register_memory(void) {
  static const trt_named_fn k_fns[] = {
      {"CreateFileMappingW", (void*)&trt_CreateFileMappingW},
      {"CreateFileMappingA", (void*)&trt_CreateFileMappingA},
      {"CreateFileMappingNumaW", (void*)&trt_CreateFileMappingNumaW},
      {"MapViewOfFile", (void*)&trt_MapViewOfFile},
      {"MapViewOfFileEx", (void*)&trt_MapViewOfFileEx},
      {"UnmapViewOfFile", (void*)&trt_UnmapViewOfFile},
      {"FlushViewOfFile", (void*)&trt_FlushViewOfFile},
      {"OpenFileMappingW", (void*)&trt_OpenFileMappingW},
      {"OpenFileMappingA", (void*)&trt_OpenFileMappingA},
      {"VirtualProtect", (void*)&trt_VirtualProtect},
      {"VirtualProtectEx", (void*)&trt_VirtualProtectEx},
      {"VirtualAllocEx", (void*)&trt_VirtualAllocEx},
      {"VirtualAlloc2", (void*)&trt_VirtualAlloc2},
      {"VirtualAlloc2FromApp", (void*)&trt_VirtualAlloc2FromApp},
      {"VirtualFreeEx", (void*)&trt_VirtualFreeEx},
      {"VirtualQueryEx", (void*)&trt_VirtualQueryEx},
  };
  trt_module_register("kernel32.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  trt_module_register("kernelbase.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

}  // extern "C"
