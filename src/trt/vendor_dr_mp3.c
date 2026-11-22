// tiramisu/src/trt/vendor_dr_mp3.c
// dr_mp3 — single-header MP3 decoder (public domain / MIT-0), vendored the
// same way as stb_vorbis. This TU is the ONLY place DR_MP3_IMPLEMENTATION is
// defined (bridge_fmod.cpp includes the declarations).
//
// Why it exists: FMOD payloads are shipped as WAV, OGG, MP3 and FSB banks.
// WAV and OGG decoded for real; MP3 was a named-silent path. dr_mp3 closes
// that container class without a new dependency at build time.
//
// DR_MP3_NO_STDIO: the decoder must not open files itself — the bridge hands
// it an in-memory payload already resolved through the VFS, exactly like the
// Vorbis path.
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "vendor_dr_mp3.h"
