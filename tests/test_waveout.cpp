// tiramisu/tests/test_waveout.cpp
// waveOut used to answer "0 devices", which makes a title disable audio
// entirely. It is now one real device feeding the host sink, so this checks the
// parts a game actually depends on: the device is enumerable, the caps are
// readable, open accepts PCM and rejects everything else, and a written buffer
// comes back marked done rather than stuck in the queue.
#include "../src/trt/trt.h"

#include <cstdio>
#include <cstring>
#include <vector>

extern "C" u32 TRT_GUEST_ABI trt_waveOutGetNumDevs(void);
extern "C" u32 TRT_GUEST_ABI trt_waveOutGetDevCapsW(u64 dev, void* caps, u32 size);
extern "C" u32 TRT_GUEST_ABI trt_waveOutGetDevCapsA(u64 dev, void* caps, u32 size);
extern "C" u32 TRT_GUEST_ABI trt_waveOutOpen(u64* handle, u32 dev, u64 fmt, u64 cb,
                                             u64 inst, u32 flags);
extern "C" u32 TRT_GUEST_ABI trt_waveOutPrepareHeader(u64 hwo, void* hdr, u32 cb);
extern "C" u32 TRT_GUEST_ABI trt_waveOutWrite(u64 hwo, void* hdr, u32 cb);
extern "C" u32 TRT_GUEST_ABI trt_waveOutUnprepareHeader(u64 hwo, void* hdr, u32 cb);
extern "C" u32 TRT_GUEST_ABI trt_waveOutGetPosition(u64 hwo, void* time, u32 size);
extern "C" u32 TRT_GUEST_ABI trt_waveOutClose(u64 hwo);
extern "C" u32 TRT_GUEST_ABI trt_waveOutReset(u64 hwo);

static int failures = 0;
static int checks = 0;

#define CHECK(cond, what)                                           \
  do {                                                              \
    ++checks;                                                       \
    if (cond) {                                                     \
      printf("PASS %s\n", what);                                    \
    } else {                                                        \
      printf("FAIL %s (%s:%d)\n", what, __FILE__, __LINE__);         \
      ++failures;                                                   \
    }                                                               \
  } while (0)

// WAVEFORMATEX, PCM 16-bit stereo 48 kHz.
struct wave_format {
  u16 tag;
  u16 channels;
  u32 rate;
  u32 avg_bytes;
  u16 block_align;
  u16 bits;
  u16 cb_size;
};

// 64-bit WAVEHDR: lpData@0, dwBufferLength@8, dwBytesRecorded@12,
// dwUser@16, dwFlags@24, dwLoops@28, lpNext@32, reserved@40 (48 bytes).
struct wave_hdr64 {
  const void* data;
  u32 length;
  u32 recorded;
  u64 user;
  u32 flags;
  u32 loops;
  void* next;
  u64 reserved;
};

int main() {
  CHECK(trt_waveOutGetNumDevs() == 1, "one output device is reported");

  u8 caps_w[84];
  memset(caps_w, 0xCC, sizeof(caps_w));
  CHECK(trt_waveOutGetDevCapsW(0, caps_w, sizeof(caps_w)) == 0,
        "device 0 caps readable");
  CHECK(*(u16*)(caps_w + 76) == 2, "caps report stereo");
  CHECK((*(u32*)(caps_w + 72) & 0x880) == 0x880,
        "caps advertise 16-bit stereo 44.1/48 kHz");
  char name[40];
  const u16* nm = (const u16*)(caps_w + 8);
  int n = 0;
  for (; n < 31 && nm[n]; ++n) name[n] = (char)nm[n];
  name[n] = 0;
  CHECK(name[0] != 0, "caps carry a device name");
  CHECK(trt_waveOutGetDevCapsW(1, caps_w, sizeof(caps_w)) == 2,
        "a nonexistent device id is rejected with BADDEVICEID");
  CHECK(trt_waveOutGetDevCapsW(0, caps_w, 8) == 11,
        "a too-small caps buffer is rejected");

  u8 caps_a[52];
  CHECK(trt_waveOutGetDevCapsA(0, caps_a, sizeof(caps_a)) == 0,
        "ANSI caps readable");
  CHECK(*(u16*)(caps_a + 44) == 2, "ANSI caps report stereo");

  u64 hwo = 0xDEADBEEF;
  wave_format fmt{};
  fmt.tag = 1;
  fmt.channels = 2;
  fmt.rate = 48000;
  fmt.avg_bytes = 48000 * 4;
  fmt.block_align = 4;
  fmt.bits = 16;
  fmt.cb_size = 0;
  CHECK(trt_waveOutOpen(&hwo, 0, (u64)(uintptr_t)&fmt, 0, 0, 0) == 0,
        "open accepts PCM");
  CHECK(hwo != 0 && hwo != 0xDEADBEEF, "open returns a real handle");

  wave_format bad = fmt;
  bad.tag = 0x55;  // MP3
  u64 h2 = 0;
  CHECK(trt_waveOutOpen(&h2, 0, (u64)(uintptr_t)&bad, 0, 0, 0) == 32,
        "a non-PCM format is refused, not faked");
  CHECK(h2 == 0, "refused open leaves the handle null");

  std::vector<i16> pcm(4800 * 2, 0);  // 100 ms of silence
  wave_hdr64 hdr{};
  hdr.data = pcm.data();
  hdr.length = (u32)(pcm.size() * sizeof(i16));
  CHECK(trt_waveOutPrepareHeader(hwo, &hdr, sizeof(hdr)) == 0,
        "prepare accepts the header");
  CHECK((hdr.flags & 0x02) != 0, "prepare sets WHDR_PREPARED");
  CHECK(trt_waveOutWrite(hwo, &hdr, sizeof(hdr)) == 0, "write accepted");
  CHECK((hdr.flags & 0x01) != 0, "write marks WHDR_DONE");
  CHECK((hdr.flags & 0x10) == 0, "write clears WHDR_INQUEUE");

  u8 mmtime[12];
  CHECK(trt_waveOutGetPosition(hwo, mmtime, sizeof(mmtime)) == 0,
        "position readable");
  CHECK(*(u32*)mmtime == 0x2, "position is reported in samples");
  CHECK(*(u32*)(mmtime + 4) == 4800, "position counts the frames written");

  CHECK(trt_waveOutUnprepareHeader(hwo, &hdr, sizeof(hdr)) == 0,
        "unprepare accepted");
  CHECK((hdr.flags & 0x02) == 0, "unprepare clears WHDR_PREPARED");

  CHECK(trt_waveOutReset(hwo) == 0, "reset accepted");
  CHECK(trt_waveOutReset(0x1234) == 4, "reset rejects an invalid handle");
  CHECK(trt_waveOutWrite(0x1234, &hdr, sizeof(hdr)) == 4,
        "write rejects an invalid handle");
  CHECK(trt_waveOutClose(hwo) == 0, "close accepted");
  CHECK(trt_waveOutClose(hwo) == 4, "double close is refused");

  printf("%s: %d checks, %d failed\n",
         failures ? "RESULT: FAIL" : "RESULT: ALL PASS", checks, failures);
  return failures ? 1 : 0;
}
