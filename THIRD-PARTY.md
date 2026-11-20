# Third-party components

Vendored, with licenses preserved in their directories:

| Component | Where | License | Why |
|---|---|---|---|
| miniz | `src/host/vendor/` | MIT | PNG inflate, zip (phone transfer) |
| stb_image | `src/host/vendor/stb_image.h` | public domain / MIT | image-format fallback loader |
| mojoshader | `third_party/mojoshader` | zlib | GLSL → HLSL translation |
| d3d8to9 | `third_party/d3d8to9` | MIT | D3D8 semantics reference for the d3d8 bridge |
| gl4es | `third_party/gl4es` | MIT | OpenGL surface semantics reference |
| FNA | `third_party/fna-src`, built as `Microsoft.Xna.Framework` 4.0.0.0 in `third_party/fna/out` | MS-PL | XNA 4.0 reimplementation; Celeste's XNA stack under Mono (Path B) |
| FNA3D | `third_party/fna3d-src`, built in `third_party/fna3d/out` | zlib | FNA's 3D renderer (OpenGL offline; D3D11 adaptation is a later step) |
| FAudio | `third_party/faudio-src`, built in `third_party/faudio/out` | zlib | FNA's XAudio2 reimplementation (offline audio-device dependency) |
| Mono 6.8.0.105 | `third_party/mono` (Ubuntu noble `.deb`s, extracted, never installed); `third_party/mono-uwp` (stock 6.8.0.105 source built as a static UWP lib, incl. a TEB fallback typedef from the Unity mono fork) | MIT | Embedding headers + SGen runtime for `src/trt/mono_host.cpp` (shared lib on Linux, static lib on UWP) |
| stb_vorbis | `src/trt/vendor_stb_vorbis.c` | public domain | Ogg Vorbis decoder (v1.24) for FSB5 sample playback in the FMOD Studio bridge |
| TriMO-FSB5 Vorbis setup packets | `src/trt/fsb_vorbis_setups.h` | Apache-2.0 | FSB Vorbis setup-header packets for the 2 CRC32s Celeste banks need (fork of python-fsb5, MIT) |

Everything else in this repository is original code, MIT licensed — see
`LICENSE`. D3D9/D3D8/DDraw bridges follow the published semantics of
D3D9On12 and cnc-ddraw (both MIT) without shipping their code.

No game files, game assets, SDK binaries, or credentials are committed.
