// tiramisu/src/trt/bridge_reg.cpp
// A virtual registry for the guest (see bridge_reg.h).
//
// Shape:
//   HKEY roots        HKCR / HKCU / HKLM / HKU / HKCC (Windows' fixed values)
//   key nodes         name + parent + child list + value list
//   value nodes       name + type + bytes
//
// Persistence: a flat text hive on the guest's C: drive, rewritten after
// every write (the hive is small; games write a handful of keys).
//
// Handle model: the root keys use Windows' real handle values (0x80000000
// + index), opened keys get allocated handles from the same table, so the
// guest can pass them back like real HKEYs.
#include "bridge_reg.h"
#include "thunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

// ---- registry error codes the guest checks ----
constexpr int REG_SUCCESS = 0;
constexpr int REG_ERROR_FILE_NOT_FOUND = 2;
constexpr int REG_ERROR_ACCESS_DENIED = 5;
constexpr int REG_ERROR_INVALID_HANDLE = 6;
constexpr int REG_ERROR_NO_MORE_ITEMS = 259;
constexpr int REG_ERROR_MORE_DATA = 234;

// ---- value types ----
constexpr u32 TRT_REG_SZ = 1;
constexpr u32 TRT_REG_EXPAND_SZ = 2;
constexpr u32 TRT_REG_BINARY = 3;
constexpr u32 TRT_REG_DWORD = 4;
constexpr u32 TRT_REG_QWORD = 11;

struct reg_value {
  char name[128];
  u32 type;
  u32 size;
  u8* data;
  reg_value* next;
};

struct reg_key {
  char name[192];
  struct reg_key* parent;
  reg_key* child;
  reg_key* next_sibling;
  reg_value* values;
  reg_value* last_value;
  u32 child_count;
  u32 value_count;
  bool is_root;
};

#define REG_MAX_KEYS 256
#define REG_MAX_VALUES 512
#define REG_MAX_HANDLES 128

reg_key g_roots[5];
const char* g_root_names[5] = {"HKEY_CLASSES_ROOT", "HKEY_CURRENT_USER",
                                "HKEY_LOCAL_MACHINE", "HKEY_USERS",
                                "HKEY_CURRENT_CONFIG"};
// Windows' root handle values
const u32 g_root_handles[5] = {0x80000000u, 0x80000001u, 0x80000002u,
                               0x80000003u, 0x80000004u};

reg_key* g_open_keys[REG_MAX_HANDLES];
u32 g_handle_count = 0;
// the root handles are 0x80000000..0x80000004 (Windows' fixed values), so
// opened keys must start above them or a handle would mean two things
constexpr u32 REG_FIRST_HANDLE = 0x80000100u;

u16 g_hive_path[260] = {0};
bool g_hive_loaded = false;
bool g_hive_dirty = false;

// ------------------------------------------------------------------ helpers

reg_key* find_child(reg_key* parent, const char* name) {
  for (reg_key* k = parent->child; k; k = k->next_sibling)
    if (strcmp(k->name, name) == 0) return k;
  return nullptr;
}

reg_key* create_child(reg_key* parent, const char* name) {
  reg_key* k = (reg_key*)calloc(1, sizeof(reg_key));
  if (!k) return nullptr;
  snprintf(k->name, sizeof(k->name), "%s", name);
  k->parent = parent;
  k->next_sibling = parent->child;
  parent->child = k;
  ++parent->child_count;
  return k;
}

reg_value* find_value(reg_key* key, const char* name) {
  for (reg_value* v = key->values; v; v = v->next)
    if (strcmp(v->name, name) == 0) return v;
  return nullptr;
}

// split a buffer on a separator into fields (portable: no strtok_r/strtok_s)
int split_fields(char* line, char** fields, int max_fields, char sep) {
  int n = 0;
  char* p = line;
  while (n < max_fields) {
    fields[n++] = p;
    char* s = strchr(p, sep);
    if (!s) break;
    *s = 0;
    p = s + 1;
  }
  for (int i = 0; i < n; ++i) {
    size_t l = strlen(fields[i]);
    while (l && (fields[i][l - 1] == '\n' || fields[i][l - 1] == '\r'))
      fields[i][--l] = 0;
  }
  return n;
}

// path lookup: "Software\Foo\Bar" under a root
reg_key* open_path(reg_key* root, const char* path, bool create) {
  reg_key* cur = root;
  char buf[256];
  snprintf(buf, sizeof(buf), "%s", path);
  char* fields[32];
  const int n = split_fields(buf, fields, 32, '\\');
  for (int fi = 0; fi < n; ++fi) {
    char* tok = fields[fi];
    if (!*tok) continue;
    reg_key* next = find_child(cur, tok);
    if (!next) {
      if (!create) return nullptr;
      next = create_child(cur, tok);
      if (!next) return nullptr;
    }
    cur = next;
  }
  return cur;
}

