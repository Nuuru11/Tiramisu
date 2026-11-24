// tiramisu/tests/test_spirv_cache.cpp
// Offline checks for the on-disk SPIR-V -> HLSL cache: the codec must round-
// trip every field a caller reads (the pipeline builder uses bindings and push
// constants, not just the text), reject anything malformed, and keep the
// directory bounded.
#include "../src/trt/spirv_cache.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, what)                                            \
  do {                                                               \
    ++checks;                                                        \
    if (cond) {                                                      \
      printf("PASS %s\n", what);                                     \
    } else {                                                         \
      printf("FAIL %s (%s:%d)\n", what, __FILE__, __LINE__);          \
      ++failures;                                                    \
    }                                                                \
  } while (0)

static spirv_compile_result make_result(spirv_stage stage, const char* hlsl) {
  spirv_compile_result r;
  r.ok = true;
  r.stage = stage;
  r.entry_name = "main";
  r.hlsl = hlsl;
  r.log = "SPIR-V -> HLSL SM5 ok";
  r.local_size[0] = 8;
  r.local_size[1] = 4;
  r.local_size[2] = 1;
  r.push_constant_bytes = 64;
  r.push_constant_reg = 13;

  spirv_io_desc in;
  in.location = 0;
  in.builtin = 0xFFFFFFFFu;
  in.name = "POSITION";
  in.hlsl_type = "float3";
  r.inputs.push_back(in);

  spirv_io_desc out;
  out.location = 0;
  out.builtin = 0xFFFFFFFFu;
  out.name = "SV_Position";
  out.hlsl_type = "float4";
  r.outputs.push_back(out);

  spirv_binding_desc b;
  b.set = 0;
  b.binding = 2;
  b.kind = SPV_BIND_TEXTURE;
  b.hlsl_reg = 2;
  b.smp_reg = 0;
  b.byte_size = 0;
  b.name = "albedo";
  r.bindings.push_back(b);
  return r;
}

static bool same_result(const spirv_compile_result& a,
                        const spirv_compile_result& b) {
  if (a.ok != b.ok || a.stage != b.stage) return false;
  if (a.entry_name != b.entry_name || a.hlsl != b.hlsl) return false;
  if (a.log != b.log) return false;
  for (int i = 0; i < 3; ++i)
    if (a.local_size[i] != b.local_size[i]) return false;
  if (a.push_constant_bytes != b.push_constant_bytes) return false;
  if (a.push_constant_reg != b.push_constant_reg) return false;
  if (a.inputs.size() != b.inputs.size() || a.outputs.size() != b.outputs.size())
    return false;
  for (size_t i = 0; i < a.inputs.size(); ++i) {
    if (a.inputs[i].location != b.inputs[i].location) return false;
    if (a.inputs[i].name != b.inputs[i].name) return false;
    if (a.inputs[i].hlsl_type != b.inputs[i].hlsl_type) return false;
  }
  for (size_t i = 0; i < a.outputs.size(); ++i) {
    if (a.outputs[i].name != b.outputs[i].name) return false;
    if (a.outputs[i].hlsl_type != b.outputs[i].hlsl_type) return false;
  }
  if (a.bindings.size() != b.bindings.size()) return false;
  for (size_t i = 0; i < a.bindings.size(); ++i) {
    const auto& x = a.bindings[i];
    const auto& y = b.bindings[i];
    if (x.set != y.set || x.binding != y.binding || x.kind != y.kind) return false;
    if (x.hlsl_reg != y.hlsl_reg || x.smp_reg != y.smp_reg) return false;
    if (x.byte_size != y.byte_size || x.name != y.name) return false;
  }
  return true;
}

