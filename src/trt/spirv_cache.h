// tiramisu/src/trt/spirv_cache.h
// On-disk cache for SPIR-V -> HLSL translations.
//
// The in-memory cache in bridge_vulkan dies with the process, so every launch
// pays the full translation cost again. Shader translation is deterministic
// for a given (SPIR-V, stage, entry, specialization) tuple, so the result can
// be stored and reloaded on the next run instead of rewritten.
//
// The serialized form is endian- and pointer-layout-free because only this
// device reloads it, but it is version-tagged: a compiler change that alters
// the emitted HLSL bumps k_format_tag and old entries stop matching.
#ifndef TIRAMISU_SPIRV_CACHE_H
#define TIRAMISU_SPIRV_CACHE_H

#include "spirv_hlsl.h"

#include <string>

struct spirv_cache_stats {
  u64 hits = 0;      // served from disk without translating
  u64 misses = 0;    // no entry on disk
  u64 writes = 0;    // entries stored
  u64 rejected = 0;  // corrupt / truncated / oversized entries dropped
  u64 evicted = 0;   // entries removed to stay under the size cap
};

// Enable the on-disk cache and point it at dir (created if missing).
// An empty dir disables disk caching but keeps the stats counters working.
bool spirv_cache_open(const std::string& dir);
std::string spirv_cache_dir();

void spirv_cache_stats_reset();
spirv_cache_stats spirv_cache_stats_get();
// One line for the host log: "shader cache: 412 hit(s), 9 miss(es), ...".
std::string spirv_cache_stats_line();

// Key over everything the translation result depends on. Entry names are
// hashed by content, so a guest that reuses a name buffer still hits.
u64 spirv_cache_key(const u32* words, u32 word_count, spirv_stage stage,
                    const char* entry_name, const spirv_spec_entry* specs,
                    u32 spec_count);

// Pure codec (exposed for the offline tests): "" means "not storable".
std::string spirv_cache_serialize(const spirv_compile_result& r);
bool spirv_cache_deserialize(const std::string& blob, spirv_compile_result* out);

// Disk access by key. Load refuses a blob whose stage/entry/hlsl disagree with
// the requested key inputs to the extent it can check them.
bool spirv_cache_load(u64 key, spirv_compile_result* out);
bool spirv_cache_save(u64 key, const spirv_compile_result& r);

// Drop oldest entries until the directory is under cap_bytes. Returns the
// number of files removed (0 when the cache is disabled or already small).
// keep_path (optional) is never removed: it is the entry the caller just
// wrote or is about to read, and several entries often share a modification
// second, so age alone cannot tell them apart.
int spirv_cache_prune(size_t cap_bytes, const char* keep_path = nullptr);

#endif
