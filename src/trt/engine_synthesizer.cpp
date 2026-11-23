// tiramisu/src/trt/engine_synthesizer.cpp
// Autonomous Engine Synthesizer & Adaptive Execution Harness.
//
// Detection order, highest evidence first:
//   1. The chosen exe's PE headers   (PE32 vs PE64, CLR/managed bit)
//   2. Its real import + delay-import module table (mmap'd, bounds-checked)
//   3. Runtime-loaded engine DLLs sitting next to it (Unity/Godot/FNA)
//   4. Data markers on disk (data.win, .pck, .pak, .vpk)
// Nothing is classified from a bare filename guess like "options.ini".
#include "engine_synthesizer.h"
#include "pe.h"
#include "trt.h"
#include "thunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

char lower_char(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

std::string lower_str(std::string s) {
  for (char& c : s) c = lower_char(c);
  return s;
}

std::string path_join(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  char sep = '/';
#ifdef _WIN32
  sep = '\\';
#endif
  if (a.back() == '/' || a.back() == '\\') return a + b;
  return a + sep + b;
}

bool file_exists(const std::string& p) {
#ifdef _WIN32
  DWORD a = GetFileAttributesA(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

bool dir_exists(const std::string& p) {
#ifdef _WIN32
  DWORD a = GetFileAttributesA(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool make_directory(const std::string& p) {
  if (p.empty() || dir_exists(p)) return true;
#ifdef _WIN32
  return CreateDirectoryA(p.c_str(), NULL) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
#else
  return mkdir(p.c_str(), 0755) == 0 || errno == EEXIST;
#endif
}

bool is_junk_executable(const std::string& name) {
  std::string s = lower_str(name);
  return s.find("unitycrashhandler") != std::string::npos ||
         s.find("crashreport") != std::string::npos ||
         s.find("crashpad") != std::string::npos ||
         s.find("ueprereq") != std::string::npos ||
         s.find("ue4prereq") != std::string::npos ||
         s.find("ue5prereq") != std::string::npos ||
         s.find("vcredist") != std::string::npos ||
         s.find("vc_redist") != std::string::npos ||
         s.find("dxsetup") != std::string::npos ||
         s.find("dxwebsetup") != std::string::npos ||
         s.find("dotnet") != std::string::npos ||
         s.find("setup") != std::string::npos ||      // installers, not games
         s.find("install") != std::string::npos ||
         s.find("unins0") != std::string::npos ||
         s.find("uninstall") != std::string::npos ||
         s.find("easyanticheat") != std::string::npos ||
         s.find("battleye") != std::string::npos;
}

// ---------------------------------------------------------------------------
// Read-only whole-file mapping: a 500 MB game exe is inspected without copying
// it, and every access is bounds-checked so a truncated/hostile PE cannot read
// past the mapping.
// ---------------------------------------------------------------------------
class mapped_file {
 public:
  mapped_file() = default;
  ~mapped_file() { close(); }
  mapped_file(const mapped_file&) = delete;
  mapped_file& operator=(const mapped_file&) = delete;

  bool open(const std::string& path) {
    close();
    // .220 SNAPSHOT-ONLY: the Win32 CreateFileA/CreateFileMappingA branch
    // does not compile in this UWP cross-build (neither the A nor the W
    // variants are declared by its windows.h), and the POSIX mmap branch
    // cannot run on Windows. Portable whole-file read instead (a transient
    // heap copy; the file is only inspected for engine detection).
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamoff sz = in.tellg();
    if (sz <= 0) return false;
    in.seekg(0, std::ios::beg);
    u8* buf = (u8*)malloc((size_t)sz);
    if (!buf) return false;
    if (!in.read((char*)buf, sz)) {
      free(buf);
      return false;
    }
    data_ = buf;
    size_ = (size_t)sz;
    owned_ = true;
    return true;
  }

  void close() {
    if (!data_) return;
    if (owned_) {
      free((void*)data_);
    }
#ifdef _WIN32
    else if (win_) UnmapViewOfFile(data_);
#else
    else if (!win_) munmap((void*)data_, size_);
#endif
    data_ = nullptr;
    size_ = 0;
    owned_ = false;
  }

  const u8* data() const { return data_; }
  size_t size() const { return size_; }
  bool valid() const { return data_ != nullptr; }

 private:
  const u8* data_ = nullptr;
  size_t size_ = 0;
  bool win_ = false;
  bool owned_ = false;
};

struct pe_section {
  uint32_t va;
  uint32_t vsize;
  uint32_t raw_size;
  uint32_t raw_ptr;
};

struct pe_scan {
  bool ok = false;
  bool is_pe32 = false;
  bool is_pe64 = false;
  bool is_managed = false;
  uint16_t subsystem = 0;
  std::vector<std::string> modules; // lower-case, unique, sorted
};

bool in_range(size_t off, size_t len, size_t total) {
  return off <= total && len <= total - off;
}

// NUL-terminated string inside the mapping, bounded by `max_len`.
bool cstr_at(const u8* d, size_t total, size_t off, std::string* out,
             size_t max_len = 512) {
  if (off >= total) return false;
  const size_t lim = std::min(max_len, total - off);
  const char* s = (const char*)(d + off);
  if (!memchr(s, 0, lim)) return false;
  out->assign(s);
  return true;
}

size_t rva_to_off(const std::vector<pe_section>& secs, uint32_t rva, size_t total) {
  for (const auto& s : secs) {
    const uint32_t span = s.vsize ? s.vsize : s.raw_size;
    if (span && rva >= s.va && rva - s.va < span) {
      const size_t off = (size_t)s.raw_ptr + (size_t)(rva - s.va);
      if (off < total) return off;
    }
  }
  return (size_t)-1;
}

// Parses the import + delay-import module tables of a PE already in memory.
bool scan_pe_at(const u8* d, size_t total, pe_scan* out) {
  if (!d || total < sizeof(trt_dos_header)) return false;

  trt_dos_header dos{};
  memcpy(&dos, d, sizeof(dos));
  if (dos.e_magic != 0x5A4D) return false; // 'MZ'

  const size_t nt = dos.e_lfanew;
  if (!in_range(nt, 4 + sizeof(trt_file_header) + 2, total)) return false;

  uint32_t sig = 0;
  memcpy(&sig, d + nt, 4);
  if (sig != 0x00004550u) return false; // 'PE\0\0'

  trt_file_header fh{};
  memcpy(&fh, d + nt + 4, sizeof(fh));

  const size_t opt_off = nt + 4 + sizeof(fh);
  uint16_t magic = 0;
  memcpy(&magic, d + opt_off, 2);

  const bool pe32 = (magic == TRT_PE_OPT32_MAGIC);
  const bool pe64 = (magic == 0x020b);
  if (!pe32 && !pe64) return false;
  out->is_pe32 = pe32;
  out->is_pe64 = pe64;

  // Optional header must cover its data directories before we read them.
  const size_t dirs_off = opt_off + (pe32 ? TRT_PE32_OPT_DIRS_OFF : TRT_PE64_OPT_DIRS_OFF);
  if (!in_range(dirs_off, sizeof(trt_data_directory) * TRT_PE_NUM_DIRECTORIES, total)) {
    return false;
  }
  trt_data_directory dirs[TRT_PE_NUM_DIRECTORIES];
  memcpy(dirs, d + dirs_off, sizeof(dirs));
  out->is_managed = dirs[TRT_PE_DIR_COM_DESCRIPTOR].virtual_address != 0;

  // Section table, used to translate RVAs to file offsets.
  std::vector<pe_section> secs;
  const size_t sec_off = opt_off + fh.size_of_optional_header;
  for (uint16_t i = 0; i < fh.number_of_sections; ++i) {
    const size_t o = sec_off + (size_t)i * sizeof(trt_section_header);
    if (!in_range(o, sizeof(trt_section_header), total)) break;
    trt_section_header sh{};
    memcpy(&sh, d + o, sizeof(sh));
    secs.push_back({sh.virtual_address, sh.virtual_size, sh.size_of_raw_data,
                    sh.pointer_to_raw_data});

  // subsystem (never trusted for classification, reported for diagnostics)
  }
  const size_t subsys_off = opt_off + (pe32 ? 68 : 68); // same offset in both
  if (in_range(subsys_off, 2, total)) memcpy(&out->subsystem, d + subsys_off, 2);

  auto add_module = [&](const std::string& raw) {
    std::string m = lower_str(raw);
    if (m.empty()) return;
    for (const auto& existing : out->modules) {
      if (existing == m) return;
    }
    out->modules.push_back(m);
  };

  // ---- import directory ----
  const uint32_t imp_rva = dirs[TRT_PE_DIR_IMPORT].virtual_address;
  if (imp_rva) {
    const size_t imp_off = rva_to_off(secs, imp_rva, total);
    if (imp_off != (size_t)-1) {
      for (size_t i = 0; i < 512; ++i) {
        const size_t o = imp_off + i * sizeof(trt_import_descriptor);
        if (!in_range(o, sizeof(trt_import_descriptor), total)) break;
        trt_import_descriptor desc{};
        memcpy(&desc, d + o, sizeof(desc));
        if (!desc.name && !desc.original_first_thunk && !desc.first_thunk) break;
        if (!desc.name) continue;
        const size_t no = rva_to_off(secs, desc.name, total);
        if (no == (size_t)-1) continue;
        std::string mod;
        if (cstr_at(d, total, no, &mod)) add_module(mod);
      }
    }

  // ---- delay-import directory (RVA-based descriptors only) ----
  }
  const uint32_t dl_rva = dirs[TRT_PE_DIR_DELAY_IMPORT].virtual_address;
  if (dl_rva) {
    const size_t dl_off = rva_to_off(secs, dl_rva, total);
    if (dl_off != (size_t)-1) {
      for (size_t i = 0; i < 256; ++i) {
        const size_t o = dl_off + i * sizeof(trt_delayload_descriptor);
        if (!in_range(o, sizeof(trt_delayload_descriptor), total)) break;
        trt_delayload_descriptor desc{};
        memcpy(&desc, d + o, sizeof(desc));
        if (!desc.name_rva && !desc.iat_rva && !desc.int_rva) break;
        if (!(desc.attributes & 1) || !desc.name_rva) continue; // legacy VA form: skipped
        const size_t no = rva_to_off(secs, desc.name_rva, total);
        if (no == (size_t)-1) continue;
        std::string mod;
        if (cstr_at(d, total, no, &mod)) add_module(mod);
      }
    }
  }

  std::sort(out->modules.begin(), out->modules.end());
  out->modules.erase(std::unique(out->modules.begin(), out->modules.end()),
                     out->modules.end());
  out->ok = true;
  return true;
}

bool scan_pe_file(const std::string& path, pe_scan* out) {
  mapped_file mf;
  if (!mf.open(path)) return false;
  return scan_pe_at(mf.data(), mf.size(), out);
}

// ---------------------------------------------------------------------------
// Engine byte signatures. A binary that delay-loads its engine, or whose
// import names were stripped, still carries the engine's own strings. Only
// unambiguous markers are used (an asset path scheme, a build tag, a product
// name), never a bare "engine" word that would match half the industry.
// ---------------------------------------------------------------------------
struct pe_signatures {
  bool unreal = false;
  bool unity = false;
  bool godot = false;
  bool gamemaker = false;
  bool source = false;
  bool xna = false;
};

bool contains_bytes(const u8* d, size_t total, const char* needle) {
  const size_t nlen = strlen(needle);
  if (!nlen || total < nlen) return false;
  const u8* p = d;
  const u8* end = d + total - nlen + 1;
  const unsigned char first = (unsigned char)needle[0];
  while (p < end) {
    const void* hit = memchr(p, first, (size_t)(end - p));
    if (!hit) return false;
    const u8* h = (const u8*)hit;
    if (memcmp(h, needle, nlen) == 0) return true;
    p = h + 1;
  }
  return false;
}

// Bounded scan: the head and tail of a huge exe carry the build tags and
// asset paths; the middle of a 500 MB recompile adds minutes for no evidence.
bool scan_signatures(const std::string& path, pe_signatures* out) {
  static const char* const kUnreal[] = {"++UE4+Release", "++UE5+Release",
                                        "/Script/Engine", ".uasset", ".umap"};
  static const char* const kUnity[] = {"UnityPlayer.dll", "Unity IL2CPP",
                                       "Assembly-CSharp.dll"};
  static const char* const kGodot[] = {"GodotEngine", "GodotSharp"};
  static const char* const kGm[] = {"YoYo Games", "data.win"};
  static const char* const kSource[] = {"Source Engine", "vstdlib.dll"};
  static const char* const kXna[] = {"XNA Framework", "Microsoft.Xna.Framework"};

  mapped_file mf;
  if (!mf.open(path)) return false;
  const u8* d = mf.data();
  const size_t total = mf.size();

  const size_t kHead = (size_t)128u << 20;
  const size_t kTail = (size_t)16u << 20;
  const size_t head = std::min(total, kHead);
  const size_t tail_off = (total > head) ? total - std::min(total - head, kTail) : total;

  auto any = [&](const char* const* set, size_t count) {
    for (size_t i = 0; i < count; ++i) {
      if (contains_bytes(d, head, set[i])) return true;
      if (tail_off < total && contains_bytes(d + tail_off, total - tail_off, set[i])) {
        return true;
      }
    }
    return false;
  };

  out->unreal = any(kUnreal, sizeof(kUnreal) / sizeof(kUnreal[0]));
  out->unity = any(kUnity, sizeof(kUnity) / sizeof(kUnity[0]));
  out->godot = any(kGodot, sizeof(kGodot) / sizeof(kGodot[0]));
  out->gamemaker = any(kGm, sizeof(kGm) / sizeof(kGm[0]));
  out->source = any(kSource, sizeof(kSource) / sizeof(kSource[0]));
  out->xna = any(kXna, sizeof(kXna) / sizeof(kXna[0]));
  return true;
}

// ---------------------------------------------------------------------------
// Directory walk
// ---------------------------------------------------------------------------
struct exe_candidate {
  std::string rel_path;
  std::string full_path;
  uint64_t size = 0;
  int depth = 0;
  int score = 0;
};

void scan_dir_recursive(const std::string& base_dir, const std::string& rel_dir,
                        int depth, std::vector<exe_candidate>* out_exes,
                        std::vector<std::string>* out_all_files) {
  if (depth > 5) return;
  std::string cur_full = rel_dir.empty() ? base_dir : path_join(base_dir, rel_dir);

#ifdef _WIN32
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(path_join(cur_full, "*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    std::string fn = fd.cFileName;
    if (fn == "." || fn == ".." || fn == "__MACOSX") continue;
    std::string child_rel = rel_dir.empty() ? fn : path_join(rel_dir, fn);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      std::string fn_low = lower_str(fn);
      if (fn_low != "thirdparty" && fn_low != "prereqs" && fn_low != "extras") {
        scan_dir_recursive(base_dir, child_rel, depth + 1, out_exes, out_all_files);
      }
    } else {
      if (out_all_files) out_all_files->push_back(lower_str(child_rel));
      std::string fn_low = lower_str(fn);
      if (fn_low.size() >= 4 && fn_low.substr(fn_low.size() - 4) == ".exe") {
        if (!is_junk_executable(fn)) {
          exe_candidate cand;
          cand.rel_path = child_rel;
          cand.full_path = path_join(base_dir, child_rel);
          cand.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
          cand.depth = depth;
          out_exes->push_back(cand);
        }
      }
    }
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  DIR* d = opendir(cur_full.c_str());
  if (!d) return;
  struct dirent* de;
  while ((de = readdir(d)) != nullptr) {
    std::string fn = de->d_name;
    if (fn == "." || fn == ".." || fn == "__MACOSX") continue;
    std::string child_rel = rel_dir.empty() ? fn : path_join(rel_dir, fn);
    std::string child_full = path_join(base_dir, child_rel);

    struct stat st;
    if (stat(child_full.c_str(), &st) != 0) continue;

    if (S_ISDIR(st.st_mode)) {
      std::string fn_low = lower_str(fn);
      if (fn_low != "thirdparty" && fn_low != "prereqs" && fn_low != "extras") {
        scan_dir_recursive(base_dir, child_rel, depth + 1, out_exes, out_all_files);
      }
    } else if (S_ISREG(st.st_mode)) {
      if (out_all_files) out_all_files->push_back(lower_str(child_rel));
      std::string fn_low = lower_str(fn);
      if (fn_low.size() >= 4 && fn_low.substr(fn_low.size() - 4) == ".exe") {
        if (!is_junk_executable(fn)) {
          exe_candidate cand;
          cand.rel_path = child_rel;
          cand.full_path = child_full;
          cand.size = (uint64_t)st.st_size;
          cand.depth = depth;
          out_exes->push_back(cand);
        }
      }
    }
  }
  closedir(d);
#endif
}

} // namespace

extern "C" {

const char* trt_engine_type_name(trt_engine_type t) {
  switch (t) {
    case TRT_ENGINE_UNITY_IL2CPP: return "Unity (IL2CPP)";
    case TRT_ENGINE_UNITY_MONO: return "Unity (Mono)";
    case TRT_ENGINE_UNREAL: return "Unreal Engine";
    case TRT_ENGINE_GODOT: return "Godot Engine";
    case TRT_ENGINE_GAMEMAKER: return "GameMaker";
    case TRT_ENGINE_FNA_XNA: return "FNA / XNA (Mono Host)";
    case TRT_ENGINE_SOURCE: return "Source Engine";
    case TRT_ENGINE_CUSTOM_NATIVE: return "Custom Native C/C++";
    case TRT_ENGINE_MANAGED_DOTNET: return "Managed (.NET / Mono Host)";
    case TRT_ENGINE_UNIVERSAL: return "Universal (One Engine For Everything)";
    default: return "Unknown Engine";
}

  }
const char* trt_graphics_api_name(trt_graphics_api g) {
  switch (g) {
    case TRT_GFX_D3D11: return "Direct3D 11 (Passthrough)";
    case TRT_GFX_D3D12: return "Direct3D 12 (Recording/Submit)";
    case TRT_GFX_D3D9: return "Direct3D 9 (d3d9to11 Bridge)";
    case TRT_GFX_VULKAN: return "Vulkan (Tiramisu ICD)";
    case TRT_GFX_OPENGL: return "OpenGL (WGL Bridge)";
    default: return "Auto / Default";
}

  }
const char* trt_audio_middleware_name(trt_audio_middleware a) {
  switch (a) {
    case TRT_AUDIO_FMOD: return "FMOD Studio / LowLevel";
    case TRT_AUDIO_XAUDIO2: return "XAudio2";
    case TRT_AUDIO_WASAPI: return "WASAPI Core";
    case TRT_AUDIO_OPENAL: return "OpenAL Soft";
    case TRT_AUDIO_WWISE: return "Wwise (AkSoundEngine)";
    default: return "DirectSound / System";
}

  }
int trt_pe_scan_import_modules(const char* path, char* out_csv, size_t cap) {
  if (out_csv && cap) out_csv[0] = 0;
  if (!path || !path[0]) return -1;
  pe_scan scan;
  if (!scan_pe_file(path, &scan)) return -1;
  if (out_csv && cap) {
    size_t used = 0;
    for (size_t i = 0; i < scan.modules.size(); ++i) {
      const std::string& m = scan.modules[i];
      const size_t need = m.size() + (i ? 1 : 0) + 1;
      if (used + need > cap) break;
      if (i) out_csv[used++] = ',';
      memcpy(out_csv + used, m.data(), m.size());
      used += m.size();
      out_csv[used] = 0;
    }
  }
  return (int)scan.modules.size();
}

bool trt_synthesize_engine(const char* game_dir, const char* target_exe_hint,
                           trt_engine_profile* out_profile) {
  if (!game_dir || !game_dir[0] || !out_profile) return false;

  std::string base_dir = game_dir;
  // Clean trailing separator
  while (base_dir.size() > 1 && (base_dir.back() == '/' || base_dir.back() == '\\')) {
    base_dir.pop_back();

  // Extract base game name
  }
  std::string game_name;
  size_t last_slash = base_dir.find_last_of("/\\");
  if (last_slash != std::string::npos) {
    game_name = base_dir.substr(last_slash + 1);
  } else {
    game_name = base_dir;

  }
  out_profile->game_name = game_name;
  out_profile->game_dir = base_dir;

  // Universal override (manual): force universal even with evidence.
  if (getenv("TRT_FORCE_UNIVERSAL")) {
    target_exe_hint = nullptr; // so the candidate scan still runs
  }

  // 1. Recursive directory scan
  std::vector<exe_candidate> exes;
  std::vector<std::string> all_files;
  scan_dir_recursive(base_dir, "", 0, &exes, &all_files);

  if (exes.empty() && (!target_exe_hint || !target_exe_hint[0])) {
    return false;

  // 2. Score executable candidates to identify the true primary binary
  }
  std::string g_low = lower_str(game_name);
  for (auto& cand : exes) {
    cand.score = 100;
    std::string fn = cand.rel_path;
    size_t s_pos = fn.find_last_of("/\\");
    if (s_pos != std::string::npos) fn = fn.substr(s_pos + 1);
    std::string stem = fn;
    if (stem.size() >= 4 && stem.substr(stem.size() - 4) == ".exe") {
      stem = stem.substr(0, stem.size() - 4);
    }
    std::string stem_low = lower_str(stem);

    // Exact name match or shipping name match
    if (stem_low == g_low) cand.score += 300;
    if (stem_low == g_low + "-win64-shipping") cand.score += 350;
    if (stem_low.find(g_low) != std::string::npos) cand.score += 150;

    // Prefer root or 1-level deep binaries
    if (cand.depth == 0) cand.score += 80;
    else if (cand.depth == 1) cand.score += 40;
    else cand.score -= cand.depth * 25;

    // Prefer substantial file sizes
    if (cand.size > 20000000) cand.score += 60; // > 20MB
    else if (cand.size > 2000000) cand.score += 30; // > 2MB
    else if (cand.size < 100000) cand.score -= 50; // tiny helper stub

    // Prefer hint if provided
    if (target_exe_hint && target_exe_hint[0]) {
      std::string h_low = lower_str(target_exe_hint);
      if (lower_str(cand.rel_path) == h_low || stem_low == h_low) {
        cand.score += 1000;
      }
    }

  // Sort candidates by score descending
  }
  std::sort(exes.begin(), exes.end(), [](const exe_candidate& a, const exe_candidate& b) {
    return a.score > b.score;
  });

  exe_candidate chosen;
  if (!exes.empty()) {
    chosen = exes[0];
  } else if (target_exe_hint && target_exe_hint[0]) {
    chosen.rel_path = target_exe_hint;
    chosen.full_path = path_join(base_dir, target_exe_hint);

  }
  out_profile->main_exe_rel = chosen.rel_path;
  out_profile->main_exe_abs = chosen.full_path;

  // 3. Inspect the chosen exe for real: headers + import/delay-import tables
  pe_scan scan;
  const bool scanned = scan_pe_file(chosen.full_path, &scan);
  if (scanned) {
    out_profile->is_pe32 = scan.is_pe32;
    out_profile->is_pe64 = scan.is_pe64;
    out_profile->is_managed = scan.is_managed;
    out_profile->imported_modules = scan.modules;
  } else {
    // Default fallback assumption
    out_profile->is_pe64 = true;

  }
  auto mod_is = [&](const char* name) {
    const std::string want = lower_str(name);
    return std::find(scan.modules.begin(), scan.modules.end(), want) != scan.modules.end();
  };
  auto mod_prefix = [&](const char* prefix) {
    const std::string p = lower_str(prefix);
    for (const auto& m : scan.modules) {
      if (m.compare(0, p.size(), p) == 0) return true;
    }
    return false;
  };
  auto file_suffix = [&](const char* suffix) {
    const std::string s = lower_str(suffix);
    for (const auto& f : all_files) {
      if (f.size() >= s.size() && f.compare(f.size() - s.size(), s.size(), s) == 0) {
        return true;
      }
    }
    return false;
  };

  // 4. Engine classification - imports first, disk contents second.
  const bool is_managed = out_profile->is_managed;
  // Universal mode: no engine evidence + no is_managed -> TRT_ENGINE_UNIVERSAL.
  // PC games are also universal-first (the universal guest is the "one engine
  // for everything"): classify every unmanaged, no-evidence game as universal
  // unless a higher-evidence engine is found below.
  const bool no_engine_evidence = !is_managed &&
      out_profile->engine_type == TRT_ENGINE_UNKNOWN;
  const bool imp_unity_player = mod_is("unityplayer.dll");
  const bool imp_il2cpp = mod_is("gameassembly.dll");
  const bool imp_mono = mod_prefix("mono-2.0") || mod_is("mono.dll");
  const bool imp_fna = mod_is("fna.dll");
  const bool imp_monogame = mod_prefix("monogame.framework");
  const bool imp_xna = mod_prefix("microsoft.xna.framework");

  pe_signatures sig;
  const bool have_sig = scan_signatures(chosen.full_path, &sig);

  const bool disk_unity_player = file_suffix("unityplayer.dll");
  const bool disk_il2cpp = file_suffix("gameassembly.dll");
  const bool disk_mono =
      file_suffix("mono-2.0.dll") || file_suffix("assembly-csharp.dll") ||
      file_suffix("monobleedingedge.dll");
  const bool disk_fna = file_suffix("fna.dll") || file_suffix("monogame.framework.dll");

  const bool is_unity_il2cpp = imp_il2cpp || disk_il2cpp;
  const bool is_unity_mono =
      imp_unity_player || imp_mono || disk_unity_player || disk_mono ||
      (have_sig && sig.unity);
  const bool is_fna = imp_fna || imp_monogame || imp_xna || disk_fna ||
                      (have_sig && sig.xna);

  // Unreal: monolithic shipping exe, .pak containers, or explicit UE modules.
  const bool is_unreal =
      mod_prefix("ue4game") || mod_prefix("ue5game") || mod_prefix("unrealengine") ||
      file_suffix("-win64-shipping.exe") || file_suffix("-win32-shipping.exe") ||
      file_suffix(".pak") || file_suffix(".utoc") || file_suffix(".ucas") ||
      (have_sig && sig.unreal);
  // Godot: .pck container, a godot runtime module, or the engine's own strings.
  const bool is_godot = mod_prefix("godot") || file_suffix(".pck") ||
                        file_suffix("godotsharp.dll") || (have_sig && sig.godot);
  // GameMaker: the compiled game is data.win / game.unx; nothing else counts.
  const bool is_gamemaker = file_suffix("data.win") || file_suffix("game.unx") ||
                            file_suffix("game.ios") || (have_sig && sig.gamemaker);
  // Source: VPK containers, the tier0/vstdlib pair, or the engine's banner.
  const bool is_source = mod_is("tier0.dll") || mod_is("vstdlib.dll") ||
                         file_suffix(".vpk") || file_suffix("engine.dll") ||
                         (have_sig && sig.source);

  if (is_managed && is_fna) {
    out_profile->engine_type = TRT_ENGINE_FNA_XNA;
  } else if (is_managed && is_unity_mono) {
    out_profile->engine_type = TRT_ENGINE_UNITY_MONO;
  } else if (is_managed) {
    out_profile->engine_type = TRT_ENGINE_MANAGED_DOTNET;
  } else if (is_unity_il2cpp) {
    out_profile->engine_type = TRT_ENGINE_UNITY_IL2CPP;
  } else if (is_unity_mono) {
    out_profile->engine_type = TRT_ENGINE_UNITY_MONO;
  } else if (is_fna) {
    out_profile->engine_type = TRT_ENGINE_FNA_XNA;
  } else if (is_unreal) {
    out_profile->engine_type = TRT_ENGINE_UNREAL;
  } else if (is_godot) {
    out_profile->engine_type = TRT_ENGINE_GODOT;
  } else if (is_gamemaker) {
    out_profile->engine_type = TRT_ENGINE_GAMEMAKER;
  } else if (is_source) {
    out_profile->engine_type = TRT_ENGINE_SOURCE;
  } else if (is_managed) {
    // Managed with no engine evidence — already covered above, but keep a
    // defensive fallback for future expansion.
    out_profile->engine_type = TRT_ENGINE_MANAGED_DOTNET;
  } else if (no_engine_evidence) {
    out_profile->engine_type = TRT_ENGINE_UNIVERSAL;
  } else {
    out_profile->engine_type = TRT_ENGINE_CUSTOM_NATIVE;
  }
  if (out_profile->engine_type == TRT_ENGINE_UNIVERSAL) {
    out_profile->universal_flags |= TRT_UNIV_CLEAR_PC;  // PC games: universal-first
  }

  // 5. Graphics API from the real imports. Engines whose mature Tiramisu path
  //    is D3D11 keep it when the binary imports d3d11, even if d3d12 is also
  //    linked in for a SM6 path.
  const bool imp_d12 = mod_is("d3d12.dll");
  const bool imp_d11 = mod_is("d3d11.dll") || mod_is("d3d10_1.dll") ||
                       mod_is("d3d10.dll") || mod_is("dxgi.dll");
  const bool imp_d9 = mod_is("d3d9.dll") || mod_is("d3d8.dll") || mod_is("ddraw.dll");
  const bool imp_vk = mod_is("vulkan-1.dll");
  const bool imp_gl = mod_is("opengl32.dll");

  const bool prefer_d3d11 = out_profile->engine_type == TRT_ENGINE_UNITY_IL2CPP ||
                            out_profile->engine_type == TRT_ENGINE_UNITY_MONO ||
                            out_profile->engine_type == TRT_ENGINE_UNREAL ||
                            out_profile->engine_type == TRT_ENGINE_FNA_XNA;

  if (out_profile->engine_type == TRT_ENGINE_UNIVERSAL) {
    // Universal guest: prefer a GPU-backed API when the game's imports hint
    // at one (the universal host/guest translates PC imports to Xbox
    // equivalents; this picks the right GPU path to translate into).
    if (imp_d11) out_profile->preferred_gfx = TRT_GFX_D3D11;
    else if (imp_d12) out_profile->preferred_gfx = TRT_GFX_D3D12;
    else if (imp_vk) out_profile->preferred_gfx = TRT_GFX_VULKAN;
    else if (imp_d9) out_profile->preferred_gfx = TRT_GFX_D3D9;
    else if (imp_gl) out_profile->preferred_gfx = TRT_GFX_OPENGL;
  } else if (out_profile->engine_type == TRT_ENGINE_GODOT && imp_vk) {
    out_profile->preferred_gfx = TRT_GFX_VULKAN;
  } else if (prefer_d3d11 && imp_d11) {
    out_profile->preferred_gfx = TRT_GFX_D3D11;
  } else if (imp_d12) {
    out_profile->preferred_gfx = TRT_GFX_D3D12;
  } else if (imp_d11) {
    out_profile->preferred_gfx = TRT_GFX_D3D11;
  } else if (imp_vk) {
    out_profile->preferred_gfx = TRT_GFX_VULKAN;
  } else if (imp_d9) {
    out_profile->preferred_gfx = TRT_GFX_D3D9;
  } else if (imp_gl) {
    out_profile->preferred_gfx = TRT_GFX_OPENGL;
  } else {
    out_profile->preferred_gfx = TRT_GFX_D3D11; // bridge default
  }

  // 5b. Universal guest prefers its own abstracted graphics path by default
  //     (the universal guest is the "one engine"; PC imports translate in the
  //     host/guest at launch, not in the engine profile).
  if (out_profile->engine_type == TRT_ENGINE_UNIVERSAL) {
    if (imp_d11) out_profile->preferred_gfx = TRT_GFX_D3D11;
    else if (imp_d12) out_profile->preferred_gfx = TRT_GFX_D3D12;
    else if (imp_vk) out_profile->preferred_gfx = TRT_GFX_VULKAN;
    else if (imp_d9) out_profile->preferred_gfx = TRT_GFX_D3D9;
    else if (imp_gl) out_profile->preferred_gfx = TRT_GFX_OPENGL;
  }

  // 6. Audio middleware from the real imports.
  const bool imp_fmod = mod_is("fmod.dll") || mod_is("fmod64.dll") ||
                        mod_is("fmodstudio.dll") || mod_is("fmodstudio64.dll") ||
                        mod_prefix("fmod");
  const bool imp_wwise = mod_prefix("aksoundengine") || mod_prefix("akaudio");
  const bool imp_xaudio = mod_prefix("xaudio2") || mod_is("xaudio2.dll");
  const bool imp_openal = mod_is("openal32.dll");

  if (imp_fmod) out_profile->preferred_audio = TRT_AUDIO_FMOD;
  else if (imp_wwise) out_profile->preferred_audio = TRT_AUDIO_WWISE;
  else if (imp_xaudio) out_profile->preferred_audio = TRT_AUDIO_XAUDIO2;
  else if (imp_openal) out_profile->preferred_audio = TRT_AUDIO_OPENAL;
  else out_profile->preferred_audio = TRT_AUDIO_WASAPI;

  // 7. Engine-specific command line (only arguments that engine actually parses).
  switch (out_profile->engine_type) {
    case TRT_ENGINE_UNITY_IL2CPP:
    case TRT_ENGINE_UNITY_MONO:
      out_profile->cmdline_args.push_back("-force-d3d11");
      out_profile->cmdline_args.push_back("-logFile");
      out_profile->cmdline_args.push_back("tiramisu_unity.log");
      break;
    case TRT_ENGINE_UNREAL:
      out_profile->cmdline_args.push_back("-windowed");
      out_profile->cmdline_args.push_back("-NoVerifyGC");
      if (out_profile->preferred_gfx == TRT_GFX_D3D12) {
        out_profile->cmdline_args.push_back("-d3d12");
      } else if (out_profile->preferred_gfx == TRT_GFX_VULKAN) {
        out_profile->cmdline_args.push_back("-vulkan");
      } else {
        out_profile->cmdline_args.push_back("-dx11");
      }
      break;
    default:
      break;

  // 8. Steam: a real steam_api import (or the DLL on disk) needs steam_appid.txt.
  }
  const bool has_steam_api = mod_is("steam_api64.dll") || mod_is("steam_api.dll") ||
                             file_suffix("steam_api64.dll") || file_suffix("steam_api.dll");
  out_profile->has_steam = has_steam_api;
  std::string appid_path = path_join(base_dir, "steam_appid.txt");
  bool appid_found = false;
  if (file_exists(appid_path)) {
    FILE* af = fopen(appid_path.c_str(), "r");
    if (af) {
      unsigned int id = 0;
      if (fscanf(af, "%u", &id) == 1 && id > 0) {
        out_profile->steam_appid = id;
        appid_found = true;
      }
      fclose(af);
    }
  }
  if (!appid_found && has_steam_api) {
    // Autonomous synthesis: write steam_appid.txt (default 480: Spacewar sandbox)
    FILE* af = fopen(appid_path.c_str(), "w");
    if (af) {
      fprintf(af, "480\n");
      fclose(af);
      out_profile->steam_appid = 480;
    }
  }

  // 9. Shims: the bridge modules that will service this title's imports.
  auto add_shim = [&](const char* module, const char* bridge) {
    std::string s = std::string(module) + ":" + bridge;
    if (std::find(out_profile->synthetic_shims.begin(),
                  out_profile->synthetic_shims.end(), s) ==
        out_profile->synthetic_shims.end()) {
      out_profile->synthetic_shims.push_back(s);
    }
  };
  switch (out_profile->preferred_gfx) {
    case TRT_GFX_D3D12: add_shim("d3d12.dll", "bridge_d3d12"); break;
    case TRT_GFX_D3D9: add_shim("d3d9.dll", "bridge_d3d9"); break;
    case TRT_GFX_VULKAN: add_shim("vulkan-1.dll", "bridge_vulkan"); break;
    case TRT_GFX_OPENGL: add_shim("opengl32.dll", "bridge_opengl32"); break;
    default: add_shim("d3d11.dll", "bridge_d3d"); break;
  }
  if (imp_d9 && out_profile->preferred_gfx != TRT_GFX_D3D9) {
    add_shim("d3d9.dll", "bridge_d3d9"); // a D3D9 fallback path still runs
  }
  switch (out_profile->preferred_audio) {
    case TRT_AUDIO_FMOD:
      add_shim("fmod.dll", "bridge_fmod");
      add_shim("fmodstudio.dll", "bridge_fmod_studio");
      break;
    case TRT_AUDIO_WWISE: add_shim("aksoundengine.dll", "bridge_wwise"); break;
    case TRT_AUDIO_OPENAL: add_shim("openal32.dll", "bridge_openal"); break;
    case TRT_AUDIO_XAUDIO2: add_shim("xaudio2.dll", "bridge_audio"); break;
    default: add_shim("wasapi", "bridge_wasapi"); break;
  }
  if (has_steam_api) add_shim("steam_api64.dll", "bridge_steam");
  if (is_managed) add_shim("mscoree.dll", "mono_host");

  // 10. Persist .tiramisu/profile.json (valid JSON: every string escaped) and
  //     .tiramisu/shims.txt.
  std::string meta_dir = path_join(base_dir, ".tiramisu");
  make_directory(meta_dir);

  auto json_escape = [](const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (c < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
          } else {
            out += (char)c;
          }
      }
    }
    return out;
  };

  std::string profile_json_path = path_join(meta_dir, "profile.json");
  FILE* pf = fopen(profile_json_path.c_str(), "w");
  if (pf) {
    fprintf(pf, "{\n");
    fprintf(pf, "  \"game\": \"%s\",\n", json_escape(game_name).c_str());
    fprintf(pf, "  \"engine\": \"%s\",\n", json_escape(trt_engine_type_name(out_profile->engine_type)).c_str());
    fprintf(pf, "  \"architecture\": \"%s\",\n",
            out_profile->is_managed ? "Managed (.NET)"
                                    : (out_profile->is_pe32 ? "PE32 (x86)" : "PE64 (x64)"));
    fprintf(pf, "  \"main_exe\": \"%s\",\n", json_escape(out_profile->main_exe_rel).c_str());
    fprintf(pf, "  \"graphics\": \"%s\",\n", json_escape(trt_graphics_api_name(out_profile->preferred_gfx)).c_str());
    fprintf(pf, "  \"audio\": \"%s\",\n", json_escape(trt_audio_middleware_name(out_profile->preferred_audio)).c_str());
    fprintf(pf, "  \"steam_appid\": %u,\n", out_profile->steam_appid);
    fprintf(pf, "  \"has_steam\": %s,\n", out_profile->has_steam ? "true" : "false");
    fprintf(pf, "  \"universal_flags\": %u,\n", out_profile->universal_flags);
    fprintf(pf, "  \"imports\": [");
    for (size_t i = 0; i < out_profile->imported_modules.size(); ++i) {
      fprintf(pf, "%s\"%s\"", i ? ", " : "",
              json_escape(out_profile->imported_modules[i]).c_str());
    }
    fprintf(pf, "],\n");
    fprintf(pf, "  \"shims\": [");
    for (size_t i = 0; i < out_profile->synthetic_shims.size(); ++i) {
      fprintf(pf, "%s\"%s\"", i ? ", " : "",
              json_escape(out_profile->synthetic_shims[i]).c_str());
    }
    fprintf(pf, "],\n");
    fprintf(pf, "  \"cmdline\": [");
    for (size_t i = 0; i < out_profile->cmdline_args.size(); ++i) {
      fprintf(pf, "%s\"%s\"", i ? ", " : "",
              json_escape(out_profile->cmdline_args[i]).c_str());
    }
    fprintf(pf, "]\n");
    fprintf(pf, "}\n");
    fclose(pf);
  }

  std::string shims_path = path_join(meta_dir, "shims.txt");
  FILE* sf = fopen(shims_path.c_str(), "w");
  if (sf) {
    fprintf(sf, "# tiramisu shims for %s\n", game_name.c_str());
    for (const auto& s : out_profile->synthetic_shims) {
      fprintf(sf, "%s\n", s.c_str());
    }
    fclose(sf);
  }

  // Build user-visible summary string
  char sum[512];
  snprintf(sum, sizeof(sum),
           "synthesizer: auto-configured %s -> %s [%s] gfx=%s audio=%s steam=%u imports=%zu sig=%s",
           game_name.c_str(), out_profile->main_exe_rel.c_str(),
           trt_engine_type_name(out_profile->engine_type),
           trt_graphics_api_name(out_profile->preferred_gfx),
           trt_audio_middleware_name(out_profile->preferred_audio),
           out_profile->steam_appid, out_profile->imported_modules.size(),
           !have_sig ? "none"
                     : (sig.unreal ? "unreal"
                                   : (sig.unity ? "unity"
                                                : (sig.godot ? "godot"
                                                             : (sig.gamemaker
                                                                    ? "gamemaker"
                                                                    : (sig.source
                                                                           ? "source"
                                                                           : (sig.xna ? "xna" : "none")))))));
  out_profile->summary = sum;
  trt_core_log(sum);

  return true;
}

} // extern "C"