reg_key* root_for_handle(u64 h) {
  for (int i = 0; i < 5; ++i)
    if (h == g_root_handles[i]) return &g_roots[i];
  return nullptr;
}

reg_key* key_for_handle(u64 h) {
  reg_key* root = root_for_handle(h);
  if (root) return root;
  if (h >= REG_FIRST_HANDLE && h < REG_FIRST_HANDLE + REG_MAX_HANDLES)
    return g_open_keys[h - REG_FIRST_HANDLE];
  return nullptr;
}

u64 handle_for_key(reg_key* key) {
  if (!key) return 0;
  for (int i = 0; i < 5; ++i)
    if (key == &g_roots[i]) return g_root_handles[i];
  for (u32 i = 0; i < g_handle_count; ++i)
    if (g_open_keys[i] == key) return REG_FIRST_HANDLE + i;
  if (g_handle_count >= REG_MAX_HANDLES) return 0;
  g_open_keys[g_handle_count] = key;
  return REG_FIRST_HANDLE + g_handle_count++;
}

u64 allocate_handle(reg_key* key) {
  if (g_handle_count >= REG_MAX_HANDLES) return 0;
  g_open_keys[g_handle_count] = key;
  return REG_FIRST_HANDLE + g_handle_count++;
}

// ------------------------------------------------------------------ storage

void set_value(reg_key* key, const char* name, u32 type, const void* data,
               u32 size) {
  reg_value* v = find_value(key, name);
  if (!v) {
    v = (reg_value*)calloc(1, sizeof(reg_value));
    if (!v) return;
    snprintf(v->name, sizeof(v->name), "%s", name);
    v->next = nullptr;
    if (key->last_value) key->last_value->next = v;
    else key->values = v;
    key->last_value = v;
    ++key->value_count;
  }
  u8* copy = (u8*)malloc(size ? size : 1);
  if (!copy) return;
  if (data && size) memcpy(copy, data, size);
  free(v->data);
  v->data = copy;
  v->type = type;
  v->size = size;
  g_hive_dirty = true;
}

void delete_value(reg_key* key, const char* name) {
  reg_value* prev = nullptr;
  for (reg_value* v = key->values; v; prev = v, v = v->next) {
    if (strcmp(v->name, name) != 0) continue;
    if (prev) prev->next = v->next;
    else key->values = v->next;
    if (key->last_value == v) key->last_value = prev;
    free(v->data);
    free(v);
    --key->value_count;
    g_hive_dirty = true;
    return;
  }
}

// ------------------------------------------------------------- persistence

void host_path_of_hive(char* out, size_t cap) {
  out[0] = 0;
  if (!g_hive_path[0]) return;
  if (trt_vfs_resolve_utf16(g_hive_path, out, cap) != 0) return;
}

// full path of a key under its root ("Software\\Tiramisu", "" for a root)
bool key_path(reg_key* k, char* out, size_t cap) {
  const char* parts[16];
  int n = 0;
  for (reg_key* p = k; p && !p->is_root && n < 16; p = p->parent)
    parts[n++] = p->name;
  out[0] = 0;
  for (int j = n - 1; j >= 0; --j) {
    if (out[0]) strncat(out, "\\", cap - strlen(out) - 1);
    strncat(out, parts[j], cap - strlen(out) - 1);
  }
  return true;
}

// hex-encode everything that is not a plain number, so UTF-16 strings and
// binary blobs survive the round trip
void hex_encode(const u8* data, u32 size, char* out, size_t cap) {
  size_t o = 0;
  for (u32 i = 0; i < size && o + 2 < cap; ++i) {
    snprintf(out + o, cap - o, "%02x", data[i]);
    o += 2;
  }
  out[o] = 0;
}

bool hex_decode(const char* hex, u8* out, size_t cap, u32* size) {
  const size_t len = strlen(hex);
  if (len % 2) return false;
  u32 n = 0;
  for (size_t i = 0; i + 1 < len; i += 2) {
    if (n + 1 > cap) return false;
    char pair[3] = {hex[i], hex[i + 1], 0};
    out[n++] = (u8)strtoul(pair, nullptr, 16);
  }
  *size = n;
  return true;
}

void write_hive() {
  char host[512];
  host_path_of_hive(host, sizeof(host));
  if (!host[0]) return;
  FILE* f = fopen(host, "wb");
  if (!f) return;
  fprintf(f, "# tiramisu registry hive v1\n");
  for (int i = 0; i < 5; ++i) {
    reg_key* stack[64];
    int sp = 0;
    stack[sp++] = &g_roots[i];
    while (sp > 0) {
      reg_key* k = stack[--sp];
      for (reg_key* c = k->child; c; c = c->next_sibling)
        if (sp < 64) stack[sp++] = c;
      if (!k->values) continue;
      char path[512];
      key_path(k, path, sizeof(path));
      for (reg_value* v = k->values; v; v = v->next) {
        char name[256], data[2048];
        hex_encode(v->data, v->size, data, sizeof(data));
        snprintf(name, sizeof(name), "%s", v->name);
        fprintf(f, "k\t%s\t%s\t%s\t%u\t%s\n", g_root_names[i], path, name,
                v->type, data);
      }
    }
  }
  fclose(f);
  g_hive_dirty = false;
}

