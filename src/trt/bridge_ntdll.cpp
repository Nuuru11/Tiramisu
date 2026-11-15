// tiramisu/src/trt/bridge_ntdll.cpp
// Minimal honest ntdll.dll surface: Rtl unwind/status/version, file query/write,
// and ETW provider no-ops. api-ms-win-eventing-provider-* and
// api-ms-win-core-rtlsupport-* route here via apiset.cpp.
#include "bridge_ntdll.h"
#include "thunk.h"

#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_set>

extern "C" {

// Reuse existing Rtl/memory implementations from bridge_kernel32 / bridge_mem.
void* TRT_STDCALL trt_RtlCaptureContext(void* ctx);
void* TRT_STDCALL trt_RtlLookupFunctionEntry(u64 ctrlpc, u64* base_out,
                                             void* history);
u64 TRT_STDCALL trt_RtlVirtualUnwind(u32 handler_type, u64 image_base,
                                     u64 ctrl_pc, void* fn_entry, void* ctx,
                                     void** handler_data, u64* establisher,
                                     void* ctx_ptrs);
void* TRT_STDCALL trt_RtlPcToFileHeader(u64 pc, u64* base_out);
u8 TRT_STDCALL trt_RtlAddFunctionTable(void* fn_table, u32 entry_count,
                                       u64 base_addr);
u8 TRT_STDCALL trt_RtlDeleteFunctionTable(void* fn_table);
void TRT_STDCALL trt_RtlUnwindEx(u64 target, u64 target_ip, void* exrec,
                                 u64 retval, void* ctx, void* history);
void TRT_STDCALL trt_RtlUnwind(u64 target, u64 target_ip, void* exrec,
                               u64 retval);
void* TRT_STDCALL trt_VirtualAlloc2(void* proc, void* base_addr, u64 size,
                                    u32 type, u32 prot, void* ext_params,
                                    u32 param_count);
int TRT_STDCALL trt_VirtualFree(void* addr, u64 bytes, u32 type);
int TRT_STDCALL trt_VirtualProtect(void* addr, u64 bytes, u32 prot, u32* old_prot);
u32 TRT_STDCALL trt_GetLastError(void);
void TRT_STDCALL trt_SetLastError(u32 e);
int TRT_STDCALL trt_MultiByteToWideChar(u32 cp, u32 flags, const char* src,
                                        i32 src_len, u16* dst, i32 dst_len);
int TRT_STDCALL trt_WideCharToMultiByte(u32 cp, u32 flags, const u16* src,
                                        i32 src_len, char* dst, i32 dst_len,
                                        const char* defchar, void* used_def);

FILE* trt_file_os(void* h);
const char* trt_file_vpath(void* h);
int TRT_STDCALL trt_CloseHandle(void* h);
int TRT_STDCALL trt_VerifyVersionInfoW(void* vi, u32 type_mask, u64 cond_mask);

namespace {

constexpr u32 k_status_ok = 0x00000000u;
constexpr u32 k_status_not_implemented = 0xC0000002u;
constexpr u32 k_status_invalid_handle = 0xC0000008u;
constexpr u32 k_status_invalid_parameter = 0xC000000Du;
constexpr u32 k_status_buffer_overflow = 0x80000005u;

u32 TRT_GUEST_ABI nt_RtlNtStatusToDosError(u32 status) {
  switch (status) {
    case 0x00000000u: return 0;     // ERROR_SUCCESS
    case 0xC0000002u: return 50;    // ERROR_NOT_SUPPORTED
    case 0xC0000008u: return 6;     // ERROR_INVALID_HANDLE
    case 0xC000000Du: return 87;    // ERROR_INVALID_PARAMETER
    case 0xC000000Fu: return 2;     // ERROR_FILE_NOT_FOUND
    case 0xC0000017u: return 8;     // ERROR_NOT_ENOUGH_MEMORY
    case 0xC0000022u: return 5;     // ERROR_ACCESS_DENIED
    case 0xC0000034u: return 2;     // ERROR_FILE_NOT_FOUND
    case 0xC0000035u: return 80;    // ERROR_FILE_EXISTS
    case 0xC000009Au: return 8;     // ERROR_NOT_ENOUGH_MEMORY
    case 0xC00000BAu: return 5;     // ERROR_ACCESS_DENIED (is directory)
    case 0xC00000BBu: return 50;    // ERROR_NOT_SUPPORTED
    case 0x80000005u: return 234;   // ERROR_MORE_DATA
    case 0x80000006u: return 18;    // ERROR_NO_MORE_FILES
    case 0x8000001Au: return 259;   // ERROR_NO_MORE_ITEMS
    default:
      if ((status & 0xFFFF0000u) == 0x80070000u) return status & 0xFFFFu;
      return 317;                   // ERROR_MR_MID_NOT_FOUND
  }
}

u32 TRT_GUEST_ABI nt_RtlGetVersion(void* lpvi) {
  if (!lpvi) return k_status_invalid_parameter;
  u32* d = (u32*)lpvi;
  const u32 sz = d[0];
  if (sz < 20) return k_status_invalid_parameter;
  memset(d + 1, 0, sz - 4);
  d[1] = 10;     // dwMajorVersion
  d[2] = 0;      // dwMinorVersion
  d[3] = 19045;  // dwBuildNumber (Win10 / Xbox Series shell era)
  d[4] = 2;      // VER_PLATFORM_WIN32_NT
  if (sz >= 284) {
    u8* b = (u8*)lpvi;
    b[282] = 1;  // wProductType = VER_NT_WORKSTATION
  }
  return k_status_ok;
}

u32 TRT_GUEST_ABI nt_RtlVerifyVersionInfo(void* vi, u32 type_mask, u64 cond_mask) {
  if (!vi) return k_status_invalid_parameter;
  return trt_VerifyVersionInfoW(vi, type_mask, cond_mask) ? k_status_ok
                                                          : 0xC0000059u;  // STATUS_REVISION_MISMATCH
}

u32 TRT_GUEST_ABI nt_NtClose(void* h) {
  if (!h || h == (void*)(uintptr_t)-1) return k_status_invalid_handle;
  return trt_CloseHandle(h) ? k_status_ok : k_status_invalid_handle;
}

struct io_status_block {
  u64 status;
  u64 info;
};

u32 TRT_GUEST_ABI nt_NtQueryInformationFile(void* h, void* iosb_ptr,
                                            void* info, u32 len, u32 cls) {
  FILE* os = trt_file_os(h);
  io_status_block* iosb = (io_status_block*)iosb_ptr;
  if (!os) {
    if (iosb) { iosb->status = k_status_invalid_handle; iosb->info = 0; }
    return k_status_invalid_handle;
  }
  if (!info) {
    if (iosb) { iosb->status = k_status_invalid_parameter; iosb->info = 0; }
    return k_status_invalid_parameter;
  }
  if (cls == 5 /* FileStandardInformation */) {
    if (len < 24) return k_status_invalid_parameter;
    const long pos = ftell(os);
    fseek(os, 0, SEEK_END);
    const i64 sz = (i64)ftell(os);
    fseek(os, pos, SEEK_SET);
    struct file_std_info {
      i64 alloc_size;
      i64 eof;
      u32 links;
      u8  delete_pending;
      u8  directory;
      u16 pad;
    };
    file_std_info* fsi = (file_std_info*)info;
    memset(fsi, 0, sizeof(*fsi));
    fsi->alloc_size = sz;
    fsi->eof = sz;
    fsi->links = 1;
    if (iosb) { iosb->status = k_status_ok; iosb->info = sizeof(*fsi); }
    return k_status_ok;
  }
  if (cls == 14 /* FilePositionInformation */) {
    if (len < 8) return k_status_invalid_parameter;
    *(i64*)info = (i64)ftell(os);
    if (iosb) { iosb->status = k_status_ok; iosb->info = 8; }
    return k_status_ok;
  }
  if (cls == 9 /* FileNameInformation */) {
    if (len < 6) return k_status_invalid_parameter;
    const char* vp = trt_file_vpath(h);
    const char* rel = (vp && vp[0] && vp[1] == ':') ? vp + 2 : (vp ? vp : "\\");
    const u32 nchars = (u32)strlen(rel);
    const u32 bytes_needed = nchars * 2;
    *(u32*)info = bytes_needed;
    u16* dst = (u16*)((u8*)info + 4);
    const u32 max_chars = (len - 4) / 2;
    const u32 copy_chars = nchars < max_chars ? nchars : max_chars;
    for (u32 i = 0; i < copy_chars; ++i) dst[i] = (u16)(u8)rel[i];
    const u32 written = 4 + copy_chars * 2;
    const u32 st = (copy_chars < nchars) ? k_status_buffer_overflow : k_status_ok;
    if (iosb) { iosb->status = st; iosb->info = written; }
    return st;
  }
  char line[144];
  snprintf(line, sizeof(line),
           "ntdll: NtQueryInformationFile(cls=%u) -> STATUS_NOT_IMPLEMENTED",
           cls);
  trt_core_log(line);
  if (iosb) { iosb->status = k_status_not_implemented; iosb->info = 0; }
  return k_status_not_implemented;
}

u32 TRT_GUEST_ABI nt_NtWriteFile(void* h, void* ev, void* apc, void* apc_ctx,
                                 void* iosb_ptr, const void* buf, u32 len,
                                 const i64* byte_offset, void* key) {
  (void)ev; (void)apc; (void)apc_ctx; (void)key;
  io_status_block* iosb = (io_status_block*)iosb_ptr;
  FILE* os = trt_file_os(h);
  if (!os) {
    if (iosb) { iosb->status = k_status_invalid_handle; iosb->info = 0; }
    return k_status_invalid_handle;
  }
  if (byte_offset && *byte_offset >= 0) {
    fseek(os, (long)*byte_offset, SEEK_SET);
  }
  const size_t wrote = (buf && len) ? fwrite(buf, 1, len, os) : 0;
  if (iosb) { iosb->status = k_status_ok; iosb->info = (u64)wrote; }
  return k_status_ok;
}

// ETW: console apps have no kernel ETW trace session; register returns a valid
// nonzero token and Write verbs succeed as no-ops; EventEnabled -> 0.
u32 TRT_GUEST_ABI nt_EtwEventRegister(const void* provider_id, void* callback,
                                      void* callback_ctx, u64* reg_handle) {
  (void)provider_id; (void)callback; (void)callback_ctx;
  if (reg_handle) *reg_handle = 0x45545701ull;
  return 0;
}
u32 TRT_GUEST_ABI nt_EtwEventUnregister(u64 reg_handle) {
  (void)reg_handle;
  return 0;
}
u32 TRT_GUEST_ABI nt_EtwEventWrite(u64 reg, const void* desc, u32 count,
                                   void* data) {
  (void)reg; (void)desc; (void)count; (void)data;
  return 0;
}
u32 TRT_GUEST_ABI nt_EtwEventWriteEx(u64 reg, const void* desc, u64 filter,
                                     u32 flags, const void* act,
                                     const void* rel_act, u32 count, void* data) {
  (void)reg; (void)desc; (void)filter; (void)flags;
  (void)act; (void)rel_act; (void)count; (void)data;
  return 0;
}
u32 TRT_GUEST_ABI nt_EtwEventWriteTransfer(u64 reg, const void* desc,
                                           const void* act, const void* rel,
                                           u32 count, void* data) {
  (void)reg; (void)desc; (void)act; (void)rel; (void)count; (void)data;
  return 0;
}
u32 TRT_GUEST_ABI nt_EtwEventWriteString(u64 reg, u8 level, u64 kw,
                                         const u16* str) {
  (void)reg; (void)level; (void)kw; (void)str;
  return 0;
}
u32 TRT_GUEST_ABI nt_EtwEventSetInformation(u64 reg, u32 cls, const void* info,
                                            u32 len) {
  (void)reg; (void)cls; (void)info; (void)len;
  return 0;
}
u8 TRT_GUEST_ABI nt_EtwEventEnabled(u64 reg, const void* desc) {
  (void)reg; (void)desc;
  return 0;  // tracing disabled
}
u32 TRT_GUEST_ABI nt_EtwEventActivityIdControl(u32 ctrl, void* guid) {
  if (!guid) return 87;  // ERROR_INVALID_PARAMETER
  if (ctrl == 1 /* GET */ || ctrl == 3 /* CREATE */ || ctrl == 4 /* GET_SET */) {
    memset(guid, 0, 16);
    ((u8*)guid)[0] = 0x54;  // 'T'
  }
  return 0;
}

u32 dos_to_ntstatus(u32 e) {
  switch (e) {
    case 0: return 0x00000000u;
    case 8: return 0xC0000017u;    // STATUS_NO_MEMORY
    case 87: return 0xC000000Du;   // STATUS_INVALID_PARAMETER
    case 487: return 0xC0000018u;  // STATUS_CONFLICTING_ADDRESSES
    case 5: return 0xC0000022u;    // STATUS_ACCESS_DENIED
    default: return 0xC0000001u;   // STATUS_UNSUCCESSFUL
  }
}

u32 TRT_GUEST_ABI nt_NtAllocateVirtualMemoryEx(void* proc, void** base_ptr,
                                              u64* region_size, u32 alloc_type,
                                              u32 protect, void* ext_params,
                                              u32 param_count) {
  if (!base_ptr || !region_size || *region_size == 0) return 0xC000000Du;
  void* p = trt_VirtualAlloc2(proc, *base_ptr, *region_size, alloc_type,
                              protect, ext_params, param_count);
  if (!p) return dos_to_ntstatus(trt_GetLastError());
  *base_ptr = p;
  *region_size = (*region_size + 4095ULL) & ~4095ULL;
  return 0;
}

u32 TRT_GUEST_ABI nt_NtAllocateVirtualMemory(void* proc, void** base_ptr,
                                            u64 zero_bits, u64* region_size,
                                            u32 alloc_type, u32 protect) {
  (void)zero_bits;
  return nt_NtAllocateVirtualMemoryEx(proc, base_ptr, region_size, alloc_type,
                                      protect, nullptr, 0);
}

u32 TRT_GUEST_ABI nt_NtFreeVirtualMemory(void* proc, void** base_ptr,
                                        u64* region_size, u32 free_type) {
  (void)proc;
  if (!base_ptr || !region_size) return 0xC000000Du;
  if (!trt_VirtualFree(*base_ptr, *region_size, free_type)) {
    return dos_to_ntstatus(trt_GetLastError());
  }
  return 0;
}

u32 TRT_GUEST_ABI nt_NtProtectVirtualMemory(void* proc, void** base_ptr,
                                           u64* region_size, u32 new_prot,
                                           u32* old_prot) {
  (void)proc;
  if (!base_ptr || !region_size) return 0xC000000Du;
  u32 tmp_old = 0;
  if (!trt_VirtualProtect(*base_ptr, *region_size, new_prot,
                          old_prot ? old_prot : &tmp_old)) {
    return dos_to_ntstatus(trt_GetLastError());
  }
  return 0;
}

void TRT_GUEST_ABI nt_RtlGetDeviceFamilyInfoEnum(u64* uap_info,
                                                u32* dev_family,
                                                u32* dev_form) {
  if (uap_info) *uap_info = 0x000A00004A650000ULL;  // 10.0.19045.0
  if (dev_family) *dev_family = 3u;                 // DEVICEFAMILYINFOENUM_DESKTOP
  if (dev_form) *dev_form = 0u;                     // DEVICEFAMILYDEVICEFORM_UNKNOWN
}

struct nt_ansi_str {
  u16 length;
  u16 max_length;
  u32 pad;
  char* buffer;
};

struct nt_unicode_str {
  u16 length;
  u16 max_length;
  u32 pad;
  u16* buffer;
};

std::mutex g_nt_str_mu;
std::unordered_set<void*> g_nt_str_allocs;

void nt_str_track(void* p) {
  if (!p) return;
  std::lock_guard<std::mutex> lk(g_nt_str_mu);
  g_nt_str_allocs.insert(p);
}

void nt_str_free_tracked(void* p) {
  if (!p) return;
  bool owned = false;
  {
    std::lock_guard<std::mutex> lk(g_nt_str_mu);
    auto it = g_nt_str_allocs.find(p);
    if (it != g_nt_str_allocs.end()) {
      g_nt_str_allocs.erase(it);
      owned = true;
    }
  }
  if (owned) free(p);
}

void TRT_GUEST_ABI nt_RtlInitAnsiString(nt_ansi_str* dst, const char* src) {
  if (!dst) return;
  if (!src) {
    dst->length = 0;
    dst->max_length = 0;
    dst->buffer = nullptr;
    return;
  }
  size_t len = strlen(src);
  if (len > 0xFFFEu) len = 0xFFFEu;
  dst->length = (u16)len;
  dst->max_length = (u16)(len + 1u);
  dst->buffer = (char*)src;
}

void TRT_GUEST_ABI nt_RtlInitUnicodeString(nt_unicode_str* dst, const u16* src) {
  if (!dst) return;
  if (!src) {
    dst->length = 0;
    dst->max_length = 0;
    dst->buffer = nullptr;
    return;
  }
  size_t n = 0;
  while (src[n] && n < 0x7FFEu) ++n;
  dst->length = (u16)(n * 2u);
  dst->max_length = (u16)((n + 1u) * 2u);
  dst->buffer = (u16*)src;
}

u32 TRT_GUEST_ABI nt_RtlAnsiStringToUnicodeString(nt_unicode_str* dst,
                                                 const nt_ansi_str* src,
                                                 u8 allocate) {
  if (!dst || !src) return 0xC000000Du;
  if (src->length == 0 || !src->buffer) {
    if (allocate) {
      u16* buf = (u16*)calloc(1, sizeof(u16));
      if (!buf) return 0xC0000017u;
      nt_str_track(buf);
      dst->buffer = buf;
      dst->length = 0;
      dst->max_length = 2;
    } else {
      dst->length = 0;
      if (dst->buffer && dst->max_length >= 2) dst->buffer[0] = 0;
    }
    return 0;
  }
  int wlen = trt_MultiByteToWideChar(0, 0, src->buffer, (i32)src->length, nullptr, 0);
  if (wlen < 0) wlen = 0;
  const u32 cap_bytes = (u32)(wlen * 2);
  if (cap_bytes > 0xFFFEu) return 0x80000005u;
  if (allocate) {
    u16* buf = (u16*)malloc(cap_bytes + 2u);
    if (!buf) return 0xC0000017u;
    int actual = wlen > 0 ? trt_MultiByteToWideChar(0, 0, src->buffer, (i32)src->length, buf, wlen) : 0;
    if (actual < 0) actual = 0;
    buf[actual] = 0;
    nt_str_track(buf);
    dst->buffer = buf;
    dst->length = (u16)(actual * 2);
    dst->max_length = (u16)(cap_bytes + 2u);
    return 0;
  }
  if (!dst->buffer || dst->max_length < cap_bytes + 2u) return 0x80000005u;
  int actual = wlen > 0 ? trt_MultiByteToWideChar(0, 0, src->buffer, (i32)src->length, dst->buffer, wlen) : 0;
  if (actual < 0) actual = 0;
  dst->buffer[actual] = 0;
  dst->length = (u16)(actual * 2);
  return 0;
}

u32 TRT_GUEST_ABI nt_RtlUnicodeStringToAnsiString(nt_ansi_str* dst,
                                                 const nt_unicode_str* src,
                                                 u8 allocate) {
  if (!dst || !src) return 0xC000000Du;
  const i32 wchars = (i32)(src->length / 2u);
  if (wchars == 0 || !src->buffer) {
    if (allocate) {
      char* buf = (char*)calloc(1, 1);
      if (!buf) return 0xC0000017u;
      nt_str_track(buf);
      dst->buffer = buf;
      dst->length = 0;
      dst->max_length = 1;
    } else {
      dst->length = 0;
      if (dst->buffer && dst->max_length >= 1) dst->buffer[0] = 0;
    }
    return 0;
  }
  int alen = trt_WideCharToMultiByte(0, 0, src->buffer, wchars, nullptr, 0, nullptr, nullptr);
  if (alen < 0) alen = 0;
  const u32 bytes = (u32)alen;
  if (bytes > 0xFFFEu) return 0x80000005u;
  if (allocate) {
    char* buf = (char*)malloc(bytes + 1u);
    if (!buf) return 0xC0000017u;
    if (alen > 0) {
      trt_WideCharToMultiByte(0, 0, src->buffer, wchars, buf, alen, nullptr, nullptr);
    }
    buf[alen] = 0;
    nt_str_track(buf);
    dst->buffer = buf;
    dst->length = (u16)bytes;
    dst->max_length = (u16)(bytes + 1u);
    return 0;
  }
  if (!dst->buffer || dst->max_length < bytes + 1u) return 0x80000005u;
  if (alen > 0) {
    trt_WideCharToMultiByte(0, 0, src->buffer, wchars, dst->buffer, alen, nullptr, nullptr);
  }
  dst->buffer[alen] = 0;
  dst->length = (u16)bytes;
  return 0;
}

void TRT_GUEST_ABI nt_RtlFreeUnicodeString(nt_unicode_str* s) {
  if (!s) return;
  nt_str_free_tracked(s->buffer);
  s->buffer = nullptr;
  s->length = 0;
  s->max_length = 0;
}

void TRT_GUEST_ABI nt_RtlFreeAnsiString(nt_ansi_str* s) {
  if (!s) return;
  nt_str_free_tracked(s->buffer);
  s->buffer = nullptr;
  s->length = 0;
  s->max_length = 0;
}

i32 TRT_GUEST_ABI nt_RtlCompareUnicodeString(const nt_unicode_str* s1,
                                            const nt_unicode_str* s2,
                                            u8 case_insensitive) {
  if (!s1 && !s2) return 0;
  if (!s1) return -1;
  if (!s2) return 1;
  const u32 n1 = s1->length / 2u;
  const u32 n2 = s2->length / 2u;
  const u32 n = n1 < n2 ? n1 : n2;
  for (u32 i = 0; i < n; ++i) {
    u16 c1 = s1->buffer ? s1->buffer[i] : 0;
    u16 c2 = s2->buffer ? s2->buffer[i] : 0;
    if (case_insensitive) {
      if (c1 >= 'a' && c1 <= 'z') c1 -= 32;
      if (c2 >= 'a' && c2 <= 'z') c2 -= 32;
    }
    if (c1 != c2) return (c1 < c2) ? -1 : 1;
  }
  if (n1 == n2) return 0;
  return (n1 < n2) ? -1 : 1;
}

u8 TRT_GUEST_ABI nt_RtlEqualUnicodeString(const nt_unicode_str* s1,
                                         const nt_unicode_str* s2,
                                         u8 case_insensitive) {
  if (!s1 || !s2) return (s1 == s2) ? 1 : 0;
  if (s1->length != s2->length) return 0;
  return nt_RtlCompareUnicodeString(s1, s2, case_insensitive) == 0 ? 1 : 0;
}

u32 TRT_GUEST_ABI nt_RtlGetLastWin32Error(void) { return trt_GetLastError(); }
void TRT_GUEST_ABI nt_RtlSetLastWin32Error(u32 e) { trt_SetLastError(e); }

} // namespace

int trt_bridge_register_ntdll(void) {
  static const trt_named_fn k_fns[] = {
      {"RtlCaptureContext", (void*)&trt_RtlCaptureContext},
      {"RtlLookupFunctionEntry", (void*)&trt_RtlLookupFunctionEntry},
      {"RtlVirtualUnwind", (void*)&trt_RtlVirtualUnwind},
      {"RtlPcToFileHeader", (void*)&trt_RtlPcToFileHeader},
      {"RtlAddFunctionTable", (void*)&trt_RtlAddFunctionTable},
      {"RtlDeleteFunctionTable", (void*)&trt_RtlDeleteFunctionTable},
      {"RtlUnwind", (void*)&trt_RtlUnwind},
      {"RtlUnwindEx", (void*)&trt_RtlUnwindEx},
      {"RtlNtStatusToDosError", (void*)&nt_RtlNtStatusToDosError},
      {"RtlGetVersion", (void*)&nt_RtlGetVersion},
      {"RtlVerifyVersionInfo", (void*)&nt_RtlVerifyVersionInfo},
      {"RtlGetDeviceFamilyInfoEnum", (void*)&nt_RtlGetDeviceFamilyInfoEnum},
      {"RtlInitAnsiString", (void*)&nt_RtlInitAnsiString},
      {"RtlInitUnicodeString", (void*)&nt_RtlInitUnicodeString},
      {"RtlAnsiStringToUnicodeString", (void*)&nt_RtlAnsiStringToUnicodeString},
      {"RtlUnicodeStringToAnsiString", (void*)&nt_RtlUnicodeStringToAnsiString},
      {"RtlFreeUnicodeString", (void*)&nt_RtlFreeUnicodeString},
      {"RtlFreeAnsiString", (void*)&nt_RtlFreeAnsiString},
      {"RtlFreeOemString", (void*)&nt_RtlFreeAnsiString},
      {"RtlCompareUnicodeString", (void*)&nt_RtlCompareUnicodeString},
      {"RtlEqualUnicodeString", (void*)&nt_RtlEqualUnicodeString},
      {"RtlGetLastWin32Error", (void*)&nt_RtlGetLastWin32Error},
      {"RtlSetLastWin32Error", (void*)&nt_RtlSetLastWin32Error},
      {"NtAllocateVirtualMemory", (void*)&nt_NtAllocateVirtualMemory},
      {"NtAllocateVirtualMemoryEx", (void*)&nt_NtAllocateVirtualMemoryEx},
      {"NtFreeVirtualMemory", (void*)&nt_NtFreeVirtualMemory},
      {"NtProtectVirtualMemory", (void*)&nt_NtProtectVirtualMemory},
      {"NtClose", (void*)&nt_NtClose},
      {"NtQueryInformationFile", (void*)&nt_NtQueryInformationFile},
      {"NtWriteFile", (void*)&nt_NtWriteFile},
      {"EtwEventRegister", (void*)&nt_EtwEventRegister},
      {"EtwEventUnregister", (void*)&nt_EtwEventUnregister},
      {"EtwEventWrite", (void*)&nt_EtwEventWrite},
      {"EtwEventWriteEx", (void*)&nt_EtwEventWriteEx},
      {"EtwEventWriteTransfer", (void*)&nt_EtwEventWriteTransfer},
      {"EtwEventWriteString", (void*)&nt_EtwEventWriteString},
      {"EtwEventSetInformation", (void*)&nt_EtwEventSetInformation},
      {"EtwEventEnabled", (void*)&nt_EtwEventEnabled},
      {"EtwEventActivityIdControl", (void*)&nt_EtwEventActivityIdControl},
      // Aliases for advapi32 Event* via api-ms-win-eventing-provider
      {"EventRegister", (void*)&nt_EtwEventRegister},
      {"EventUnregister", (void*)&nt_EtwEventUnregister},
      {"EventWrite", (void*)&nt_EtwEventWrite},
      {"EventWriteEx", (void*)&nt_EtwEventWriteEx},
      {"EventWriteTransfer", (void*)&nt_EtwEventWriteTransfer},
      {"EventWriteString", (void*)&nt_EtwEventWriteString},
      {"EventSetInformation", (void*)&nt_EtwEventSetInformation},
      {"EventEnabled", (void*)&nt_EtwEventEnabled},
      {"EventActivityIdControl", (void*)&nt_EtwEventActivityIdControl},
  };
  trt_module_register("ntdll.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

} // extern "C"
