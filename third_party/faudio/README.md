# FAudio (Path B audio backend for FNA)

FAudio — FNA's XAudio2 reimplementation — built from source as
`libFAudio.so.0` (SDL2 backend) for the Linux harness, and compiled into
the UWP host (XAudio2 backend) for the console — see `uwp/README.md`.

- Source clone: `../faudio-src/` (FAudio master `0c42375`). Never modified
  by the build.
- Linux build: `build-faudio.sh` (CMake, Release, `-DBUILD_SDL3=OFF`).
  Idempotent; `--force` rebuilds.
- Linux output (kept): `out/libFAudio.so.0` (+ `libFAudio.so` symlink).
- UWP build: `tools/build-host.sh` compiles the FAudio core sources plus
  the Tiramisu UWP platform layer (`uwp/FAudio_platform_uwp.c`) straight
  into `tiramisu.exe`; FNA resolves it via the `FAudio` → `__Internal`
  dllmap (`src/trt/mono_host.cpp`). No `FAudio.dll` ships on the console.
- License: zlib — `LICENSE-zlib` (copied from upstream).

Note: Celeste uses FMOD for all its audio, not XNA audio, so FAudio is
only FNA's audio-device dependency here — but FNA still requires it to
initialize its audio device (it bails on `FAudioCreate` / zero devices),
which is why the UWP host carries a real FAudio, not a stub. Audible FNA
audio output on the console is console-verification-pending (no audio
device under Xvfb).