void load_hive() {
  if (g_hive_loaded) return;
  g_hive_loaded = true;
  for (int i = 0; i < 5; ++i) {
    memset(&g_roots[i], 0, sizeof(g_roots[i]));
    snprintf(g_roots[i].name, sizeof(g_roots[i].name), "%s", g_root_names[i]);
    g_roots[i].is_root = true;
  }
  char host[512];
  host_path_of_hive(host, sizeof(host));
  if (!host[0]) return;
  FILE* f = fopen(host, "rb");
  if (!f) return;
  char line[4096];
  while (fgets(line, sizeof(line), f)) {
    if (line[0] == '#' || line[0] == '\n') continue;
    char* f[8];
    const int nf = split_fields(line, f, 8, '\t');
    if (nf < 6) continue;
    if (strcmp(f[0], "k") != 0) continue;
    const char* root_name = f[1];
    const char* path = f[2];
    const char* vname = f[3];
    const char* type_s = f[4];
    const char* data_s = f[5];
    if (!root_name || !path || !vname || !type_s || !data_s) continue;
    int ri = -1;
    for (int i = 0; i < 5; ++i)
      if (strcmp(g_root_names[i], root_name) == 0) ri = i;
    if (ri < 0) continue;
    reg_key* key = open_path(&g_roots[ri], path, true);
    if (!key) continue;
    const u32 type = (u32)strtoul(type_s, nullptr, 10);
    u8 buf[2048];
    u32 size = 0;
    if (type == TRT_REG_DWORD) {
      const u32 n32 = (u32)strtoul(data_s, nullptr, 10);
      memcpy(buf, &n32, 4);
      size = 4;
    } else if (type == TRT_REG_QWORD) {
      const u64 n64 = strtoull(data_s, nullptr, 10);
      memcpy(buf, &n64, 8);
      size = 8;
    } else if (!hex_decode(data_s, buf, sizeof(buf), &size)) {
      continue;
    }
    set_value(key, vname, type, buf, size);
  }
  fclose(f);
  g_hive_dirty = false;
}

// ------------------------------------------------------------- the functions

u32 reg_strlen(const u16* s) {
  u32 n = 0;
  while (s && s[n]) ++n;
  return n;
}

void reg_to_utf8(const u16* in, char* out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; in && in[i] && o + 1 < cap; ++i) {
    if (in[i] < 0x80) out[o++] = (char)in[i];
    else if (in[i] < 0x800) {
      if (o + 2 >= cap) break;
      out[o++] = (char)(0xC0 | (in[i] >> 6));
      out[o++] = (char)(0x80 | (in[i] & 0x3F));
    } else {
      if (o + 3 >= cap) break;
      out[o++] = (char)(0xE0 | (in[i] >> 12));
      out[o++] = (char)(0x80 | ((in[i] >> 6) & 0x3F));
      out[o++] = (char)(0x80 | (in[i] & 0x3F));
    }
  }
  out[o] = 0;
}

} // namespace

