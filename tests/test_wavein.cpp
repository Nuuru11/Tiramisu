// tiramisu/tests/test_wavein.cpp
// waveIn used to report zero devices, which makes an engine disable voice
// capture outright. It is now one real device with the real WAVEHDR contract;
// this checks device enumeration, caps, PCM-only open, and that an added buffer
// comes back completed with recorded bytes.
#include "../src/trt/trt.h"

#include <cstdio>
#include <cstring>
#include <vector>

extern "C" u32 TRT_GUEST_ABI trt_waveInGetNumDevs(void);
extern "C" u32 TRT_GUEST_ABI trt_waveInGetDevCapsW(u64 dev, void* caps, u32 size);
extern "C" u32 TRT_GUEST_ABI trt_waveInGetDevCapsA(u64 dev, void* caps, u32 size);
extern "C" u32 TRT_GUEST_ABI trt_waveInOpen(u64* handle, u32 dev, u64 fmt, u64 cb,
                                            u64 inst, u32 flags);
extern "C" u32 TRT_GUEST_ABI trt_waveInPrepareHeader(u64 hwi, void* hdr, u32 cb);
extern "C" u32 TRT_GUEST_ABI trt_waveInAddBuffer(u64 hwi, void* hdr, u32 cb);
extern "C" u32 TRT_GUEST_ABI trt_waveInUnprepareHeader(u64 hwi, void* hdr, u32 cb);
extern "C" u32 TRT_GUEST_ABI trt_waveInStart(u64 hwi);
extern "C" u32 TRT_GUEST_ABI trt_waveInStop(u64 hwi);
extern "C" u32 TRT_GUEST_ABI trt_waveInReset(u64 hwi);
extern "C" u32 TRT_GUEST_ABI trt_waveInClose(u64 hwi);

static int failures = 0;
static int checks = 0;

#define CHECK(cond, what)                                        \
  do {                                                           \
    ++checks;                                                    \
    if (cond) {                                                  \
      printf("PASS %s\n", what);                                 \
    } else {                                                     \
      printf("FAIL %s (%s:%d)\n", what, __FILE__, __LINE__);      \
      ++failures;                                                \
    }                                                            \
  } while (0)

struct wave_format {
  u16 tag;
  u16 channels;
  u32 rate;
  u32 avg_bytes;
  u16 block_align;
  u16 bits;
  u16 cb_size;
};

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
  CHECK(trt_waveInGetNumDevs() == 1,
        "one capture device is reported (was 0: titles disabled voice)");

  u8 caps_w[80];
  memset(caps_w, 0xCC, sizeof(caps_w));
  CHECK(trt_waveInGetDevCapsW(0, caps_w, sizeof(caps_w)) == 0,
        "device 0 caps readable");
  CHECK(*(u16*)(caps_w + 76) == 1, "caps report mono");
  CHECK((*(u32*)(caps_w + 72) & 0x880) == 0x880,
        "caps advertise 16-bit 44.1/48 kHz capture");
  char name[40];
  const u16* nm = (const u16*)(caps_w + 8);
  int n = 0;
  for (; n < 31 && nm[n]; ++n) name[n] = (char)nm[n];
  name[n] = 0;
  CHECK(name[0] != 0, "caps carry a device name");
  CHECK(trt_waveInGetDevCapsW(1, caps_w, sizeof(caps_w)) == 2,
        "a nonexistent device id is rejected with BADDEVICEID");
  CHECK(trt_waveInGetDevCapsW(0, caps_w, 16) == 11,
        "a too-small caps buffer is rejected");

  u8 caps_a[48];
  CHECK(trt_waveInGetDevCapsA(0, caps_a, sizeof(caps_a)) == 0,
        "ANSI caps readable");
  CHECK(*(u16*)(caps_a + 44) == 1, "ANSI caps report mono");

  u64 hwi = 0xDEADBEEF;
  wave_format fmt{};
  fmt.tag = 1;
  fmt.channels = 1;
  fmt.rate = 22050;
  fmt.avg_bytes = 22050 * 2;
  fmt.block_align = 2;
  fmt.bits = 16;
  CHECK(trt_waveInOpen(&hwi, 0, (u64)(uintptr_t)&fmt, 0, 0, 0) == 0,
        "open accepts PCM");
  CHECK(hwi != 0 && hwi != 0xDEADBEEF, "open returns a real handle");
  CHECK(trt_waveInStart(hwi) == 0, "start accepted");
  CHECK(trt_waveInStop(hwi) == 0, "stop accepted");

  wave_format bad = fmt;
  bad.tag = 0x55;
  u64 h2 = 0;
  CHECK(trt_waveInOpen(&h2, 0, (u64)(uintptr_t)&bad, 0, 0, 0) == 32,
        "a non-PCM capture format is refused, not faked");
  CHECK(h2 == 0, "refused open leaves the handle null");

  std::vector<i16> buf(2205, 0x7777);  // 100 ms, pre-filled with garbage
  wave_hdr64 hdr{};
  hdr.data = buf.data();
  hdr.length = (u32)(buf.size() * sizeof(i16));
  CHECK(trt_waveInPrepareHeader(hwi, &hdr, sizeof(hdr)) == 0,
        "prepare accepts the header");
  CHECK((hdr.flags & 0x02) != 0, "prepare sets WHDR_PREPARED");
  CHECK(trt_waveInAddBuffer(hwi, &hdr, sizeof(hdr)) == 0, "buffer accepted");
  CHECK((hdr.flags & 0x01) != 0, "the buffer comes back WHDR_DONE");
  CHECK((hdr.flags & 0x10) == 0, "the buffer is not left INQUEUE");
  CHECK(hdr.recorded == hdr.length, "every byte reports as recorded");
  CHECK(buf[0] == 0 && buf[buf.size() - 1] == 0,
        "with no capture source the device records silence, not garbage");

  wave_hdr64 empty{};
  CHECK(trt_waveInAddBuffer(hwi, &empty, sizeof(empty)) == 11,
        "a buffer with no data pointer is rejected");

  CHECK(trt_waveInUnprepareHeader(hwi, &hdr, sizeof(hdr)) == 0,
        "unprepare accepted");
  CHECK((hdr.flags & 0x03) == 0, "unprepare clears PREPARED and DONE");
  CHECK(trt_waveInReset(hwi) == 0, "reset accepted");
  CHECK(trt_waveInReset(0x1234) == 4, "reset rejects an invalid handle");
  CHECK(trt_waveInAddBuffer(0x1234, &hdr, sizeof(hdr)) == 4,
        "addBuffer rejects an invalid handle");
  CHECK(trt_waveInClose(hwi) == 0, "close accepted");
  CHECK(trt_waveInClose(hwi) == 4, "double close is refused");

  printf("%s: %d checks, %d failed\n",
         failures ? "RESULT: FAIL" : "RESULT: ALL PASS", checks, failures);
  return failures ? 1 : 0;
}
