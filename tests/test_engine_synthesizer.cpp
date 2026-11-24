// tiramisu/tests/test_engine_synthesizer.cpp
// Verification of the Autonomous Engine Synthesizer & Dynamic API Synthesizer.
#include "../src/trt/engine_synthesizer.h"
#include "../src/trt/dynamic_api.h"
#include "../src/trt/thunk.h"
#include "../src/trt/trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>

#define CHECK(cond, msg) \
  do { \
    if (!(cond)) { \
      fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
      exit(1); \
    } else { \
      printf("  PASS: %s\n", msg); \
    } \
  } while (0)

namespace {

void* test_alloc(size_t bytes, u64 preferred_base) {
  void* p = mmap(preferred_base ? (void*)(uintptr_t)preferred_base : nullptr,
                 bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return (p == MAP_FAILED) ? nullptr : p;
}

int test_protect(void* base, size_t bytes, unsigned prot) {
  int p = 0;
  if (prot & TRT_PROT_R) p |= PROT_READ;
  if (prot & TRT_PROT_W) p |= PROT_WRITE;
  if (prot & TRT_PROT_X) p |= PROT_EXEC;
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  const uintptr_t b = (uintptr_t)base & ~(uintptr_t)(page - 1);
  const size_t len = ((uintptr_t)base + bytes - b + page - 1) & ~(page - 1);
  return mprotect((void*)b, len, p);
}

void test_release(void* base, size_t bytes) {
  munmap(base, bytes);
}

void make_fake_file(const std::string& path, const std::string& content) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return;
  fwrite(content.data(), 1, content.size(), f);
  fclose(f);
}

// Builds a minimal but structurally real PE whose import directory names the
// given modules, so the scanner is exercised on actual import tables rather
// than on file names.
void make_pe_with_imports(const std::string& path, uint16_t magic,
                          const std::vector<std::string>& modules) {
  const bool pe32 = (magic == 0x10b);
  const uint32_t name_rva = 0x1050;
  std::vector<unsigned char> buf(0x600, 0);

  buf[0] = 'M';
  buf[1] = 'Z';
  const uint32_t e_lfanew = 0x80;
  memcpy(&buf[0x3C], &e_lfanew, 4);

  buf[e_lfanew] = 'P';
  buf[e_lfanew + 1] = 'E';

  const uint16_t nsec = 1;
  const uint16_t opt_size = pe32 ? 224 : 240;
  memcpy(&buf[e_lfanew + 4 + 2], &nsec, 2);
  memcpy(&buf[e_lfanew + 4 + 16], &opt_size, 2);

  const size_t opt = e_lfanew + 24;
  memcpy(&buf[opt], &magic, 2);

  // data directory 1 = import table (RVA/size), at +96 (PE32) / +112 (PE64)
  const size_t dirs = opt + (pe32 ? 96 : 112);
  const uint32_t imp_rva = 0x1000, imp_size = 3 * 20;
  memcpy(&buf[dirs + 1 * 8], &imp_rva, 4);
  memcpy(&buf[dirs + 1 * 8 + 4], &imp_size, 4);

  // one section mapping RVA 0x1000 -> file offset 0x400
  const size_t sec = opt + opt_size;
  memcpy(&buf[sec], ".idata", 6);
  const uint32_t vsize = 0x200, va = 0x1000, raw_size = 0x200, raw_ptr = 0x400;
  memcpy(&buf[sec + 8], &vsize, 4);
  memcpy(&buf[sec + 12], &va, 4);
  memcpy(&buf[sec + 16], &raw_size, 4);
  memcpy(&buf[sec + 20], &raw_ptr, 4);

  // import descriptors: module name RVAs advance by one 16-byte slot
  for (size_t i = 0; i < modules.size() && i < 8; ++i) {
    const size_t d = 0x400 + i * 20;
    const uint32_t n = (uint32_t)(name_rva + i * 16);
    memcpy(&buf[d + 12], &n, 4);
    const size_t noff = 0x400 + (n - va);
    if (noff + modules[i].size() + 1 < buf.size()) {
      memcpy(&buf[noff], modules[i].c_str(), modules[i].size());
    }
  }

  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return;
  fwrite(buf.data(), 1, buf.size(), f);
  fclose(f);
}

void make_fake_pe(const std::string& path, uint16_t magic) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return;