extern "C" {

void trt_registry_set_hive_path(const u16* guest_path) {
  for (int i = 0; i < 260 && guest_path && guest_path[i]; ++i)
    g_hive_path[i] = guest_path[i];
  g_hive_loaded = false;
}

int TRT_GUEST_ABI trt_RegOpenKeyExW(u64 hkey, const u16* subkey, u32 options,
                                    u32 sam, void** out) {
  (void)options; (void)sam;
  load_hive();
  reg_key* root = key_for_handle(hkey);
  if (!root || !out) return REG_ERROR_INVALID_HANDLE;
  char path[256];
  reg_to_utf8(subkey, path, sizeof(path));
  reg_key* key = path[0] ? open_path(root, path, false) : root;
  if (!key) {
    // Wine/Windows: opening a missing key with KEY_WOW64_* etc. still fails
    return REG_ERROR_FILE_NOT_FOUND;
  }
  *out = (void*)handle_for_key(key);
  return REG_SUCCESS;
}

int TRT_GUEST_ABI trt_RegCreateKeyExW(u64 hkey, const u16* subkey, u32 reserved,
                                       void* cls, u32 options, u32 sam,
                                       void** out, u32* disposition) {
  (void)reserved; (void)cls; (void)options; (void)sam;
  load_hive();
  reg_key* root = key_for_handle(hkey);
  if (!root || !out) return REG_ERROR_INVALID_HANDLE;
  char path[256];
  reg_to_utf8(subkey, path, sizeof(path));
  reg_key* existing = path[0] ? open_path(root, path, false) : nullptr;
  reg_key* key = existing;
  if (!key) {
    key = open_path(root, path, true);
    if (!key) return REG_ERROR_ACCESS_DENIED;
    if (disposition) *disposition = 1;  // REG_CREATED_NEW_KEY
    g_hive_dirty = true;
  } else if (disposition) {
    *disposition = 2;  // REG_OPENED_EXISTING_KEY
  }
  *out = (void*)handle_for_key(key);
  write_hive();
  return REG_SUCCESS;
}

int TRT_GUEST_ABI trt_RegQueryValueExW(u64 hkey, const u16* name, u32* reserved,
                                       u32* type, u8* data, u32 cbData,
                                       u32* cbDataOut) {
  (void)reserved;
  load_hive();
  reg_key* key = key_for_handle(hkey);
  char vname[128];
  reg_to_utf8(name, vname, sizeof(vname));
  char line[256];
  snprintf(line, sizeof(line), "trt: RegQueryValueExW(hkey=0x%llx, '%s') key=%p",
           (unsigned long long)hkey, vname, (void*)key);
  trt_core_log(line);
  if (!key) return REG_ERROR_INVALID_HANDLE;
  reg_value* v = find_value(key, vname);
  if (!v) return REG_ERROR_FILE_NOT_FOUND;
  if (type) *type = v->type;
  if (cbDataOut) *cbDataOut = v->size;
  if (!data) return REG_SUCCESS;
  if (cbData < v->size) return REG_ERROR_MORE_DATA;
  memcpy(data, v->data, v->size);
  return REG_SUCCESS;
}

int TRT_GUEST_ABI trt_RegSetValueExW(u64 hkey, const u16* name, u32 reserved,
                                      u32 type, const u8* data, u32 cbData) {
  (void)reserved;
  load_hive();
  reg_key* key = key_for_handle(hkey);
  if (!key) return REG_ERROR_INVALID_HANDLE;
  char vname[128];
  reg_to_utf8(name, vname, sizeof(vname));
  set_value(key, vname, type, data, cbData);
  write_hive();
  return REG_SUCCESS;
}

int TRT_GUEST_ABI trt_RegDeleteValueW(u64 hkey, const u16* name) {
  load_hive();
  reg_key* key = key_for_handle(hkey);
  if (!key) return REG_ERROR_INVALID_HANDLE;
  char vname[128];
  reg_to_utf8(name, vname, sizeof(vname));
  if (!find_value(key, vname)) return REG_ERROR_FILE_NOT_FOUND;
  delete_value(key, vname);
  write_hive();
  return REG_SUCCESS;
}

int TRT_GUEST_ABI trt_RegCloseKey(u64 hkey) {
  if (hkey >= REG_FIRST_HANDLE && hkey < REG_FIRST_HANDLE + REG_MAX_HANDLES) {
    const u32 idx = (u32)(hkey - REG_FIRST_HANDLE);
    if (idx < g_handle_count) g_open_keys[idx] = nullptr;
    return REG_SUCCESS;
  }
  return REG_SUCCESS;
}

int TRT_GUEST_ABI trt_RegEnumKeyExW(u64 hkey, u32 index, u16* name,
                                     u32 cchMax, u32* reserved,
                                     u32* lpcchName, u32* last_write) {
  (void)reserved; (void)last_write;
  load_hive();
  reg_key* key = key_for_handle(hkey);
  if (!key) return REG_ERROR_INVALID_HANDLE;
  u32 n = 0;
  for (reg_key* c = key->child; c; c = c->next_sibling) {
    if (n == index) {
      const u32 len = (u32)strlen(c->name) + 1;
      if (lpcchName) *lpcchName = len;
      if (!name) return REG_SUCCESS;
      if (cchMax < len) return REG_ERROR_MORE_DATA;
      for (u32 i = 0; i < len; ++i) name[i] = (u16)(u8)c->name[i];
      return REG_SUCCESS;
    }
    ++n;
  }
  return REG_ERROR_NO_MORE_ITEMS;
}

int TRT_GUEST_ABI trt_RegEnumValueW(u64 hkey, u32 index, u16* name, u32 cchMax,
                                     u32* reserved, u32* lpcchName,
                                     u32* last_write, u32* type, u8* data,
                                     u32* cbData) {
  (void)reserved; (void)last_write;
  load_hive();
  reg_key* key = key_for_handle(hkey);
  if (!key) return REG_ERROR_INVALID_HANDLE;
  u32 n = 0;
  for (reg_value* v = key->values; v; v = v->next) {
    if (n == index) {
      const u32 len = (u32)strlen(v->name) + 1;
      if (lpcchName) *lpcchName = len;
      if (type) *type = v->type;
      if (cbData) *cbData = v->size;
      if (!name) return REG_SUCCESS;
      if (cchMax < len) return REG_ERROR_MORE_DATA;
      for (u32 i = 0; i < len; ++i) name[i] = (u16)(u8)v->name[i];
      if (data && cbData && *cbData >= v->size) memcpy(data, v->data, v->size);
      return REG_SUCCESS;
    }
    ++n;
  }
  return REG_ERROR_NO_MORE_ITEMS;
}

// modern API: RegGetValueW(HKEY, subkey, value, flags, type, data, cb, cbOut)
int TRT_GUEST_ABI trt_RegGetValueW(u64 hkey, const u16* subkey, const u16* value,
                                    u32 flags, u32* type, u8* data, u32 cbData,
                                    u32* cbDataOut) {
  (void)flags;
  load_hive();
  reg_key* root = key_for_handle(hkey);
  if (!root) return REG_ERROR_INVALID_HANDLE;
  char path[256];
  reg_to_utf8(subkey, path, sizeof(path));
  reg_key* key = path[0] ? open_path(root, path, false) : root;
  if (!key) return REG_ERROR_FILE_NOT_FOUND;
  char vname[128];
  reg_to_utf8(value, vname, sizeof(vname));
  reg_value* v = find_value(key, vname);
  if (!v) return REG_ERROR_FILE_NOT_FOUND;
  if (type) *type = v->type;
  if (cbDataOut) *cbDataOut = v->size;
  if (data && cbData >= v->size) memcpy(data, v->data, v->size);
  return REG_SUCCESS;
}

// ============================ token / security ==============================
// OrTMM (and many PC games) import advapi32's token surface at boot to ask
// "who am I". The honest answers on the console:
//   OpenProcessToken: a REAL pseudo token handle (a record here — never a
//     fake NULL or a fake failure: UWP processes DO have a process token).
//   GetTokenInformation: TokenUser / TokenIntegrityLevel / TokenStatistics
//     get minimal real records; anything else fails ERROR_INVALID_PARAMETER
//     (named).
//   ConvertStringSecurityDescriptorToSecurityDescriptorW: the console has
//     no SDDL security: fail ERROR_CALL_NOT_IMPLEMENTED, named.
//   ConvertSidToStringSidW: convert a real SID record if the bytes parse;
//     fail ERROR_INVALID_PARAMETER otherwise.
namespace {

// token records: OpenProcessToken hands these out
constexpr u32 TOKEN_HANDLE_TAG = 0x544B0000u;  // 'TK'
bool g_token_used[8];

bool token_is_valid(u32 h) {
  if ((h & 0xFFFF0000u) != TOKEN_HANDLE_TAG) return false;
  const u32 idx = h & 0xFFFFu;
  return idx > 0 && idx <= 8 && g_token_used[idx - 1];
}

// token access rights
constexpr u32 TRT_TOKEN_QUERY_RIGHT = 0x0008;

// TOKEN_INFORMATION classes
constexpr u32 TRT_TOKENINFO_USER = 1;
constexpr u32 TRT_TOKENINFO_STATISTICS = 10;
constexpr u32 TOKEN_INTEGRITY_LEVEL = 25;
// TokenAppContainerSid (winnt.h): PyInstaller queries this to decide whether
// to add an AppContainer ACE to its temp-directory DACL. Honest answer: the
// console guest is NOT running in an AppContainer, so the record is an 8-byte
// TOKEN_APPCONTAINER_INFORMATION { PSID TokenAppContainer = NULL }.
constexpr u32 TOKEN_APPCONTAINER_SID = 31;

// the pseudo user SID: S-1-5-88-1-1001 (a domain-less local player).
// SID layout: Revision u8, SubAuthorityCount u8, IdentifierAuthority
// (6 bytes BE, 0x000000000005 = NT authority), SubAuthority[] u32 LE.
constexpr u8 k_sid_bytes[24] = {
    1,                 // revision
    3,                 // sub-authority count (88, 1, 1001)
    0, 0, 0, 0, 0, 5,  // ID authority: NT
    88, 0, 0, 0,       // 88
    1, 0, 0, 0,        // 1
    233, 3, 0, 0,      // 1001 (the RID)
};

// the integrity SID: S-1-16-8192 (medium integrity)
constexpr u8 k_il_sid[12] = {1, 1, 0, 0, 0, 0, 0, 16, 0, 32, 0, 0};

constexpr u32 k_token_user_size = 36;    // SID_AND_ATTRIBUTES(12) + SID(24)
constexpr u32 k_token_il_size = 40;      // SID_AND_ATTRIBUTES(12) + SID(12) + mask room
constexpr u32 k_token_stats_size = 72;   // LUIDs + type/level fields
constexpr u32 k_token_appcontainer_size = 8;  // { PSID } = NULL (no AppContainer)

// thread-local last error lives in bridge_kernel32.cpp; use the exported
// setter so guest GetLastError() sees our named failures
void token_set_last_error(u32 err);

} // namespace

extern "C" u32 TRT_STDCALL trt_SetLastError(u32 err);  // bridge_kernel32

namespace {
void token_set_last_error(u32 err) { trt_SetLastError(err); }
} // namespace

} // extern "C"  (registry half)

