// tiramisu/src/host/audio.cpp
// The real audio output on console. The guest's XAudio2 bridge hands PCM to
// trt_audio_set_hooks; this file owns an XAudio2 2.8 mastering + source
// voice and actually plays it.
//
// Design: one source voice created lazily per (rate, channels, width) —
// games commonly mix 22050/44100/48000 SFX + music streams. Buffers are
// copied (XAudio2 requires memory to stay valid until the buffer completes)
// into a small ring of slots; a slot is reused only after XAudio2 reports
// its buffer consumed. Submit never blocks the guest: if all slots are busy
// the oldest busy slot is stolen (a tiny click beats a deadlock).
#include "audio.h"
#include "host_log.h"

#include "../trt/trt.h"
#include "../trt/bridge_audio.h"

#include <xaudio2.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#pragma comment(lib, "xaudio2_8.lib")
#pragma comment(lib, "ole32.lib")

namespace longan {

namespace {

constexpr int k_slots = 8;          // concurrent buffers in flight
constexpr int k_failed_hr_shift = 0;

struct pcm_slot {
  std::vector<uint8_t> pcm;         // our copy (XAudio2 borrows it)
  XAUDIO2_BUFFER buf{};
  bool busy = false;
  u64 submit_seq = 0;
};

IXAudio2* g_xa = nullptr;
IXAudio2MasteringVoice* g_master = nullptr;
IXAudio2SourceVoice* g_src = nullptr;
std::mutex g_mu;
pcm_slot g_slots[k_slots];
u64 g_seq = 0;
u32 g_rate = 0, g_channels = 0, g_width = 0;
u32 g_submitted = 0;               // buffers handed to XAudio2
u32 g_dropped = 0;                 // slots exhausted
std::atomic<bool> g_started{false};

// reconfigure the source voice when the stream format changes
bool ensure_voice(u32 rate, u32 channels, u32 width) {
  if (g_src && g_rate == rate && g_channels == channels && g_width == width)
    return true;
  if (!g_xa || channels < 1 || channels > 8 || width < 1 || width > 4)
    return false;

  if (g_src) { g_src->DestroyVoice(); g_src = nullptr; }

  WAVEFORMATEX wf{};
  wf.wFormatTag = WAVE_FORMAT_PCM;
  wf.nChannels = (WORD)channels;
  wf.nSamplesPerSec = rate;
  wf.wBitsPerSample = (WORD)(width * 8);
  wf.nBlockAlign = (WORD)(channels * width);
  wf.nAvgBytesPerSec = rate * wf.nBlockAlign;
  const HRESULT hr =
      g_xa->CreateSourceVoice(&g_src, &wf, 0, 2.0f, nullptr,
                              nullptr, nullptr);
  if (FAILED(hr)) {
    char line[128];
    snprintf(line, sizeof(line), "audio: CreateSourceVoice(%u/%u/%ub) hr=0x%08lx",
             rate, channels, width * 8, (unsigned long)hr);
    longan::log_line(line);
    return false;
  }
  g_rate = rate; g_channels = channels; g_width = width;
  if (!g_started.exchange(true)) {
    g_src->Start(0);
    char line[96];
    snprintf(line, sizeof(line), "audio: started %u Hz %u ch %u-bit", rate,
             channels, width * 8);
    longan::log_line(line);
  } else {
    g_src->Start(0);
  }
  return true;
}

// the hook the guest's XAudio2 bridge calls
int sink_submit(const void* pcm, u32 bytes, u32 rate, u32 channels,
                u32 bytes_per_sample, void* user) {
  (void)user;
  if (!pcm || bytes == 0) return 0;
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_xa) return 1;
  if (!ensure_voice(rate, channels, bytes_per_sample)) { ++g_dropped; return 1; }

  // free a slot: prefer genuinely finished ones, then steal the oldest
  int pick = -1;
  u64 oldest = ~0ull;
  int steal = -1;
  for (int i = 0; i < k_slots; ++i) {
    XAUDIO2_VOICE_STATE st{};
    if (g_slots[i].busy) {
      g_src->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
      if (st.BuffersQueued < (UINT32)(k_slots))
        g_slots[i].busy = false;  // XAudio2 freed it between callbacks
    }
    if (!g_slots[i].busy && pick < 0) pick = i;
    if (g_slots[i].busy && g_slots[i].submit_seq < oldest) {
      oldest = g_slots[i].submit_seq;
      steal = i;
    }
  }
  if (pick < 0 && steal >= 0) {
    // all busy: steal the oldest in-flight buffer (rare; short click)
    g_slots[steal].busy = false;
    pick = steal;
    ++g_dropped;
  }
  if (pick < 0) return 1;

  pcm_slot& s = g_slots[pick];
  s.pcm.assign((const uint8_t*)pcm, (const uint8_t*)pcm + bytes);
  memset(&s.buf, 0, sizeof(s.buf));
  s.buf.AudioBytes = bytes;
  s.buf.pAudioData = s.pcm.data();
  s.buf.Flags = XAUDIO2_END_OF_STREAM;
  const HRESULT hr = g_src->SubmitSourceBuffer(&s.buf);
  if (FAILED(hr)) { ++g_dropped; return 1; }
  s.busy = true;
  s.submit_seq = ++g_seq;
  ++g_submitted;
  return 0;
}

} // namespace

void audio_init() {
  // COM apartment for XAudio2 (UWP: CoCreateInstance from the app manifest's
  // allowed set; XAudio2 2.8 is in-box on Xbox)
  const HRESULT hr = XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
  if (FAILED(hr)) {
    char line[128];
    snprintf(line, sizeof(line), "audio: XAudio2Create hr=0x%08lx (guest will run silent)",
             (unsigned long)hr);
    longan::log_line(line);
    g_xa = nullptr;
    return;
  }
  const HRESULT hr2 = g_xa->CreateMasteringVoice(&g_master);
  if (FAILED(hr2)) {
    char line[128];
    snprintf(line, sizeof(line), "audio: CreateMasteringVoice hr=0x%08lx (silent)",
             (unsigned long)hr2);
    longan::log_line(line);
    g_xa = nullptr;
    return;
  }
  char line[96];
  snprintf(line, sizeof(line), "audio: XAudio2 2.8 mastering voice OK (device sink live)");
  longan::log_line(line);

  // publish the sink to the guest bridge
  trt_audio_set_hooks(&sink_submit, nullptr);
}

void audio_status(char* out, unsigned cap) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_xa) { snprintf(out, cap, "audio: no device"); return; }
  snprintf(out, cap, "audio: %u bufs, %u dropped, %u Hz %u ch", g_submitted,
           g_dropped, g_rate, g_channels);
}

void audio_shutdown() {
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_src) { g_src->Stop(0); g_src->DestroyVoice(); g_src = nullptr; }
  if (g_master) g_master = nullptr;
  if (g_xa) g_xa = nullptr;
}

} // namespace longan
