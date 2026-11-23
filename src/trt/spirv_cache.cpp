// tiramisu/src/trt/spirv_cache.cpp
// On-disk SPIR-V -> HLSL cache (see spirv_cache.h).
#include "spirv_cache.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

std::mutex g_mu;
std::string g_dir;
spirv_cache_stats g_stats;
unsigned g_since_prune = 0;

// A single translation is a few KB of HLSL; anything past this is a corrupt
// or hostile file, not a cache entry.
constexpr size_t k_entry_cap = 4u << 20;
// Total on-disk budget. Pruning is checked every k_prune_period writes so a
// busy first run does not pay a directory scan per shader.
constexpr size_t k_total_cap = 128u << 20;
constexpr unsigned k_prune_period = 64;

// Bump when the emitted HLSL changes in a way that makes a stored entry stale.
constexpr const char* k_format_tag = "tiraspv-cache-1";
constexpr char k_magic[8] = {'T', 'S', 'H', 'C', '0', '0', '0', '1'};

std::string join_path(const std::string& a, const char* b) {
  if (a.empty()) return b;
  const char last = a[a.size() - 1];
  if (last == '/' || last == '\\') return a + b;
  return a + "/" + b;
}

bool make_dirs(const std::string& path) {
  if (path.empty()) return false;
  std::string sofar;
  for (size_t i = 0; i <= path.size(); ++i) {
    const bool end = (i == path.size());
    if (!end && path[i] != '/' && path[i] != '\\') continue;
    sofar = path.substr(0, i);
    if (sofar.empty()) continue;
#if defined(_WIN32)
    // 0 = created, EEXIST = already there: both fine.
    if (_mkdir(sofar.c_str()) != 0) {
      if (errno != EEXIST) return false;
    }
#else
    if (mkdir(sofar.c_str(), 0755) != 0 && errno != EEXIST) return false;
#endif
  }
  return true;
}

std::string entry_name(u64 key) {
  char name[32];
  snprintf(name, sizeof(name), "%016llx.shc", (unsigned long long)key);
  return name;
}

void bump(u64 spirv_cache_stats::*field) {
  std::lock_guard<std::mutex> lk(g_mu);
  ++(g_stats.*field);
}

// ---- little-endian-ish codec ---------------------------------------------
// Same machine writes and reads, so host byte order is used as-is; the magic
// and explicit sizes are what reject a truncated or foreign file.
struct wbuf {
  std::string s;
  void bytes(const void* p, size_t n) { s.append((const char*)p, n); }
  void u32v(u32 v) { bytes(&v, sizeof(v)); }
  void strv(const std::string& x) {
    u32v((u32)x.size());
    bytes(x.data(), x.size());
  }
};

struct rbuf {
  const char* p = nullptr;
  size_t n = 0;
  size_t i = 0;
  bool ok = true;
  bool need(size_t k) {
    if (!ok || i + k > n) {
      ok = false;
      return false;
    }
    return true;
  }
  u32 u32v() {
    u32 v = 0;
    if (!need(sizeof(v))) return 0;
    memcpy(&v, p + i, sizeof(v));
    i += sizeof(v);
    return v;
  }
  std::string strv() {
    const u32 k = u32v();
    if (!need(k)) return std::string();
    std::string r(p + i, k);
    i += k;
    return r;
  }
};

void write_ios(wbuf& w, const std::vector<spirv_io_desc>& v) {
  w.u32v((u32)v.size());
  for (const auto& io : v) {
    w.u32v(io.location);
    w.u32v(io.builtin);
    w.strv(io.name);
    w.strv(io.hlsl_type);
  }
}

bool read_ios(rbuf& r, std::vector<spirv_io_desc>* out, u32 count) {
  if (count > 4096) return false;  // a real shader never has this many
  out->clear();
  out->reserve(count);
  for (u32 i = 0; i < count && r.ok; ++i) {
    spirv_io_desc io;
    io.location = r.u32v();
    io.builtin = r.u32v();
    io.name = r.strv();
    io.hlsl_type = r.strv();
    out->push_back(std::move(io));
  }
  return r.ok;
}

struct entry_meta {
  std::string path;
  unsigned long long size = 0;
  long long mtime = 0;
};