int main() {
  // ---- codec -------------------------------------------------------------
  const spirv_compile_result src =
      make_result(SPV_STAGE_FRAGMENT, "float4 PS(VSOut _in) : SV_Target { return 1; }\n");
  const std::string blob = spirv_cache_serialize(src);
  CHECK(!blob.empty(), "serialize produces a blob");

  spirv_compile_result back;
  CHECK(spirv_cache_deserialize(blob, &back), "deserialize accepts our own blob");
  CHECK(same_result(src, back), "round-trip preserves every field");

  spirv_compile_result not_ok = src;
  not_ok.ok = false;
  CHECK(spirv_cache_serialize(not_ok).empty(), "a failed translation is never stored");
  spirv_compile_result no_text = src;
  no_text.hlsl.clear();
  CHECK(spirv_cache_serialize(no_text).empty(), "a result without HLSL is never stored");

  std::string bad = blob;
  bad[0] = 'X';
  CHECK(!spirv_cache_deserialize(bad, &back), "bad magic rejected");
  CHECK(!spirv_cache_deserialize(blob.substr(0, blob.size() / 2), &back),
        "truncated blob rejected");
  CHECK(!spirv_cache_deserialize(blob + "junk", &back), "trailing bytes rejected");
  CHECK(!spirv_cache_deserialize(std::string(), &back), "empty blob rejected");

  // ---- keys --------------------------------------------------------------
  u32 words[4] = {0x07230203u, 0x00010000u, 0u, 32u};
  spirv_spec_entry spec{7, 1};
  const u64 k_base = spirv_cache_key(words, 4, SPV_STAGE_VERTEX, "main", nullptr, 0);
  CHECK(k_base == spirv_cache_key(words, 4, SPV_STAGE_VERTEX, "main", nullptr, 0),
        "same inputs give the same key");
  CHECK(k_base != spirv_cache_key(words, 4, SPV_STAGE_FRAGMENT, "main", nullptr, 0),
        "stage changes the key");
  char name_a[8] = "main";
  char name_b[8] = "main";
  CHECK(spirv_cache_key(words, 4, SPV_STAGE_VERTEX, name_a, nullptr, 0) ==
            spirv_cache_key(words, 4, SPV_STAGE_VERTEX, name_b, nullptr, 0),
        "entry name hashed by content, not by pointer");
  CHECK(k_base != spirv_cache_key(words, 4, SPV_STAGE_VERTEX, "other", nullptr, 0),
        "renamed entry changes the key");
  CHECK(k_base != spirv_cache_key(words, 4, SPV_STAGE_VERTEX, "main", &spec, 1),
        "specialization changes the key");
  CHECK(spirv_cache_key(words, 4, SPV_STAGE_VERTEX, "main", &spec, 1) ==
            spirv_cache_key(words, 4, SPV_STAGE_VERTEX, "main", &spec, 1),
        "specialization is stable");
  words[3] = 33;
  CHECK(k_base != spirv_cache_key(words, 4, SPV_STAGE_VERTEX, "main", nullptr, 0),
        "changed SPIR-V changes the key");
  words[3] = 32;

  // ---- disk --------------------------------------------------------------
  char dir[] = "/tmp/tsc_test_XXXXXX";
  CHECK(mkdtemp(dir) != nullptr, "temp cache directory created");
  spirv_cache_stats_reset();
  CHECK(spirv_cache_open(dir), "cache opens on a fresh directory");
  CHECK(spirv_cache_dir() == dir, "cache reports its directory");

  CHECK(spirv_cache_save(k_base, src), "save writes an entry");
  spirv_compile_result loaded;
  CHECK(spirv_cache_load(k_base, &loaded), "load finds the entry");
  CHECK(same_result(src, loaded), "loaded entry matches what was saved");

  const spirv_cache_stats s1 = spirv_cache_stats_get();
  CHECK(s1.writes == 1 && s1.hits == 1 && s1.misses == 0,
        "write and hit counted");

  CHECK(!spirv_cache_load(k_base + 1, &loaded), "unknown key misses");
  CHECK(spirv_cache_stats_get().misses == 1, "miss counted");

  // A corrupt entry must not be served, and must not survive to be re-read.
  char path[512];
  snprintf(path, sizeof(path), "%s/%016llx.shc", dir, (unsigned long long)k_base);
  CHECK(remove(path) == 0, "entry removed for the corruption test");
  FILE* f = fopen(path, "wb");
  CHECK(f != nullptr, "hand-written corrupt entry created");
  if (f) {
    fputs("TSHC0001garbage", f);
    fclose(f);
  }
  CHECK(!spirv_cache_load(k_base, &loaded), "corrupt entry rejected");
  CHECK(spirv_cache_stats_get().rejected == 1, "rejection counted");
  CHECK(fopen(path, "rb") == nullptr, "corrupt entry deleted after rejection");

  // ---- pruning -----------------------------------------------------------
  spirv_cache_stats_reset();
  const spirv_cache_stats before = spirv_cache_stats_get();
  for (int i = 0; i < 40; ++i) {
    char hlsl[256];
    snprintf(hlsl, sizeof(hlsl),
             "float4 PS%d(VSOut _in) : SV_Target { return %d; }\n", i, i);
    const spirv_compile_result r = make_result(SPV_STAGE_FRAGMENT, hlsl);
    if (!spirv_cache_save(k_base + 1000 + (u64)i, r)) break;
  }
  CHECK(spirv_cache_stats_get().writes == 40, "40 entries stored");
  // Prune must never drop the entry the caller is about to use, even when
  // every entry shares one modification second.
  char newest[512];
  snprintf(newest, sizeof(newest), "%s/%016llx.shc", dir,
           (unsigned long long)(k_base + 1000 + 39));
  const int removed = spirv_cache_prune(4096, newest);  // far under the real total
  CHECK(removed > 0, "prune removes entries over the cap");
  CHECK(spirv_cache_stats_get().evicted >= (u64)removed, "eviction counted");
  CHECK(fopen(newest, "rb") != nullptr, "protected entry survives pruning");
  (void)before;

  const std::string line = spirv_cache_stats_line();
  CHECK(line.find("hit") != std::string::npos && line.find("evicted") != std::string::npos,
        "stats line is human readable");

  // Cleanup
  system((std::string("rm -rf ") + dir).c_str());

  printf("%s: %d checks, %d failed\n", failures ? "RESULT: FAIL" : "RESULT: ALL PASS",
         checks, failures);
  return failures ? 1 : 0;
}