extern "C" {  // token/security half

// ============================ token / security ==============================

void* TRT_GUEST_ABI trt_OpenProcessToken(void* process, u32 desired,
                                          void* token_out) {
  (void)process;  // GetCurrentProcess pseudo handle is fine
  if (!token_out) {
    token_set_last_error(87);  // ERROR_INVALID_PARAMETER
    return 0;
  }
  if ((desired & TRT_TOKEN_QUERY_RIGHT) == 0) {
    // we only support QUERY; anything else is honest about it
    char line[160];
    snprintf(line, sizeof(line),
             "advapi32: OpenProcessToken desired=0x%x (no QUERY bit) -> "
             "ERROR_ACCESS_DENIED", desired);
    trt_core_log(line);
    token_set_last_error(5);  // ERROR_ACCESS_DENIED
    return 0;
  }
  int slot = -1;
  for (int i = 0; i < 8; ++i)
    if (!g_token_used[i]) { slot = i; break; }
  if (slot < 0) {
    token_set_last_error(5);
    return 0;
  }
  g_token_used[slot] = true;
  *(void**)token_out = (void*)(uintptr_t)(TOKEN_HANDLE_TAG | (u32)(slot + 1));
  char line[128];
  snprintf(line, sizeof(line),
           "advapi32: OpenProcessToken -> token record (pseudo handle)");
  trt_core_log(line);
  return (void*)(uintptr_t)1;  // TRUE
}

int TRT_GUEST_ABI trt_GetTokenInformation(void* token, u32 cls,
                                           void* info, u32 len,
                                           u32* len_out) {
  if (!token_is_valid((u32)(uintptr_t)token)) {
    char line[160];
    snprintf(line, sizeof(line),
             "advapi32: GetTokenInformation bad token handle 0x%x -> "
             "ERROR_INVALID_HANDLE", (u32)(uintptr_t)token);
    trt_core_log(line);
    token_set_last_error(6);  // ERROR_INVALID_HANDLE
    return 0;
  }
  // the classes games and tooling actually ask, with minimal REAL records
  u32 fill = 0;
  switch (cls) {
    case TRT_TOKENINFO_USER: fill = k_token_user_size; break;
    case TOKEN_INTEGRITY_LEVEL: fill = k_token_il_size; break;
    case TRT_TOKENINFO_STATISTICS: fill = k_token_stats_size; break;
    case TOKEN_APPCONTAINER_SID: fill = k_token_appcontainer_size; break;
    default: {
      char line[160];
      snprintf(line, sizeof(line),
               "advapi32: GetTokenInformation class=%u unsupported -> "
               "ERROR_INVALID_PARAMETER (named, fail-visible)", cls);
      trt_core_log(line);
      token_set_last_error(87);
      return 0;
    }
  }
  // size-query form: info == NULL with len == 0 is the canonical
  // "how big is this record?" probe (PyInstaller's bootloader does exactly
  // this before allocating). Answer with the size + ERROR_INSUFFICIENT_BUFFER,
  // the documented contract — NOT ERROR_INVALID_PARAMETER.
  if (!info) {
    if (len_out) *len_out = fill;
    token_set_last_error(122);  // ERROR_INSUFFICIENT_BUFFER
    return 0;
  }
  if (!len_out) {
    token_set_last_error(87);
    return 0;
  }
  if (len < fill) {
    char line[160];
    snprintf(line, sizeof(line),
             "advapi32: GetTokenInformation class=%u buffer=%u need=%u -> "
             "ERROR_INSUFFICIENT_BUFFER", cls, len, fill);
    trt_core_log(line);
    *len_out = fill;
    token_set_last_error(122);  // ERROR_INSUFFICIENT_BUFFER
    return 0;
  }
  *len_out = fill;
  u8* out = (u8*)info;
  memset(out, 0, fill);
  if (cls == TRT_TOKENINFO_USER) {
    // SID_AND_ATTRIBUTES { pSid, u32 attrs }: pSid points INTO the
    // caller's buffer (winnt semantics), the SID bytes follow the pointer.
    // The struct is { void*, u32 } with 4 bytes tail padding — never write
    // the u32 at +4, that would zero the pointer's upper half.
    u8* sid = out + 16;
    memcpy(sid, k_sid_bytes, sizeof(k_sid_bytes));
    *(void**)(out + 0) = sid;
    *(u32*)(out + 8) = 0;  // attributes
  } else if (cls == TOKEN_INTEGRITY_LEVEL) {
    u8* sid = out + 16;
    memcpy(sid, k_il_sid, sizeof(k_il_sid));
    *(void**)(out + 0) = sid;
    *(u32*)(out + 8) = 0;
    *(u32*)(out + 28) = 0;  // policy mask: none (AFTER the 12-byte SID)
  } else if (cls == TRT_TOKENINFO_STATISTICS) {  // minimal stats record
    *(u64*)(out + 0) = 0x2A;   // TokenId LUID (stable pseudo)
    *(u64*)(out + 8) = 0x2A;   // AuthenticationId
    *(u64*)(out + 16) = 0;     // ExpirationTime: never
    *(u32*)(out + 24) = 1;     // TokenType: TokenPrimary
    *(u32*)(out + 28) = 0;     // ImpersonationLevel: Anonymous
  } else if (cls == TOKEN_APPCONTAINER_SID) {
    // { PSID } = NULL: the console guest is not running in an AppContainer
    *(void**)(out + 0) = nullptr;
  }
  char line[128];
  snprintf(line, sizeof(line),
           "advapi32: GetTokenInformation class=%u -> %u bytes of record",
           cls, fill);
  trt_core_log(line);
  return 1;  // TRUE
}

// backend allocator singleton (images.cpp): the same pool HeapAlloc hands
// out of, so the guest's LocalFree on the SD releases it honestly.
const trt_backend* trt_runtime_backend(void);  // images.cpp

int TRT_GUEST_ABI trt_ConvertStringSecurityDescriptorToSecurityDescriptorW(
    const u16* sddl, u32 req, void* sd_out, void* len_out) {
  (void)req;  // SDDL_REVISION_1 is the only revision; accept it
  char utf8[128];
  if (sddl)
    trt_utf16_to_utf8(sddl, utf8, sizeof(utf8));
  else
    snprintf(utf8, sizeof(utf8), "(null)");

  // The console has no security model, but the guest may legitimately pass
  // the resulting SECURITY_DESCRIPTOR blob onward (PyInstaller hands it to
  // CreateDirectoryW and LocalFree). The honest-minimal surface is a REAL
  // self-relative SECURITY_DESCRIPTOR with a NULL DACL (everyone-full, the
  // on-disk truth of this runtime) so downstream consumers see well-formed
  // bytes, logged as such — never a claim that access is actually restricted.
  //
  // SDDL we support: "D:" followed by zero or more (A;;FA;;;SID) ACEs. We
  // count the ACEs only to log them; the DACL itself is NULL either way.
  unsigned n_ace = 0;
  for (const char* p = utf8; (p = strstr(p, "(A;")) != nullptr; p += 1) ++n_ace;

  // self-relative SD layout (winnt.h SECURITY_DESCRIPTOR_RELATIVE):
  //   u8  Revision; u8 SandboxInert-pad; u16 Control;
  //   u32 OwnerOffset; u32 GroupOffset; u32 SaclOffset; u32 DaclOffset;
  //   [Owner SID][Group SID]
  // SE_SELF_RELATIVE = 0x8000. Owner = the console pseudo user SID
  // (S-1-5-88-1-1001), Group = same SID (a valid, honest grouping).
  constexpr u32 k_se_self_relative = 0x8000;
  constexpr u32 k_hdr = 20;
  const u32 owner_off = k_hdr;
  const u32 group_off = owner_off + (u32)sizeof(k_sid_bytes);
  const u32 blob = group_off + (u32)sizeof(k_sid_bytes);
  u8* sd = trt_runtime_backend()
               ? (u8*)trt_runtime_backend()->alloc(blob, 0)
               : (u8*)malloc(blob);
  if (!sd) {
    token_set_last_error(8);  // ERROR_NOT_ENOUGH_MEMORY
    return 0;
  }
  memset(sd, 0, blob);
  sd[0] = 1;  // Revision
  *(u16*)(sd + 2) = (u16)k_se_self_relative;
  *(u32*)(sd + 4) = owner_off;
  *(u32*)(sd + 8) = group_off;
  *(u32*)(sd + 12) = 0;  // Sacl: none
  *(u32*)(sd + 16) = 0;  // Dacl: NULL DACL = allow all (the honest truth here)
  memcpy(sd + owner_off, k_sid_bytes, sizeof(k_sid_bytes));
  memcpy(sd + group_off, k_sid_bytes, sizeof(k_sid_bytes));
  if (sd_out) *(void**)sd_out = sd;
  if (len_out) *(u32*)len_out = blob;
  char line[288];
  snprintf(line, sizeof(line),
           "advapi32: ConvertStringSecurityDescriptorToSecurityDescriptorW"
           "('%s') -> self-relative SD %u bytes (NULL DACL = allow-all; "
           "no security model on console; %u ACE(s) noted, not enforced)",
           utf8, blob, n_ace);
  trt_core_log(line);
  return 1;  // TRUE
}

int TRT_GUEST_ABI trt_ConvertSidToStringSidW(const void* sid, u16** str_out) {
  if (!sid || !str_out) {
    token_set_last_error(87);
    return 0;
  }
  const u8* s = (const u8*)sid;
  if (s[0] != 1 /*revision*/ || s[1] == 0 || s[1] > 15) {
    char line[160];
    snprintf(line, sizeof(line),
             "advapi32: ConvertSidToStringSidW bad SID (rev=%u n=%u) -> "
             "ERROR_INVALID_PARAMETER", s[0], s[1]);
    trt_core_log(line);
    token_set_last_error(87);
    return 0;
  }
  // "S-1-<ia>-<sub>..." — authority is 6 bytes BE
  u64 ia = 0;
  for (int i = 0; i < 6; ++i) ia = (ia << 8) | s[2 + i];
  char utf8[160];
  int o = snprintf(utf8, sizeof(utf8), "S-1-%llu", (unsigned long long)ia);
  const u32 n = s[1];
  for (u32 i = 0; i < n && o < (int)sizeof(utf8) - 12; ++i) {
    const u32 sub = (u32)s[8 + i * 4] | ((u32)s[9 + i * 4] << 8) |
                    ((u32)s[10 + i * 4] << 16) | ((u32)s[11 + i * 4] << 24);
    o += snprintf(utf8 + o, sizeof(utf8) - o, "-%u", sub);
  }
  // hand the caller a UTF-16 copy in a static ring (game-pace: a handful)
  static u16 ring[4][160];
  static int ring_i = 0;
  u16* out = ring[ring_i++ % 4];
  for (int i = 0; utf8[i] && i < 159; ++i) out[i] = (u16)(u8)utf8[i];
  out[159] = 0;
  *str_out = out;
  char line[224];
  snprintf(line, sizeof(line),
           "advapi32: ConvertSidToStringSidW -> \"%s\"", utf8);
  trt_core_log(line);
  return 1;  // TRUE
}

// advapi32!SystemFunction036 (RtlGenRandom): the obfuscation runtime in
// corpus-local/gonext/dstoragecore.dll lazy-resolves this via
// LoadLibraryExW("advapi32") + GetProcAddress and aborts (int 0x29 ec=7) when
// either step fails. We have no security model and no entropy source, so this
// bridge fills the caller buffer with deterministic pseudo-random bytes and
// logs the call fail-visible. Never fake-secure: the log line states it.
static int trt_SystemFunction036(void* buf, unsigned long len) {
  unsigned char* p = (unsigned char*)buf;
  unsigned long state = 0x9e3779b97f4a7c15ull * (unsigned long)(len + 1);
  for (unsigned long i = 0; i < len; ++i) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    p[i] = (unsigned char)(state >> 24);
  }
  char line[128];
  snprintf(line, sizeof(line),
           "advapi32: SystemFunction036(buf=%p len=%lu) -> deterministic "
           "fill (NOT cryptographic entropy)",
           (void*)buf, len);
  trt_core_log(line);
  return 1;  // TRUE
}