bool list_entries(const std::string& dir, std::vector<entry_meta>* out) {
  out->clear();
#if defined(_WIN32)
  const std::string pattern = join_path(dir, "*.shc");
  struct _finddata_t fd;
  intptr_t h = _findfirst(pattern.c_str(), &fd);
  if (h == -1) return false;
  do {
    entry_meta m;
    m.path = join_path(dir, fd.name);
    m.size = (unsigned long long)fd.size;
    m.mtime = (long long)fd.time_write;
    out->push_back(std::move(m));
  } while (_findnext(h, &fd) == 0);
  _findclose(h);
#else
  DIR* d = opendir(dir.c_str());
  if (!d) return false;
  while (struct dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    if (name.size() < 4 || name.compare(name.size() - 4, 4, ".shc") != 0) continue;
    struct stat st;
    const std::string p = join_path(dir, e->d_name);
    if (stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
    entry_meta m;
    m.path = p;
    m.size = (unsigned long long)st.st_size;
    m.mtime = (long long)st.st_mtime;
    out->push_back(std::move(m));
  }
  closedir(d);
#endif
  return true;
}

}  // namespace

bool spirv_cache_open(const std::string& dir) {
  if (dir.empty()) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_dir.clear();
    return false;
  }
  if (!make_dirs(dir)) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_dir.clear();
    return false;
  }
  std::lock_guard<std::mutex> lk(g_mu);
  g_dir = dir;
  return true;
}

std::string spirv_cache_dir() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_dir;
}

void spirv_cache_stats_reset() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_stats = spirv_cache_stats{};
  g_since_prune = 0;
}

spirv_cache_stats spirv_cache_stats_get() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_stats;
}

std::string spirv_cache_stats_line() {
  const spirv_cache_stats s = spirv_cache_stats_get();
  char buf[256];
  snprintf(buf, sizeof(buf),
           "shader cache: %llu hit(s), %llu miss(es), %llu write(s), "
           "%llu rejected, %llu evicted",
           (unsigned long long)s.hits, (unsigned long long)s.misses,
           (unsigned long long)s.writes, (unsigned long long)s.rejected,
           (unsigned long long)s.evicted);
  return buf;
}

u64 spirv_cache_key(const u32* words, u32 word_count, spirv_stage stage,
                    const char* entry_name, const spirv_spec_entry* specs,
                    u32 spec_count) {
  u64 h = 0xcbf29ce484222325ull;
  auto mix = [&h](const void* p, size_t n) {
    const unsigned char* b = (const unsigned char*)p;
    for (size_t i = 0; i < n; ++i) {
      h ^= (u64)b[i];
      h *= 0x100000001b3ull;
    }
  };
  mix(k_format_tag, strlen(k_format_tag) + 1);
  if (words && word_count) mix(words, (size_t)word_count * sizeof(u32));
  const u32 st = (u32)stage;
  mix(&st, sizeof(st));
  if (entry_name) mix(entry_name, strlen(entry_name) + 1);
  for (u32 i = 0; i < spec_count; ++i) {
    mix(&specs[i].spec_id, sizeof(u32));
    mix(&specs[i].value_u32, sizeof(u32));
  }
  return h;
}

std::string spirv_cache_serialize(const spirv_compile_result& r) {
  if (!r.ok) return std::string();
  if (r.hlsl.empty()) return std::string();
  wbuf w;
  w.bytes(k_magic, sizeof(k_magic));
  w.u32v((u32)r.stage);
  w.strv(r.entry_name);
  w.strv(r.hlsl);
  w.strv(r.log);
  for (int i = 0; i < 3; ++i) w.u32v(r.local_size[i]);
  w.u32v(r.push_constant_bytes);
  w.u32v(r.push_constant_reg);
  write_ios(w, r.inputs);
  write_ios(w, r.outputs);
  w.u32v((u32)r.bindings.size());
  for (const auto& b : r.bindings) {
    w.u32v(b.set);
    w.u32v(b.binding);
    w.u32v((u32)b.kind);
    w.u32v(b.hlsl_reg);
    w.u32v(b.smp_reg);
    w.u32v(b.byte_size);
    w.strv(b.name);
  }
  if (w.s.size() > k_entry_cap) return std::string();
  return w.s;
}

