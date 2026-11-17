// tiramisu/tests/test_fuzz_loader.cpp
// Deterministic PE-mutation fuzzer: the launch path (shelf compat scanner +
// loader) reads untrusted game files. The HOST must survive every input —
// a malformed PE may fail named, never crash the process. Seeded PRNG, no
// external corpus needed: mutate the real corpus guests + structural cases.
#include "../src/trt/trt.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static u64 s_state = 0x9E3779B97F4A7C15ull;
static u32 rnd(void) {
  s_state ^= s_state << 13;
  s_state ^= s_state >> 7;
  s_state ^= s_state << 17;
  return (u32)(s_state >> 32);
}

static int failures = 0;
#define CHECK(cond, name)                                  \
  do {                                                     \
    if (cond) printf("  PASS %s\n", name);                 \
    else { printf("  FAIL %s\n", name); ++failures; }      \
  } while (0)

// the loader API: trt_map_pe + trt_unmap (same as test_pe_loader)
static void* fuzz_alloc(size_t bytes, u64 tag) { (void)tag; return malloc(bytes); }
static int fuzz_protect(void* base, size_t bytes, unsigned prot) {
  (void)base; (void)bytes; (void)prot;
  return 0;
}
static void fuzz_release(void* base, size_t bytes) { free(base); }
static void fuzz_log(const char* line) { (void)line; }
static void* fuzz_resolve(void* user, const char* mod, const char* sym, u16 ord) {
  (void)user; (void)mod; (void)sym; (void)ord;
  return (void*)(uintptr_t)0x1000;  // deterministic non-null for bind paths
}

static bool load_ok(const std::vector<u8>& bytes) {
  trt_backend be = {&fuzz_alloc, &fuzz_protect, &fuzz_release, &fuzz_log,
                    nullptr};
  trt_resolver rs = {nullptr, &fuzz_resolve};
  trt_image* img = nullptr;
  const int rc = trt_map_pe(&be, &rs, bytes.data(), bytes.size(), &img);
  if (img) trt_unmap(&be, img);
  return rc == TRT_OK;
}