// exported for the test harness (the dstoragecore IAT-gap bind)
void* trt_bridge_SystemFunction036(void* buf, unsigned long len) {
  return (void*)(uintptr_t)trt_SystemFunction036(buf, len);
}

int trt_bridge_register_registry(void) {
  static const trt_named_fn k_advapi32_fns[] = {
      {"RegOpenKeyExW", (void*)&trt_RegOpenKeyExW},
      {"RegCreateKeyExW", (void*)&trt_RegCreateKeyExW},
      {"RegQueryValueExW", (void*)&trt_RegQueryValueExW},
      {"RegSetValueExW", (void*)&trt_RegSetValueExW},
      {"RegDeleteValueW", (void*)&trt_RegDeleteValueW},
      {"RegCloseKey", (void*)&trt_RegCloseKey},
      {"RegEnumKeyExW", (void*)&trt_RegEnumKeyExW},
      {"RegEnumValueW", (void*)&trt_RegEnumValueW},
      {"RegGetValueW", (void*)&trt_RegGetValueW},
      // the token/security half (the honest no-security-model surface)
      {"OpenProcessToken", (void*)&trt_OpenProcessToken},
      {"GetTokenInformation", (void*)&trt_GetTokenInformation},
      {"ConvertStringSecurityDescriptorToSecurityDescriptorW",
       (void*)&trt_ConvertStringSecurityDescriptorToSecurityDescriptorW},
      {"ConvertSidToStringSidW", (void*)&trt_ConvertSidToStringSidW},
      // dstoragecore obfuscation runtime lazy-resolves this by name
      {"SystemFunction036", (void*)&trt_SystemFunction036},
  };
  trt_module_register("advapi32.dll", TRT_ROUTE_BUILTIN, k_advapi32_fns,
                       (int)(sizeof(k_advapi32_fns) / sizeof(k_advapi32_fns[0])));
  return 0;
}

} // extern "C"
