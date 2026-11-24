// tiramisu/tests/test_acm.cpp
// msacm32 used to answer ACMERR_NOTPOSSIBLE for every non-PCM request. The
// codec engine now decodes MS/IMA ADPCM and u-law/A-law and performs PCM format
// conversion, so this test pins the decoder output against hand-computed
// vectors (not against itself), and then exercises the ACMSTREAMHEADER contract
// a game actually drives: open -> prepare -> convert -> close.
#include "../src/trt/trt.h"
#include "../src/trt/acm_codec.h"

#include <cstdio>
#include <cstring>
#include <vector>

extern "C" u32 TRT_GUEST_ABI acm_StreamOpen(void** has_h, void* had,
                                             const void* wfx_src,
                                             const void* wfx_dst, void* filter,
                                             u64 cb, u64 inst, u32 flags);
extern "C" u32 TRT_GUEST_ABI acm_StreamPrepareHeader(void* h, void* hdr, u32 cb);
extern "C" u32 TRT_GUEST_ABI acm_StreamUnprepareHeader(void* h, void* hdr,
                                                        u32 cb);
extern "C" u32 TRT_GUEST_ABI acm_StreamConvert(void* h, void* hdr, u32 cb);
extern "C" u32 TRT_GUEST_ABI acm_StreamSize(void* h, u32 in, u32* out, u32 flags);
extern "C" u32 TRT_GUEST_ABI acm_StreamClose(void* h, u32 flags);
extern "C" u32 TRT_GUEST_ABI acm_FormatSuggest(void* had, const void* src,
                                                void* dst, u32 cb, u32 flags);

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

namespace {

struct wfx_buf {
  u8 b[32];
};

// Build a WAVEFORMATEX (+ optional trailing codec data).
wfx_buf make_wfx(u16 tag, u16 ch, u32 rate, u16 block_align, u16 bits,
                 u16 samples_per_block = 0) {
  wfx_buf w{};
  memset(w.b, 0, sizeof(w.b));
  *(u16*)(w.b + 0) = tag;
  *(u16*)(w.b + 2) = ch;
  *(u32*)(w.b + 4) = rate;
  *(u16*)(w.b + 12) = block_align;
  *(u16*)(w.b + 14) = bits;
  if (samples_per_block) {
    *(u16*)(w.b + 16) = 2;
    *(u16*)(w.b + 18) = samples_per_block;
  }
  *(u32*)(w.b + 8) = (u32)rate * block_align / (samples_per_block ? samples_per_block : 1);
  return w;
}

// ACMSTREAMHEADER as the 64-bit guest lays it out (144 bytes).
struct acm_hdr {
  u32 cb_struct;
  u32 status;
  u64 user;
  const void* src;
  u32 src_len;
  u32 src_used;
  u64 src_user;
  void* dst;
  u32 dst_len;
  u32 dst_used;
  u64 dst_user;
  u64 reserved[10];
};

constexpr u32 kDone = 0x00010000;
constexpr u32 kPrepared = 0x00020000;

}  // namespace

