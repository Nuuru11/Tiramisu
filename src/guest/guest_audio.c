// tiramisu/src/guest/guest_audio.c
// The XAudio2 path a shipping game uses:
//   XAudio2Create -> StartEngine -> CreateMasteringVoice -> CreateSourceVoice
//   (PCM format) -> Start -> SubmitSourceBuffer -> GetState -> Stop.
// Returns a bitmask of what worked.
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef unsigned long long u64;
typedef long long i64;
typedef int i32;

// the MSVC linker requires this symbol whenever a floating-point value is used
int _fltused = 1;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

__attribute__((dllimport)) i32 GSTD XAudio2Create(void** out, u32 flags,
                                                  u32 processor);
__attribute__((dllimport)) void GSTD trt_audio_note_submit(u32 bytes);
__attribute__((dllimport)) void GSTD trt_audio_note_start(void);

// read slot N of a COM object's vtable
static void* vmethod(void* obj, u32 slot) {
  void** vtbl = *(void***)obj;
  return vtbl[slot];
}

u32 GSTD audio_boot(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  // 1. create + start the engine
  void* engine = 0;
  if (XAudio2Create(&engine, 0, 1) == 0 && engine) bits |= 0x1u;
  if (((i32 GSTD (*)(void*))vmethod(engine, 8))(engine) == 0)  // StartEngine
    bits |= 0x2u;

  // 2. the mastering voice (defaults: 2ch 48kHz)
  void* master = 0;
  if (((i32 GSTD (*)(void*, void**, u32, u32, u32, const u16*, const void*,
                     u8, u8))vmethod(engine, 7))(engine, &master, 2, 48000, 0,
                                                 0, 0, 0, 0) == 0 &&
      master)
    bits |= 0x4u;

  // 3. a PCM source voice (WAVEFORMATEX: PCM 2ch 48000 16-bit)
  u16 fmt[3];
  u32 fmtd[2];
  fmt[0] = 1;      // WAVE_FORMAT_PCM
  fmt[1] = 2;      // channels
  fmtd[0] = 48000; // sample rate
  fmtd[1] = 48000 * 2 * 2;  // avg bytes/sec
  u16* rest = (u16*)(fmtd + 1);
  (void)rest;
  u8 wave[16];
  u16* w = (u16*)wave;
  w[0] = 1; w[1] = 2;
  u32* d = (u32*)(wave + 4);
  d[0] = 48000; d[1] = 192000;
  u16* tail = (u16*)(wave + 8);
  tail[0] = 4;     // block align
  tail[1] = 16;    // bits per sample

  void* voice = 0;
  if (((i32 GSTD (*)(void*, const void*, void**, u32, float, void*,
                     const void*, u32))vmethod(engine, 5))(
          engine, wave, &voice, 0, 1.0f, 0, 0, 0) == 0 &&
      voice)
    bits |= 0x8u;

  // 4. start it and submit a PCM buffer
  if (voice) {
    ((i32 GSTD (*)(void*, u32, u32))vmethod(voice, 22))(voice, 0, 0); // Start
    trt_audio_note_start();

    // XAUDIO2_BUFFER
    struct { u32 flags; u32 audio_bytes; const void* data;
             u32 play_begin, play_length, loop_begin, loop_length, loop_count;
             void* context; } buf;
    static const u16 pcm[256] = {0};
    buf.flags = 1;  // END_OF_STREAM
    buf.audio_bytes = sizeof(pcm);
    buf.data = pcm;
    buf.play_begin = 0; buf.play_length = 0;
    buf.loop_begin = 0; buf.loop_length = 0; buf.loop_count = 0;
    buf.context = 0;
    if (((i32 GSTD (*)(void*, const void*, const void*))vmethod(voice, 24))(
            voice, &buf, 0) == 0)
      bits |= 0x10u;
    trt_audio_note_submit(buf.audio_bytes);

    // 5. GetState reflects the queue
    struct { void* ctx; u32 queued; u32 pad; u64 played; } state;
    ((void GSTD (*)(void*, void*, u32))vmethod(voice, 28))(voice, &state, 0);
    bits |= (state.queued == 1) ? 0x20u : 0u;

    // 6. flush empties the queue
    if (((i32 GSTD (*)(void*))vmethod(voice, 25))(voice) == 0) {  // Flush
      ((void GSTD (*)(void*, void*, u32))vmethod(voice, 28))(voice, &state, 0);
      bits |= (state.queued == 0) ? 0x40u : 0u;
    }

    // 7. stop
    if (((i32 GSTD (*)(void*, u32, u32))vmethod(voice, 23))(voice, 0, 0) == 0)
      bits |= 0x80u;
  }

  return bits;
}
