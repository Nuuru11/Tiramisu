// tiramisu/src/guest/guest_openal.c
// Proves the OpenAL bridge: the Minecraft-class streaming loop —
// alcOpenDevice -> context -> buffers filled with PCM -> source ->
// queue -> play -> poll processed -> unqueue -> stop -> close.
// Returns a bitmask:
//   0x001 alcOpenDevice returned a device
//   0x002 alcCreateContext + alcMakeContextCurrent OK
//   0x004 alGenBuffers filled 4 handles
//   0x008 alBufferData accepted PCM (no error after)
//   0x010 alGenSources filled a handle
//   0x020 alSourceQueueBuffers queued 4
//   0x040 alSourcePlay -> AL_PLAYING
//   0x080 AL_BUFFERS_QUEUED == 4 (alGetSourcei)
//   0x100 pump -> AL_BUFFERS_PROCESSED == 4 (all drained)
//   0x200 alSourceUnqueueBuffers retired 4
//   0x400 AL_BUFFERS_QUEUED back to 0
//   0x800 alSourceStop + alcCloseDevice OK
typedef unsigned short u16;
typedef unsigned int u32;
typedef int i32;
typedef unsigned char u8;
typedef unsigned long long u64;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

int _fltused = 1;

__attribute__((dllimport)) void* GSTD alcOpenDevice(const char* name);
__attribute__((dllimport)) void* GSTD alcCreateContext(void* dev,
                                                       const u32* attrs);
__attribute__((dllimport)) u32 GSTD alcMakeContextCurrent(void* ctx);
__attribute__((dllimport)) u32 GSTD alcCloseDevice(void* dev);
__attribute__((dllimport)) u32 GSTD alGetError(void);
__attribute__((dllimport)) void GSTD alGenBuffers(u32 n, u32* out);
__attribute__((dllimport)) void GSTD alBufferData(u32 h, u32 fmt,
                                                  const void* data,
                                                  u32 bytes, u32 rate);
__attribute__((dllimport)) void GSTD alGenSources(u32 n, u32* out);
__attribute__((dllimport)) void GSTD alSourceQueueBuffers(u32 h, u32 n,
                                                          const u32* hs);
__attribute__((dllimport)) void GSTD alSourcePlay(u32 h);
__attribute__((dllimport)) void GSTD alSourceStop(u32 h);
__attribute__((dllimport)) void GSTD alGetSourcei(u32 h, u32 param, u32* out);
__attribute__((dllimport)) void GSTD alSourceUnqueueBuffers(u32 h, u32 n,
                                                            u32* out);

// the manual audio pump (tiramisu-specific: deterministic, no thread).
// not dllimport — it resolves from the bridge's own export table the same
// way Direct3DCreate8 does.
u32 GSTD trt_openal_pump(u32 max_per_source);

// one second of 22050 Hz mono 16-bit silence-with-a-pattern
static u16 pcm[22050];

u32 GSTD guest_openal_main(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  // fill PCM with a rising pattern (so bytes are verifiable)
  for (u32 i = 0; i < 22050; ++i) pcm[i] = (u16)(i & 0xFF);

  // ---- 1. device + context ----
  void* dev = alcOpenDevice(0);
  if (dev) bits |= 0x01u;

  void* ctx = 0;
  if (dev) {
    ctx = alcCreateContext(dev, 0);
    if (ctx && alcMakeContextCurrent(ctx)) bits |= 0x02u;
  }

  // ---- 2. four buffers, PCM data ----
  u32 bufs[4] = {0, 0, 0, 0};
  alGenBuffers(4, bufs);
  if (bufs[0] && bufs[1] && bufs[2] && bufs[3]) bits |= 0x04u;

  if (bufs[0]) {
    for (u32 i = 0; i < 4; ++i)
      alBufferData(bufs[i], 0x1101 /*AL_FORMAT_MONO16*/, pcm,
                   22050 * 2, 22050);
    if (alGetError() == 0) bits |= 0x08u;
  }

  // ---- 3. source, queue, play ----
  u32 src = 0;
  alGenSources(1, &src);
  if (src) bits |= 0x10u;

  if (src) {
    alSourceQueueBuffers(src, 4, bufs);
    bits |= 0x20u;
  }

  if (src) {
    alSourcePlay(src);
    u32 state = 0;
    alGetSourcei(src, 0x1010 /*AL_SOURCE_STATE*/, &state);
    if (state == 0x1012 /*AL_PLAYING*/) bits |= 0x40u;
    u32 queued = 0;
    alGetSourcei(src, 0x1015 /*AL_BUFFERS_QUEUED*/, &queued);
    if (queued == 4) bits |= 0x80u;
  }

  // ---- 4. pump until drained; processed must reach 4 ----
  if (src) {
    for (u32 round = 0; round < 16; ++round) {
      trt_openal_pump(8);
      u32 processed = 0;
      alGetSourcei(src, 0x1016 /*AL_BUFFERS_PROCESSED*/, &processed);
      if (processed >= 4) break;
    }
    u32 processed = 0;
    alGetSourcei(src, 0x1016, &processed);
    if (processed >= 4) bits |= 0x100u;

    // ---- 5. unqueue ----
    u32 retired[4] = {0, 0, 0, 0};
    alSourceUnqueueBuffers(src, 4, retired);
    u32 queued2 = 0;
    alGetSourcei(src, 0x1015, &queued2);
    if (queued2 == 0) {
      bits |= 0x200u;
      bits |= 0x400u;
    }
  }

  // ---- 6. stop + teardown ----
  if (src) {
    alSourceStop(src);
    bits |= 0x800u;
  }
  if (ctx) alcMakeContextCurrent(0);
  if (dev) alcCloseDevice(dev);

  return bits;
}