  uint8_t buf[1024];
  memset(buf, 0, sizeof(buf));

  // DOS header
  buf[0] = 'M'; buf[1] = 'Z';
  uint32_t e_lfanew = 0x80;
  memcpy(buf + 0x3C, &e_lfanew, 4);

  // PE signature
  buf[e_lfanew] = 'P'; buf[e_lfanew + 1] = 'E';

  // File header (20 bytes at e_lfanew + 4)
  // Optional header starts at e_lfanew + 24
  memcpy(buf + e_lfanew + 24, &magic, 2);

  fwrite(buf, 1, sizeof(buf), f);
  fclose(f);
}

} // namespace

int main() {
  trt_backend be = {};
  be.alloc = test_alloc;
  be.protect = test_protect;
  be.release = test_release;
  be.log = [](const char* s) { printf("  [trt] %s\n", s); };
  trt_core_set_backend(&be);

  printf("== 1. Dynamic API Return Value & Arity Deduction ==\n");
  {
    uint64_t h = trt_dynamic_deduce_return_val("kernel32.dll", "CreateCustomDevice");
    CHECK(h != 0 && h != (uint64_t)-1, "CreateCustomDevice deduces valid pseudo-handle");
    CHECK((h & 0x77000000ULL) == 0x77000000ULL, "pseudo-handle is in 0x77000000 range");

    uint64_t b = trt_dynamic_deduce_return_val("user32.dll", "SetCustomFeatureEnabled");
    CHECK(b == 1, "SetCustomFeatureEnabled deduces TRUE (1)");

    uint64_t hr = trt_dynamic_deduce_return_val("d3d11.dll", "D3D11CustomCall");
    CHECK(hr == 0, "D3D11CustomCall deduces S_OK (0)");

    uint64_t v = trt_dynamic_deduce_return_val("kernel32.dll", "GetVersionExW");
    CHECK(v == 0x0A00, "GetVersionExW deduces Windows 10 version 0x0A00");

    int a1 = trt_dynamic_deduce_arg_count("kernel32.dll", "CloseHandle");
    CHECK(a1 == 1, "CloseHandle deduced arity 1");

    int a2 = trt_dynamic_deduce_arg_count("kernel32.dll", "GetProcAddress");
    CHECK(a2 == 2, "GetProcAddress deduced arity 2");

    int a7 = trt_dynamic_deduce_arg_count("kernel32.dll", "CreateFileW");
    CHECK(a7 == 7, "CreateFileW deduced arity 7");

    int a12 = trt_dynamic_deduce_arg_count("user32.dll", "CreateWindowExW");
    CHECK(a12 == 12, "CreateWindowExW deduced arity 12");
  }

  printf("== 2. Dynamic Thunk Synthesis & Invocation ==\n");
  {
    void* thunk = trt_dynamic_synthesize_api("mycustom.dll", "CreateMagicPortal", 0);
    CHECK(thunk != nullptr, "synthesized thunk is non-null");
    CHECK(trt_dynamic_is_synthesized("mycustom.dll", "CreateMagicPortal") == 1,
          "symbol marked as synthesized");

    // Call the thunk through a generic Win64 callable
    typedef uint64_t (TRT_STDCALL *portal_fn)(void);
    portal_fn pfn = (portal_fn)thunk;
    uint64_t res = pfn();
    CHECK((res & 0x77000000ULL) == 0x77000000ULL,
          "invoking synthesized thunk returns inferred pseudo-handle");
  }

  printf("== 3. Autonomous Engine Introspection & Profile Synthesis ==\n");
  {
    std::string test_dir = "tests/out/test_synth_unity";
    mkdir("tests/out", 0755);
    mkdir(test_dir.c_str(), 0755);

    // Create Unity IL2CPP file signatures
    make_fake_pe(test_dir + "/Game.exe", 0x20b); // PE64
    make_fake_file(test_dir + "/UnityPlayer.dll", "fake unity player");
    make_fake_file(test_dir + "/GameAssembly.dll", "fake il2cpp assembly");
    make_fake_file(test_dir + "/steam_api64.dll", "fake steam");

    trt_engine_profile prof;
    bool ok = trt_synthesize_engine(test_dir.c_str(), nullptr, &prof);
    CHECK(ok, "engine synthesis succeeded for Unity IL2CPP game");
    CHECK(prof.engine_type == TRT_ENGINE_UNITY_IL2CPP, "classified as Unity IL2CPP");
    CHECK(prof.is_pe64 && !prof.is_pe32, "detected PE64 architecture");
    CHECK(prof.preferred_gfx == TRT_GFX_D3D11, "preferred graphics is D3D11");
    CHECK(prof.has_steam, "detected Steam API dependency");
    CHECK(prof.steam_appid == 480, "synthesized sandbox Steam AppID 480");

    // Check that steam_appid.txt was created
    FILE* af = fopen((test_dir + "/steam_appid.txt").c_str(), "r");
    CHECK(af != nullptr, "steam_appid.txt was auto-synthesized on disk");
    if (af) {
      int id = 0;
      fscanf(af, "%d", &id);
      CHECK(id == 480, "steam_appid.txt holds AppID 480");
      fclose(af);
    }

    // Check that .tiramisu/profile.json was created
    FILE* pf = fopen((test_dir + "/.tiramisu/profile.json").c_str(), "r");
    CHECK(pf != nullptr, ".tiramisu/profile.json was auto-generated");
    if (pf) fclose(pf);

    // Verify command line arguments
    bool has_force_d3d11 = false;
    for (const auto& arg : prof.cmdline_args) {
      if (arg == "-force-d3d11") has_force_d3d11 = true;
    }
    CHECK(has_force_d3d11, "synthesized -force-d3d11 command line argument");
  }

  printf("== 4. Engine Synthesis on 32-bit GameMaker Game ==\n");
  {
    std::string gm_dir = "tests/out/test_synth_gm";
    mkdir(gm_dir.c_str(), 0755);

    make_fake_pe(gm_dir + "/RetroGame.exe", 0x10b); // PE32
    make_fake_file(gm_dir + "/data.win", "gamemaker bytecode data");
    make_fake_file(gm_dir + "/audiogroup1.dat", "audio data");

    trt_engine_profile prof;
    bool ok = trt_synthesize_engine(gm_dir.c_str(), nullptr, &prof);
    CHECK(ok, "engine synthesis succeeded for GameMaker game");
    CHECK(prof.engine_type == TRT_ENGINE_GAMEMAKER, "classified as GameMaker");
    CHECK(prof.is_pe32 && !prof.is_pe64, "detected 32-bit PE32 architecture");
  }

  printf("== 5. Real PE import-table scanning ==\n");
  {
    std::string imp_dir = "tests/out/test_synth_imports";
    mkdir(imp_dir.c_str(), 0755);
    const std::string exe = imp_dir + "/Game.exe";
    make_pe_with_imports(exe, 0x10b, {"D3D11.dll", "steam_api.dll"});

    char csv[512] = {};
    int n = trt_pe_scan_import_modules(exe.c_str(), csv, sizeof(csv));
    CHECK(n == 2, "import scanner found exactly 2 modules");
    CHECK(strcmp(csv, "d3d11.dll,steam_api.dll") == 0,
          "scanner reports lower-case module CSV");
    CHECK(trt_pe_scan_import_modules("tests/out/does_not_exist.exe", csv, sizeof(csv)) == -1,
          "missing file reports -1, never a fake success");

    trt_engine_profile prof;
    CHECK(trt_synthesize_engine(imp_dir.c_str(), nullptr, &prof),
          "synthesis succeeded from a real import table");
    CHECK(prof.is_pe32 && !prof.is_pe64, "import fixture is PE32");
    CHECK(prof.preferred_gfx == TRT_GFX_D3D11, "d3d11.dll import selects D3D11");
    CHECK(prof.has_steam, "steam_api.dll import flags Steam");
    CHECK(prof.imported_modules.size() == 2, "profile carries the imported modules");

    bool has_d3d11_shim = false, has_steam_shim = false;
    for (const auto& s : prof.synthetic_shims) {
      if (s == "d3d11.dll:bridge_d3d") has_d3d11_shim = true;
      if (s == "steam_api64.dll:bridge_steam") has_steam_shim = true;
    }
    CHECK(has_d3d11_shim, "synthesized the D3D11 bridge shim");
    CHECK(has_steam_shim, "synthesized the Steam bridge shim");

    std::ifstream pj(imp_dir + "/.tiramisu/profile.json");
    std::string js((std::istreambuf_iterator<char>(pj)), std::istreambuf_iterator<char>());
    CHECK(js.find("\"imports\"") != std::string::npos &&
              js.find("d3d11.dll") != std::string::npos,
          "profile.json records the real imports");
    CHECK(js.find("\"shims\"") != std::string::npos, "profile.json records the shims");
    CHECK(js.find("\"architecture\": \"PE32 (x86)\"") != std::string::npos,
          "profile.json records the architecture");

    std::ifstream sh(imp_dir + "/.tiramisu/shims.txt");
    CHECK(sh.good(), "shims.txt was written");
  }

  printf("== 6. D3D9-only title resolves to the D3D9 bridge ==\n");
  {
    std::string d9_dir = "tests/out/test_synth_d3d9";
    mkdir(d9_dir.c_str(), 0755);
    make_pe_with_imports(d9_dir + "/OldGame.exe", 0x10b, {"d3d9.dll", "dsound.dll"});

    trt_engine_profile prof;
    CHECK(trt_synthesize_engine(d9_dir.c_str(), nullptr, &prof), "D3D9 synthesis succeeded");
    CHECK(prof.preferred_gfx == TRT_GFX_D3D9, "d3d9.dll import selects the D3D9 bridge");
  }

  printf("== 7. options.ini alone is not GameMaker ==\n");
  {
    std::string opt_dir = "tests/out/test_synth_options";
    mkdir(opt_dir.c_str(), 0755);
    make_fake_pe(opt_dir + "/PlainGame.exe", 0x20b);
    make_fake_file(opt_dir + "/options.ini", "[video]\nwidth=1280\n");

    trt_engine_profile prof;
    CHECK(trt_synthesize_engine(opt_dir.c_str(), nullptr, &prof), "plain game synthesis succeeded");
    CHECK(prof.engine_type != TRT_ENGINE_GAMEMAKER,
          "a generic options.ini no longer misclassifies as GameMaker");
    // Policy (host/main.cpp has a dedicated TRT_ENGINE_UNIVERSAL launch path):
    // an unmanaged binary with no engine evidence is the universal guest.
    // Custom Native is for binaries that carry evidence of an in-house engine,
    // not for "the scanner found nothing".
    CHECK(prof.engine_type == TRT_ENGINE_UNIVERSAL,
          "no engine evidence resolves to the universal guest");
    CHECK((prof.universal_flags & TRT_UNIV_CLEAR_PC) != 0,
          "universal classification sets the PC clear flag");
  }

  printf("== 8. Engine byte-signature detection (stripped imports) ==\n");
  {
    std::string ue_dir = "tests/out/test_synth_ue";
    mkdir(ue_dir.c_str(), 0755);
    make_fake_pe(ue_dir + "/Shipped.exe", 0x20b);
    // A shipping exe whose engine is only visible as its own build tag, the
    // case that file names and imports both miss.
    FILE* af = fopen((ue_dir + "/Shipped.exe").c_str(), "ab");
    CHECK(af != nullptr, "marker exe opened for append");
    if (af) {
      const char* tag = "++UE4+Release-4.27-CL-18319896\0";
      fwrite(tag, 1, strlen(tag) + 1, af);
      fclose(af);
    }

    trt_engine_profile prof;
    CHECK(trt_synthesize_engine(ue_dir.c_str(), nullptr, &prof),
          "signature-only synthesis succeeded");
    CHECK(prof.engine_type == TRT_ENGINE_UNREAL,
          "++UE4+Release tag classifies as Unreal");
    bool has_windowed = false;
    for (const auto& a : prof.cmdline_args) {
      if (a == "-windowed") has_windowed = true;
    }
    CHECK(has_windowed, "Unreal profile carries -windowed");
  }

  printf("ALL ENGINE SYNTHESIZER CHECKS PASSED CLEANLY.\n");
  return 0;
}