int main() {
  printf("tiramisu ACM codec test\n");

  // ---- format parsing ----------------------------------------------------
  {
    wfx_buf pcm = make_wfx(TRT_ACM_TAG_PCM, 2, 44100, 4, 16);
    trt_acm_fmt f;
    CHECK(trt_acm_parse(pcm.b, &f), "PCM format parses");
    CHECK(f.tag == 1 && f.channels == 2 && f.rate == 44100,
          "PCM fields read at the documented offsets");
    CHECK(trt_acm_parse(nullptr, &f) == false, "a null format is rejected");
  }

  // ---- IMA ADPCM ---------------------------------------------------------
  // One 8-byte mono block: predictor 0, step index 0, then 0x7F followed by
  // six zero nibbles. An IMA nibble is sign-magnitude (bit 3 is the sign, bits
  // 0-2 the magnitude), so 0x7 is +7 and 0xF is -7. Hand-computed through the
  // spec tables:
  //   seed 0
  //   nib 7  step 7  -> +11 = 11   index 0+8  = 8  (step 16)
  //   nib F  step 16 -> -30 = -19  index 8+8  = 16 (step 34)
  //   nib 0  step 34 -> +4  = -15  index 15 (step 31)
  //   nib 0  step 31 -> +3  = -12  index 14 (step 28)
  //   nib 0  step 28 -> +3  = -9   index 13 (step 25)
  //   nib 0  step 25 -> +3  = -6   index 12 (step 23)
  //   nib 0  step 23 -> +2  = -4   index 11 (step 21)
  //   nib 0  step 21 -> +2  = -2   index 10 (step 19)
  {
    wfx_buf src = make_wfx(TRT_ACM_TAG_IMA_ADPCM, 1, 44100, 8, 4, 9);
    wfx_buf dst = make_wfx(TRT_ACM_TAG_PCM, 1, 44100, 2, 16);
    trt_acm_fmt sf, df;
    CHECK(trt_acm_parse(src.b, &sf) && trt_acm_parse(dst.b, &df),
          "IMA ADPCM format parses");
    const u8 block[8] = {0x00, 0x00, 0x00, 0x00, 0x7F, 0x00, 0x00, 0x00};
    u8 out[64];
    memset(out, 0xAA, sizeof(out));
    u32 used_in = 0, used_out = 0;
    const int rc = trt_acm_convert(sf, df, block, sizeof(block), out, sizeof(out),
                                   &used_in, &used_out);
    CHECK(rc == 0, "IMA ADPCM decodes");
    CHECK(used_in == 8, "the whole block is consumed");
    CHECK(used_out == 18, "9 samples become 18 bytes of PCM");
    const i16* s = (const i16*)out;
    CHECK(s[0] == 0 && s[1] == 11 && s[2] == -19,
          "IMA first samples match the hand-computed step table");
    CHECK(s[3] == -15 && s[8] == -2,
          "IMA block completes at the hand-computed value");
    CHECK(trt_acm_out_size(sf, df, 8) == 18,
          "acmStreamSize(SOURCE) predicts the destination bytes");
    CHECK(trt_acm_in_size(sf, df, 18) == 8,
          "acmStreamSize(DEST) predicts the source bytes");
  }

  // ---- MS ADPCM ----------------------------------------------------------
  // Mono 8-byte block: predictor 0, delta 16, s1=1000, s2=900, nibbles 7 then 8.
  //   sample0 = 1000, sample1 = 900
  //   nibble 7: 1000 + 7*16  = 1112  (delta 16 -> 38)
  //   nibble 8: 1112 - 8*38  = 808
  {
    wfx_buf src = make_wfx(TRT_ACM_TAG_MS_ADPCM, 1, 22050, 8, 4, 4);
    wfx_buf dst = make_wfx(TRT_ACM_TAG_PCM, 1, 22050, 2, 16);
    trt_acm_fmt sf, df;
    trt_acm_parse(src.b, &sf);
    trt_acm_parse(dst.b, &df);
    const u8 block[8] = {0x00, 0x10, 0x00, 0xE8, 0x03, 0x84, 0x03, 0x78};
    u8 out[32];
    memset(out, 0xAA, sizeof(out));
    u32 used_in = 0, used_out = 0;
    const int rc = trt_acm_convert(sf, df, block, sizeof(block), out, sizeof(out),
                                   &used_in, &used_out);
    CHECK(rc == 0 && used_out == 8, "MS ADPCM decodes a whole block");
    const i16* s = (const i16*)out;
    CHECK(s[0] == 1000 && s[1] == 900, "MS ADPCM seed samples are preserved");
    CHECK(s[2] == 1112, "MS ADPCM positive nibble matches the adaptation math");
    CHECK(s[3] == 808, "MS ADPCM negative nibble matches the adaptation math");
  }

  // ---- G.711 -------------------------------------------------------------
  {
    wfx_buf src = make_wfx(TRT_ACM_TAG_MULAW, 1, 8000, 1, 8);
    wfx_buf dst = make_wfx(TRT_ACM_TAG_PCM, 1, 8000, 2, 16);
    trt_acm_fmt sf, df;
    trt_acm_parse(src.b, &sf);
    trt_acm_parse(dst.b, &df);
    const u8 ulaw[2] = {0xFF, 0x00};
    u8 out[8];
    u32 used_in = 0, used_out = 0;
    CHECK(trt_acm_convert(sf, df, ulaw, 2, out, sizeof(out), &used_in,
                          &used_out) == 0,
          "u-law decodes");
    const i16* s = (const i16*)out;
    CHECK(s[0] == 0, "u-law 0xFF decodes to silence");
    CHECK(s[1] == -32124, "u-law 0x00 decodes to full negative scale");

    wfx_buf asrc = make_wfx(TRT_ACM_TAG_ALAW, 1, 8000, 1, 8);
    trt_acm_fmt af;
    trt_acm_parse(asrc.b, &af);
    const u8 alaw[2] = {0xD5, 0x55};
    CHECK(trt_acm_convert(af, df, alaw, 2, out, sizeof(out), &used_in,
                          &used_out) == 0,
          "A-law decodes");
    s = (const i16*)out;
    CHECK(s[0] == 8 && s[1] == -8, "A-law sign/magnitude matches the spec");
  }

  // ---- PCM format conversion --------------------------------------------
  {
    wfx_buf src = make_wfx(TRT_ACM_TAG_PCM, 2, 44100, 4, 16);
    wfx_buf dst = make_wfx(TRT_ACM_TAG_PCM, 1, 48000, 2, 16);
    trt_acm_fmt sf, df;
    trt_acm_parse(src.b, &sf);
    trt_acm_parse(dst.b, &df);
    std::vector<i16> in;
    for (int i = 0; i < 441; ++i) {
      in.push_back(1000);  // left
      in.push_back(3000);  // right
    }
    u8 out[2048];
    u32 used_in = 0, used_out = 0;
    CHECK(trt_acm_convert(sf, df, in.data(), (u32)(in.size() * 2), out,
                          sizeof(out), &used_in, &used_out) == 0,
          "stereo 44.1k -> mono 48k converts");
    CHECK(used_out == 480 * 2, "the resampled frame count is exact (441 -> 480)");
    const i16* s = (const i16*)out;
    CHECK(s[0] == 2000, "downmix averages the stereo pair");
    CHECK(s[479] == 2000, "the resampled tail holds the same value");
  }

  // ---- the stream contract a game drives --------------------------------
  {
    wfx_buf src = make_wfx(TRT_ACM_TAG_MS_ADPCM, 1, 22050, 8, 4, 4);
    wfx_buf dst = make_wfx(TRT_ACM_TAG_PCM, 1, 22050, 2, 16);
    void* h = nullptr;
    CHECK(acm_StreamOpen(&h, nullptr, src.b, dst.b, nullptr, 0, 0, 0) == 0 &&
              h != nullptr,
          "an ADPCM stream opens");
    u32 size = 0;
    CHECK(acm_StreamSize(h, 8, &size, 0) == 0 && size == 8,
          "StreamSize reports the converted size");

    const u8 block[8] = {0x00, 0x10, 0x00, 0xE8, 0x03, 0x84, 0x03, 0x78};
    u8 out[32];
    acm_hdr hdr{};
    hdr.cb_struct = sizeof(acm_hdr);
    hdr.src = block;
    hdr.src_len = sizeof(block);
    hdr.dst = out;
    hdr.dst_len = sizeof(out);
    CHECK(sizeof(acm_hdr) == 144, "the test header is the 64-bit ACM layout");
    CHECK(acm_StreamConvert(h, &hdr, sizeof(hdr)) == 0x80006002,
          "convert before prepare is refused as UNPREPARED");
    CHECK(acm_StreamPrepareHeader(h, &hdr, sizeof(hdr)) == 0,
          "prepare accepts the header");
    CHECK((hdr.status & kPrepared) != 0, "prepare sets STATUSF_PREPARED");
    CHECK(acm_StreamConvert(h, &hdr, sizeof(hdr)) == 0, "convert accepted");
    CHECK((hdr.status & kDone) != 0, "convert sets STATUSF_DONE");
    CHECK(hdr.src_used == 8 && hdr.dst_used == 8,
          "convert reports both used lengths");
    CHECK(((const i16*)out)[2] == 1112, "convert wrote real PCM");
    CHECK(acm_StreamUnprepareHeader(h, &hdr, sizeof(hdr)) == 0 &&
              (hdr.status & (kPrepared | kDone)) == 0,
          "unprepare clears the status flags");
    CHECK(acm_StreamClose(h, 0) == 0, "close accepted");
    CHECK(acm_StreamClose(h, 0) == 5, "double close is refused");

    void* bad = nullptr;
    wfx_buf mp3 = make_wfx(0x55, 2, 44100, 4, 16);  // MPEG layer 3
    CHECK(acm_StreamOpen(&bad, nullptr, mp3.b, dst.b, nullptr, 0, 0, 0) ==
              0x80006001u,
          "a codec we cannot decode fails visibly with ACMERR_NOTPOSSIBLE");
    CHECK(bad == nullptr, "a refused open leaves the handle null");

    void* query = nullptr;
    CHECK(acm_StreamOpen(&query, nullptr, src.b, dst.b, nullptr, 0, 0, 1) == 0 &&
              query == nullptr,
          "ACM_STREAMOPENF_QUERY validates without creating a stream");
  }

  // ---- format suggestion -------------------------------------------------
  {
    wfx_buf src = make_wfx(TRT_ACM_TAG_IMA_ADPCM, 2, 32000, 512, 4, 505);
    wfx_buf sug;
    memset(sug.b, 0xCC, sizeof(sug.b));
    CHECK(acm_FormatSuggest(nullptr, src.b, sug.b, 18, 0) == 0,
          "FormatSuggest answers");
    CHECK(*(u16*)(sug.b + 0) == 1, "the suggestion is PCM");
    CHECK(*(u16*)(sug.b + 2) == 2 && *(u32*)(sug.b + 4) == 32000,
          "the suggestion keeps channels and rate");
    CHECK(*(u16*)(sug.b + 14) == 16 && *(u16*)(sug.b + 12) == 4,
          "the suggestion is 16-bit with a matching block align");
  }

  printf("%s: %d checks, %d failed\n",
         failures ? "RESULT: FAIL" : "RESULT: ALL PASS", checks, failures);
  return failures ? 1 : 0;
}
