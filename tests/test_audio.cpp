// tiramisu/tests/test_audio.cpp
// XAudio2: the guest creates the engine, voices, submits PCM and polls the
// queue. The host sink (hooked here) counts the PCM bytes it received.
#include "../src/trt/trt.h"
#include "../src/trt/bridge_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_user32(void);
int trt_bridge_register_audio(void);
}

namespace {

struct extent { void* base; size_t len; };
extent g_extents[16];
int g_extent_count = 0;

void* linux_alloc(size_t bytes, u64 preferred_base) {
  void* p = mmap(preferred_base ? (void*)(uintptr_t)preferred_base : nullptr,
                 bytes, PROT_READ | PROT_WRITE,
                 preferred_base ? (MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS)
                                : (MAP_PRIVATE | MAP_ANONYMOUS),
                 -1, 0);
  if (p == MAP_FAILED) return nullptr;
  if (g_extent_count < 16) {
    g_extents[g_extent_count].base = p;
    g_extents[g_extent_count].len = bytes;
    ++g_extent_count;
  }
  return p;
}
int linux_protect(void* base, size_t bytes, unsigned prot) {
  int p = 0;
  if (prot & TRT_PROT_R) p |= PROT_READ;
  if (prot & TRT_PROT_W) p |= PROT_WRITE;
  if (prot & TRT_PROT_X) p |= PROT_EXEC;
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  const uintptr_t b = (uintptr_t)base & ~(uintptr_t)(page - 1);
  const size_t len = ((uintptr_t)base + bytes - b + page - 1) & ~(uintptr_t)(page - 1);
  return mprotect((void*)b, len, p);
}
void linux_release(void* base, size_t bytes) {
  for (int i = 0; i < g_extent_count; ++i)
    if (g_extents[i].base == base) {
      munmap(base, g_extents[i].len);
      g_extents[i].base = nullptr;
      return;
    }
  (void)bytes;
}
void linux_log(const char* line) { printf("  [trt] %s\n", line); }

u8* read_file(const char* path, size_t* size) {
  FILE* f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  u8* buf = (u8*)malloc((size_t)n);
  const size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  *size = got;
  return buf;
}

u64 win64_call(void* fn, u64 a, u64 b, u64 c) {
  u64 args[3] = {a, b, c};
  u64 ret;
  asm volatile(
    "movq %[a0], %%rcx\n"
    "movq %[a1], %%rdx\n"
    "movq %[a2], %%r8\n"
    "subq $48, %%rsp\n"
    "call *%[fn]\n"
    "addq $48, %%rsp\n"
    : "=a"(ret)
    : [a0] "m"(args[0]), [a1] "m"(args[1]), [a2] "m"(args[2]),
      [fn] "r"(fn)
    : "rcx", "rdx", "r8", "r9", "r10", "r11",
      "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "memory", "cc");
  return ret;
}

// ---- the host audio sink (what the console would wire to its real output) ----
int g_submit_count = 0;
u32 g_submit_bytes = 0;
u32 g_submit_rate = 0;
u32 g_submit_channels = 0;
u32 g_submit_width = 0;

int sink_submit(const void* pcm, u32 bytes, u32 rate, u32 channels,
                u32 bytes_per_sample, void* user) {
  (void)pcm; (void)user;
  ++g_submit_count;
  g_submit_bytes += bytes;
  g_submit_rate = rate;
  g_submit_channels = channels;
  g_submit_width = bytes_per_sample;
  return 0;
}

int failures = 0;
#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (cond) printf("  PASS %s\n", msg);                            \
    else { printf("  FAIL %s\n", msg); ++failures; }                 \
  } while (0)

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: test_audio <guest.exe>\n");
    return 2;
  }
  size_t size = 0;
  u8* file = read_file(argv[1], &size);
  if (!file) { printf("cannot read %s\n", argv[1]); return 2; }

  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &linux_log,
                 nullptr};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();
  trt_bridge_register_audio();

  // the console host publishes its real output device the same way
  trt_audio_set_hooks(&sink_submit, nullptr);

  printf("tiramisu XAudio2 test\n");

  trt_resolver rs;
  trt_resolver_default(&rs);
  trt_image* img = nullptr;
  const int rc = trt_map_pe(&be, &rs, file, size, &img);
  CHECK(rc == TRT_OK, "guest PE maps (xaudio2 bound)");
  if (rc != TRT_OK || !img) {
    printf("  map failed: %s (%s)\n", trt_errstr(rc),
           img ? img->last_detail : "no image");
    free(file);
    return 1;
  }
  CHECK(trt_seal_image(&be, img) == TRT_OK, "image sealed (W^X)");

  const u64 ret = win64_call(img->base + img->entry_rva, 0, 0, 0);
  printf("  guest returned 0x%llx (want 0xff)\n", (unsigned long long)ret);

  CHECK(ret & 0x1u, "XAudio2Create returned the engine");
  CHECK(ret & 0x2u, "StartEngine accepted");
  CHECK(ret & 0x4u, "CreateMasteringVoice returned a voice");
  CHECK(ret & 0x8u, "CreateSourceVoice (PCM format) returned a voice");
  CHECK(ret & 0x10u, "SubmitSourceBuffer accepted");
  CHECK(ret & 0x20u, "GetState showed the queued buffer");
  CHECK(ret & 0x40u, "FlushSourceBuffers emptied the queue");
  CHECK(ret & 0x80u, "Stop accepted");

  // the host sink actually received the guest's PCM
  CHECK(g_submit_count >= 1, "the host sink received a buffer");
  CHECK(g_submit_bytes >= 512, "the sink got the PCM bytes (>= 512)");
  CHECK(g_submit_rate == 48000, "the sink got the sample rate (48000)");
  CHECK(g_submit_channels == 2, "the sink got the channel count (2)");
  CHECK(g_submit_width == 2, "the sink got 16-bit samples");

  printf("\nRESULT: %s\n", failures ? "FAILURES" : "ALL PASS");
  return failures ? 1 : 0;
}