bool spirv_cache_deserialize(const std::string& blob, spirv_compile_result* out) {
  if (!out || blob.size() < sizeof(k_magic) || blob.size() > k_entry_cap) {
    return false;
  }
  rbuf r;
  r.p = blob.data();
  r.n = blob.size();
  if (memcmp(blob.data(), k_magic, sizeof(k_magic)) != 0) return false;
  r.i = sizeof(k_magic);

  spirv_compile_result res;
  const u32 stage = r.u32v();
  if (!r.ok || stage > (u32)SPV_STAGE_COMPUTE) return false;
  res.stage = (spirv_stage)stage;
  res.entry_name = r.strv();
  res.hlsl = r.strv();
  res.log = r.strv();
  if (!r.ok || res.hlsl.empty()) return false;
  for (int i = 0; i < 3; ++i) res.local_size[i] = r.u32v();
  res.push_constant_bytes = r.u32v();
  res.push_constant_reg = r.u32v();
  const u32 n_in = r.u32v();
  if (!read_ios(r, &res.inputs, n_in)) return false;
  const u32 n_out = r.u32v();
  if (!read_ios(r, &res.outputs, n_out)) return false;
  const u32 n_bind = r.u32v();
  if (!r.ok || n_bind > 4096) return false;
  res.bindings.reserve(n_bind);
  for (u32 i = 0; i < n_bind && r.ok; ++i) {
    spirv_binding_desc b;
    b.set = r.u32v();
    b.binding = r.u32v();
    const u32 kind = r.u32v();
    if (kind > (u32)SPV_BIND_STORAGE_IMG) return false;
    b.kind = (spirv_binding_kind)kind;
    b.hlsl_reg = r.u32v();
    b.smp_reg = r.u32v();
    b.byte_size = r.u32v();
    b.name = r.strv();
    res.bindings.push_back(std::move(b));
  }
  if (!r.ok) return false;
  // Trailing bytes mean a newer format: refuse rather than half-apply it.
  if (r.i != r.n) return false;
  res.ok = true;
  *out = std::move(res);
  return true;
}

bool spirv_cache_load(u64 key, spirv_compile_result* out) {
  std::string dir;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    dir = g_dir;
  }
  if (dir.empty()) return false;
  const std::string path = join_path(dir, entry_name(key).c_str());
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    bump(&spirv_cache_stats::misses);
    return false;
  }
  std::string blob;
  char buf[8192];
  size_t got = 0;
  while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
    blob.append(buf, got);
    if (blob.size() > k_entry_cap) break;
  }
  fclose(f);
  if (!spirv_cache_deserialize(blob, out)) {
    // A half-written or stale entry is worse than a recompile: drop it so the
    // next save replaces it cleanly.
    remove(path.c_str());
    bump(&spirv_cache_stats::rejected);
    return false;
  }
  bump(&spirv_cache_stats::hits);
  return true;
}

bool spirv_cache_save(u64 key, const spirv_compile_result& r) {
  std::string dir;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    dir = g_dir;
  }
  if (dir.empty()) return false;
  const std::string blob = spirv_cache_serialize(r);
  if (blob.empty()) return false;
  const std::string path = join_path(dir, entry_name(key).c_str());
  const std::string tmp = path + ".tmp";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) return false;
  const size_t wrote = fwrite(blob.data(), 1, blob.size(), f);
  if (fclose(f) != 0 || wrote != blob.size()) {
    remove(tmp.c_str());
    return false;
  }
  // rename() does not replace on Windows: remove the old entry first so a
  // crash mid-write still leaves either the old or the new file, never both.
#if defined(_WIN32)
  remove(path.c_str());
#endif
  if (rename(tmp.c_str(), path.c_str()) != 0) {
    remove(tmp.c_str());
    return false;
  }
  bump(&spirv_cache_stats::writes);

  bool prune_now = false;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    prune_now = (++g_since_prune % k_prune_period) == k_prune_period - 1;
  }
  // Never prune away the entry that was just written: it is almost certainly
  // the one the caller is about to use.
  if (prune_now) spirv_cache_prune(k_total_cap, path.c_str());
  return true;
}

int spirv_cache_prune(size_t cap_bytes, const char* keep_path) {
  std::string dir;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    dir = g_dir;
  }
  if (dir.empty()) return 0;
  std::vector<entry_meta> entries;
  if (!list_entries(dir, &entries) || entries.empty()) return 0;

  unsigned long long total = 0;
  for (const auto& e : entries) total += e.size;
  if (total <= cap_bytes) return 0;

  std::sort(entries.begin(), entries.end(),
            [](const entry_meta& a, const entry_meta& b) {
              if (a.mtime != b.mtime) return a.mtime < b.mtime;  // oldest first
              return a.path < b.path;  // stable when a second is shared
            });
  int removed = 0;
  for (const auto& e : entries) {
    if (total <= cap_bytes) break;
    if (keep_path && e.path == keep_path) continue;
    if (remove(e.path.c_str()) == 0) {
      total -= e.size;
      ++removed;
    }
  }
  if (removed > 0) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_stats.evicted += (u64)removed;
  }
  return removed;
}
