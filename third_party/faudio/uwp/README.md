# FAudio on UWP (Tiramisu-only, Path B step 6)

FNA's audio (`SoundEffect`, `Song`, 3D audio) goes through FAudio, FNA's
XAudio2 reimplementation. On Linux the host links the vendored FAudio
built as `libFAudio.so.0` (SDL2 backend). On the console there is no SDL
and no `FAudio.dll` to load, so FAudio is compiled **into the UWP host**
and FNA's P/Invokes resolve against the exe itself (`FAudio` →
`__Internal` dllmap in `src/trt/mono_host.cpp`; `FAUDIOAPI` is
`__declspec(dllexport)` under `_WIN32`, same mechanism as FNA3D).

## Why XAudio2, not FAudio's own Windows backend

FAudio ships three platform backends: SDL2, SDL3, and Win32. There is no
XAudio2 backend in FAudio — FAudio *is* the XAudio2 reimplementation.
Its Win32 backend renders through WASAPI via `IMMDeviceEnumerator` /
`IMMDevice` — but those interfaces are declared only under
`WINAPI_PARTITION_DESKTOP | GAMES` (`mmdeviceapi.h` lines 256–907 in SDK
10.0.26100), so `FAudio_platform_win32.c` cannot compile under the host's
`WINAPI_FAMILY_APP` UWP profile. (`IAudioClient` itself *is*
app-partition, but activating one needs `IMMDevice` or
`ActivateAudioInterfaceAsync` + a WinRT device ID.)

XAudio2 2.8 is the right output instead:

- The whole `xaudio2.h` header is app-partition (`APP | TV_APP |
  TV_TITLE | GAMES`, line 23), with a real non-desktop `XAudio2Create`
  export (line 1315).
- `xaudio2_8.lib` is already linked and `src/host/audio.cpp` already runs
  a proven XAudio2 2.8 mixer on this exact UWP build.
- XAudio2 2.8 dropped device enumeration (`GetDeviceCount` /
  `GetDeviceDetails` were removed in 2.8); the mastering voice always
  targets the default device. So this layer reports exactly one device.

## Files

- `FAudio_platform_uwp.c` — the UWP platform layer, implementing FAudio's
  platform interface (`FAudio_Platform*`, `FAudio_Log`, `FAudio_fopen` /
  `memopen` / `memptr` / `close`, `XNA_Song*`) on XAudio2 + Media
  Foundation + stdio. Compiled only by `tools/build-host.sh`
  (`-DFAUDIO_WIN32_PLATFORM -DFAUDIO_UWP_PLATFORM
  -DFAUDIO_DISABLE_DEBUGCONFIGURATION`); the vendored
  `FAudio_platform_win32.c` is never compiled for UWP.
- `faudio_uwp_compat.h` — force-included declaration of `lstrcpynA`
  (desktop-only in the SDK; `FACT.c` needs it via `FAudio_strlcpy`). The
  definition is a real implementation in the platform file.

The FAudio core sources (`FAudio.c`, `FACT*.c`, `FAPO*.c`, `F3DAudio.c`,
…) compile unmodified from `third_party/faudio-src/`. The vendored
`FAudio_platform_win32_wmadec.c` is compiled too, *without*
`HAVE_WMADEC`, for its upstream fallback `decode_wma` (logs + silence);
`XNA_Song.c` is deliberately *not* compiled — the UWP file provides the
MF song functions, and compiling both would duplicate the symbols.

## What is real here

- **Render**: shared refcounted `IXAudio2` engine; one mastering voice
  (device-native format) + one float source voice (FAudio mix format) per
  FAudio device; a render thread mixes FAudio's 10 ms (or 1024-sample)
  quantum via `FAudio_INTERNAL_UpdateEngine` into three rotating buffers
  and keeps two submitted. `updateSize` is assigned before the first mix
  (upstream's win32 backend assigns it after thread creation).
- **Enumeration**: `GetDeviceCount` returns 1 (the default device).
  `GetDeviceDetails(0)` reads the real format from a temporary mastering
  voice's `GetVoiceDetails` (channels + sample rate), builds a real
  channel mask, and reports role `FAudioGlobalDefaultDevice` (includes
  the Game bit FNA looks for). FNA takes its mastering-voice format from
  here, so this is on the critical path of every FNA audio init.
- **Song**: real Media Foundation Source Reader playback (`XNA_SongInit` /
  `XNA_PlaySong` / pause / resume / stop / volume / ended), adapted from
  upstream's win32 backend. MF is app-partition; the one desktop-only
  piece (`PropVariantToInt64`, `propvarutil.h`) is replaced by reading
  the `VT_UI8` duration straight from the `PROPVARIANT` union.
- **File I/O**: `FAudio_fopen` / `memopen` / `memptr` / `close` on stdio
  (`fopen_s`), as upstream's win32 backend. Used by `FACT.c` (XACT).
- **Failure modes degrade**: if `XAudio2Create` fails, the count is 0
  and `PlatformInit` returns a NULL device — FNA logs the failure and
  bails out of audio init; the game keeps running without XNA audio.

## What is honestly absent

- **WMA decode**: `wmadmod.dll` does not exist on Xbox. `HAVE_WMADEC`
  is off, as in the Linux build; WMA voices hit upstream's fallback
  (`decode_wma` logs "WMA IS NOT SUPPORTED IN THIS BUILD!" and outputs
  silence). Celeste's audio is FMOD/OGG, not WMA.
- **DeviceID**: `GetDeviceDetails` leaves `DeviceID` empty — no
  device-ID string is obtainable from the APP partition without WinRT
  device enumeration, and FNA does not use `DeviceID`.
- **Thread naming**: skipped (upstream resolves `SetThreadDescription`
  via `LoadLibrary` + `GetProcAddress`, a desktop-only idiom; the name is
  debugger-cosmetic only).
- **Visualization**: `XNA_EnableVisualization` /
  `XNA_GetSongVisualizationData` are upstream TODO no-ops, carried as-is.
- **Thread priority**: no-op, as in upstream's win32 backend (FIXME).
- **Device indices other than 0**: invalid (`FAUDIO_E_INVALID_CALL` /
  NULL device) — XAudio2 2.8 exposes only the default device.

## Console-verification-pending

Audible FNA audio output, the MF DLLs' presence in the Dev Mode
AppContainer, and the device format values can only be verified on the
console (no audio device under Xvfb). What is verified offline:
`tools/build-host.sh` compiles the layer under `WINAPI_FAMILY_APP`,
links it, exports FAudio's full entry-point surface from the exe, and
the `FAudio` → `__Internal` dllmap string is present in `mono_host.o`
(see the §2e check in `build-host.sh`). Do not claim FNA audio plays on
the console until a console test proves it.
