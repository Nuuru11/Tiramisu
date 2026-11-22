// FSB5 envelope checks: a corrupt bank header must be refused by name, before
// anything is allocated or read.
//
// This runs without a game: the checks are pure numbers. It exists because the
// console heap-corruption crash that silenced Celeste lived in exactly this
// path, and a console is not needed to prove the door is now shut.
#include "../src/trt/bridge_fmod_studio.h"

#include <cstdio>
#include <cstring>

static int g_failures = 0;

static void check(int condition, const char* what) {
  if (condition) {
    std::printf("PASS %s\n", what);
  } else {
    std::printf("FAIL %s\n", what);
    ++g_failures;
  }
}

static const char* header_reason(unsigned samples, unsigned shdr,
                                 unsigned names, unsigned data,
                                 unsigned long long file) {
  const char* why = nullptr;
  if (trt_fsb5_header_sane(samples, shdr, names, data, file, &why)) return "";
  return why ? why : "(null)";
}

static const char* sample_reason(unsigned frames, unsigned channels) {
  const char* why = nullptr;
  if (trt_fsb5_sample_sane(frames, channels, &why)) return "";
  return why ? why : "(null)";
}

int main() {
  // --- real shapes must pass --------------------------------------------
  // Celeste's largest bank: ~700 samples, small header + name tables.
  check(trt_fsb5_header_sane(700, 700 * 8, 700 * 4 + 32, 40u << 20,
                             60ull + 700 * 8ull + 700 * 4ull + 32ull +
                                 (40u << 20), nullptr) == 1,
        "celeste-sized bank accepted");
  // A 1,617,920-frame music sample must survive the decode budget.
  check(trt_fsb5_sample_sane(1617920, 2, nullptr) == 1,
        "1.6M-frame stereo sample accepted");
  // No name table at all is legal (names are optional).
  check(trt_fsb5_header_sane(4, 32, 0, 1024, 60 + 32 + 1024, nullptr) == 1,
        "bank without a name table accepted");

  // --- corrupt headers must be refused, each with its own reason --------
  check(std::strstr(header_reason(0, 8, 8, 0, 1024), "no samples") != nullptr,
        "zero samples refused");
  check(std::strstr(header_reason(1u << 31, 8, 8, 0, 1024), "sample count") != nullptr,
        "sample count overflow refused (name-table u32 wrap)");
  check(std::strstr(header_reason(700, (1u << 20) + 1, 2800, 0, 8u << 20),
                    "1 MiB") != nullptr,
        "1 MiB+ sample-header table refused");
  check(std::strstr(header_reason(700, 16, 2800, 0, 4096), "shorter") != nullptr,
        "sample-header table smaller than 8 bytes/sample refused");
  check(std::strstr(header_reason(700, 5600, (4u << 20) + 1, 0, 8u << 20),
                    "4 MiB") != nullptr,
        "4 MiB+ name table refused");
  check(std::strstr(header_reason(700, 5600, 16, 0, 4096), "index") != nullptr,
        "name table shorter than its index refused");
  check(std::strstr(header_reason(4, 32, 16, 1u << 30, 4096), "exceeds") != nullptr,
        "data size past the end of the file refused");
  check(std::strstr(header_reason(4, 32, 16, 0, 12), "60-byte") != nullptr,
        "file shorter than the FSB5 header refused");

  // --- decode budget ----------------------------------------------------
  check(std::strstr(sample_reason(0, 2), "zero frames") != nullptr,
        "zero-frame sample refused");
  check(std::strstr(sample_reason(100, 0), "channel") != nullptr,
        "zero-channel sample refused");
  check(std::strstr(sample_reason(100, 32), "channel") != nullptr,
        "absurd channel count refused");
  check(std::strstr(sample_reason(0x3FFFFFFF, 2), "20M") != nullptr,
        "0x3FFFFFFF-frame sample refused (memory bomb)");

  std::printf(g_failures ? "RESULT: %d FAILURES\n" : "RESULT: ALL PASS\n",
              g_failures);
  return g_failures ? 1 : 0;
}