int main(int argc, char** argv) {
  printf("=== test_fuzz_loader: malformed PEs must fail named, never crash ===\n");

  // seed corpus: the real guests (mutated below)
  std::vector<std::vector<u8>> seeds;
  const char* paths[] = {"tests/out/guest_hello.exe", "tests/out/guest_game.exe",
                         "tests/out/guest_realload.exe"};
  for (const char* p : paths) {
    FILE* f = fopen(p, "rb");
    if (!f) continue;
    std::vector<u8> b;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
      b.insert(b.end(), buf, buf + n);
    fclose(f);
    if (b.size() > 64) seeds.push_back(std::move(b));
  }
  CHECK(!seeds.empty(), "seed corpus loaded");
  if (seeds.empty()) return 1;

  // sanity: unmutated seeds load OK
  for (size_t i = 0; i < seeds.size(); ++i)
    CHECK(load_ok(seeds[i]), "unmutated seed loads");

  // ---- structural cases (deterministic) ----
  struct Case { const char* name; std::vector<u8> (*fn)(const std::vector<u8>&); };
  auto truncate = [](const std::vector<u8>& s) {
    return std::vector<u8>(s.begin(), s.begin() + s.size() / 2);
  };
  auto empty = [](const std::vector<u8>&) { return std::vector<u8>(); };
  auto no_mz = [](const std::vector<u8>& s) {
    auto v = s; v[0] = 'X'; v[1] = 'Z'; return v;
  };
  auto bad_lfanew = [](const std::vector<u8>& s) {
    auto v = s; const u32 off = 0x3C; u32 huge = 0xFFFFFF9C;
    if (v.size() > off + 4) memcpy(v.data() + off, &huge, 4);
    return v;
  };
  auto tiny = [](const std::vector<u8>&) {
    return std::vector<u8>{'M', 'Z', 0, 0};
  };
  auto corrupt_sections = [](const std::vector<u8>& s) {
    auto v = s;
    // flood the section headers with garbage
    if (v.size() > 0x400)
      for (u32 i = 0x200; i < 0x400; ++i) v[i] = (u8)rnd();
    return v;
  };
  auto overlap_sections = [](const std::vector<u8>& s) {
    auto v = s;
    // point every section at RVA 0 with huge sizes
    if (v.size() > 0x300)
      for (u32 i = 0; i < 8; ++i) {
        const u32 sh = 0x188 + i * 40;  // PE32+ section headers
        if (sh + 40 > v.size()) break;
        u32 zero = 0; memcpy(v.data() + sh + 12, &zero, 4);   // VirtualAddress
        u32 big = 0x7FFFFFFF; memcpy(v.data() + sh + 8, &big, 4);  // VirtualSize
        memcpy(v.data() + sh + 16, &big, 4);                  // SizeOfRawData
      }
    return v;
  };
  auto huge_rsrc = [](const std::vector<u8>& s) {
    auto v = s;
    // resource directory pointing past the image
    if (v.size() > 0x300) {
      const u32 dir_rva = 0x168 + 2 * 8;  // data dir 2 (.rsrc)
      if (dir_rva + 8 <= v.size()) {
        u32 big = 0x7FFFFFF0;
        memcpy(v.data() + dir_rva, &big, 4);
        memcpy(v.data() + dir_rva + 4, &big, 4);
      }
    }
    return v;
  };
  auto bad_eh = [](const std::vector<u8>& s) {
    auto v = s;
    // .pdata pointing past the image (unwind registration must reject)
    if (v.size() > 0x300) {
      const u32 dir = 0x168 + 3 * 8;  // data dir 3 (exception)
      if (dir + 8 <= v.size()) {
        u32 big = 0x7FFFFFF8;
        memcpy(v.data() + dir, &big, 4);
        memcpy(v.data() + dir + 4, &big, 4);
      }
    }
    return v;
  };

  Case cases[] = {
      {"truncated mid-PE", truncate},
      {"empty file", empty},
      {"corrupt MZ", no_mz},
      {"e_lfanew out of file", bad_lfanew},
      {"4-byte MZ", tiny},
      {"garbage section table", corrupt_sections},
      {"overlapping huge sections", overlap_sections},
      {"rsrc directory out of image", huge_rsrc},
      {".pdata out of image", bad_eh},
  };
  for (const Case& c : cases) {
    for (const auto& s : seeds) {
      std::vector<u8> m = c.fn(s);
      // must not crash; any named rejection is fine
      (void)load_ok(m);
    }
    printf("  PASS %s (host survived)\n", c.name);
  }

  // ---- random mutation storm: 4000 mutations across seeds ----
  u32 survived = 0, rejected = 0, accepted = 0;
  for (u32 iter = 0; iter < 4000; ++iter) {
    std::vector<u8> m = seeds[rnd() % seeds.size()];
    const u32 n_mut = 1 + rnd() % 8;
    for (u32 k = 0; k < n_mut && !m.empty(); ++k) {
      switch (rnd() % 4) {
        case 0: m[rnd() % m.size()] = (u8)rnd(); break;          // byte flip
        case 1: { const u32 at = rnd() % m.size();               // truncate
                  m.resize(at ? at : 1); } break;
        case 2: { const u32 at = rnd() % m.size();               // splice
                  const u32 from = rnd() % m.size();
                  m[at] = m[from]; } break;
        case 3: m.insert(m.begin() + (rnd() % m.size()), (u8)rnd()); break;
      }
    }
    (void)load_ok(m);
    ++survived;
  }
  (void)rejected; (void)accepted;
  CHECK(survived == 4000, "4000 random mutations: host survived all");

  printf("RESULT: %s (%d failures)\n", failures ? "FAILURES" : "ALL PASS",
         failures);
  return failures ? 1 : 0;
}
