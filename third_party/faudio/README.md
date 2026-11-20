# FAudio (Path B audio backend for FNA)

FAudio — FNA's XAudio2 reimplementation — built from source as
`libFAudio.so.0` (SDL2 backend).

- Source clone: `../faudio-src/` (FAudio master `0c42375`). Never modified
  by the build.
- Build: `build-faudio.sh` (CMake, Release, `-DBUILD_SDL3=OFF`). Idempotent;
  `--force` rebuilds.
- Output (kept): `out/libFAudio.so.0` (+ `libFAudio.so` symlink).
- License: zlib — `LICENSE-zlib` (copied from upstream).

Note: Celeste uses FMOD for all its audio, not XNA audio, so FAudio is
only FNA's audio-device dependency here. On the console, audio goes
through the FMOD bridge / WASAPI; FAudio is an offline-harness dependency.
