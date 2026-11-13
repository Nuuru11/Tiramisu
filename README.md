# Tiramisu UWP

**A Windows PE64 runtime for Xbox Series X|S Dev Mode.** One signed UWP "Game"
app that loads Windows `.exe`s in-process — Wine's job, targeting UWP instead
of POSIX. No CPU emulation for 64-bit games: the console is x64, guests run at
native speed on the same core, same ABI, same process.

Named after the fruit-emulator tradition (yuzu, citra, papaya).

```
Tiramisu UWP host (CoreWindow + D3D11 flip + XInput, 5 GB as a Game package)
  └── trt — Tiramisu runtime
        ├── PE64 loader   map -> sections -> relocate -> bind imports -> TLS
        ├── module table  every import routed: SYSTEM | BUILTIN | STUB
        ├── bridges       kernel32 core implemented on UWP; user32 = skeleton
        └── VFS           C:\ -> LocalState\drive_c, Z:\ -> package (read-only)
```

## Status (honest)

| Layer | State |
|---|---|
| Offline loader test (Linux, real PE guest executed) | **PASS** — mapping, DIR64/HIGHLOW relocation, import binding, W^X seal, entry execution, checksum-verified |
| Offline input-bridge test | **PASS** — keyboard/mouse/pad feed -> user32 getters + message pump |
| Offline real-game load-path test | **PASS** — api-set contracts redirected, delay imports bound, unresolved symbols fail *visibly* |
| Offline guest-DLL test | **PASS** — a DLL the game ships is loaded on demand, its imports bound, DllMain runs, its export is called across the image boundary, LoadLibrary/GetProcAddress work |
| UWP host (CoreWindow/D3D11/XInput/LocalState log) | **BUILDS** — signed `Tiramisu.msix` produced and verified |
| Phone transfer (QR on TV -> upload zip -> launch) | **BUILT** — ws2_32 HTTP server, miniz zip extraction, launch handoff; console verdict pending |
| Keyboard + mouse | **BUILT** — CoreWindow events -> trt_input -> GetAsyncKeyState/GetKeyState/GetCursorPos + WM_ queue; gamepad-as-mouse mode (RS toggle, triggers = buttons) |
| Controller | **BUILT** — XInput mirrored into trt_input + D-pad shelf navigation |
| Steam | **BUILT (auth only)** — QR login via IAuthenticationService (own account), token saved to LocalState; depot download is M6 |
| Guest DLLs (the DLLs a game ships) | **BUILT** — loaded on demand from the game's own directory, imports bound recursively, DllMain run, exports resolvable by name and ordinal |
| API-set contracts (`api-ms-win-*`) | **BUILT** — redirected to the DLL that implements them (synch->kernel32, winrt/com->combase, crt->ucrtbase, d3d11/dxgi/gdi/user/ole...) |
| Delay imports | **BUILT** — bound eagerly; a missing optional DLL no longer kills the load, the call site names itself instead |
| Registry (advapi32) | **BUILT** — HKCR/HKCU/HKLM/HKU/HKCC key tree, persisted in `C:\registry.hiv` next to the game's saves; a missing value fails *visibly* (ERROR_FILE_NOT_FOUND), never as a silent zero |
| Memory-mapped files | **BUILT** — `CreateFileMapping`/`MapViewOfFile` serve the file's real bytes (engines stream their asset bundles this way); `VirtualProtect` too |
| Threads & sync | **BUILT** — real worker threads (`CreateThread`), events, mutexes, semaphores, critical sections, `WaitForSingleObject`/`WaitForMultipleObjects`, TLS |
| System information | **BUILT** — OS version, `GetSystemInfo`, tick counters, temp path, computer name, `MultiByteToWideChar`/`WideCharToMultiByte`, file/dir APIs |
| DirectInput 8 | **BUILT** — `DirectInput8Create`, factory + mouse/keyboard devices with the SDK's exact vtables, `Acquire`/`Poll`/`GetDeviceState` (DIMOUSESTATE deltas + 256-byte key map) fed from the real input ring |
| Steamworks (auth-only) | **BUILT** — the boot lifecycle (`RestartAppIfNecessary` -> `Init` -> `GetSteamID` -> presence -> callbacks). **No DRM bypass, ever**: `BIsSubscribedApp` answers 0 with a log line, so a DRM'd build refuses to start visibly; use GOG/itch builds |
| XAudio2 audio | **BUILT** — engine, mastering/source voices with the SDK's exact vtables, `SubmitSourceBuffer` hands the guest's PCM to the host sink, `GetState`/`Flush`/`Stop` |
| Quit + focus lifecycle | **BUILT** — `PostQuitMessage`, `SetFocus`/`GetFocus`, `GetClientRect`, `SetTimer`/`KillTimer` (a game that cannot quit or pause is broken) |
| D3D11/DXGI for the guest | **BUILT** — the guest's `D3D11CreateDevice` gets the host's device+context, its factory enumerates adapters through the console's real DXGI, and `CreateSwapChainForHwnd` hands it the HOST's CoreWindow swapchain (the console has no window for the guest's fake HWND) |
| Guest DLLs loading the game's own DLLs | **BUILT** — loaded on demand from the game's own directory, imports bound recursively, DllMain run, exports resolvable by name and ordinal |
| D3D9 -> D3D11 bridge (M5) | **BUILT** — `Direct3DCreate9`/`CreateDevice` with the SDK's exact 17/119-slot vtables; texture/VB/IB lifecycle, LockRect/Lock, render state, transforms, FVF, Clear/BeginScene/EndScene, all four draw paths, Present; D3DRS_* pass through to the host backend (D3D9On12 semantics, MIT); host binds plain-C hooks |
| DirectSound (M6) | **BUILT** — `dsound.dll` BUILTIN: SDK-exact vtables (DS8=12, Buffer8=22 slots); Lock/write/Unlock marshals the guest's PCM into the same console XAudio2 sink XAudio2 uses |
| DirectDraw (M6) | **BUILT** — `ddraw.dll` BUILTIN: SDK-exact vtables (DD7=30, Surface7=49 slots); primary-surface Unlock/Flip maps to the host backend present (cnc-ddraw's mapping onto D3D11); offscreen surfaces + Blt/BltFast |
| D3DX9 (M6) | **BUILT** — `d3dx9_30..43.dll` BUILTIN: exact D3DXMatrix/Vec/Quaternion math, texture creation from BMP/TGA/PNG/DDS in memory (PNG inflate + DDS BC1/2/3 decode via vendored miniz, M7), `D3DXAssembleShader` with a real ID3DXBuffer, ID3DXSprite (14-slot SDK order) with real quad batching through the draw path (M7) |
| Image parsers (M7) | **BUILT** — PNG (8-bit gray/grayA/RGB/RGBA, non-interlaced, miniz inflate + unfilter) and DDS (uncompressed with bitmasks + DXT1/3/5 block decode) -> RGBA8 for the texture path; everything else fails named |
| D3DX10/11 (M7) | **BUILT** — `d3dx10_33..43`/`d3dx11_43/47` BUILTIN: texture + SRV from memory/file through the guest device's REAL vtable (CreateTexture2D slot 5, CreateSRV slot 7), image-info pre-scan, BMP/TGA/PNG/DDS containers; d3dx10 aliases d3dx11 |
| D3D8 -> D3D11 bridge (M7) | **BUILT** — `d3d8.dll` BUILTIN: the 2001–2004 generation (Morrowind, BF1942, NWN, Max Payne) on the SAME backend hooks as D3D9; SDK-exact vtables (D3D8=16, Device8=97, Texture8=19, VB/IB8=14, Surface8=11 slots); D3D8 signatures where they differ (DWORD shader handles, `DrawIndexedPrimitive` without base-vertex — it rides in `SetIndices`, `SetStreamSource` without offset, `CopyRects`/`CreateImageSurface`); D3DCAPS8 layout for feature gating |
| COM boot + OLEAUT (M7) | **BUILT** — `ole32.dll`/`oleaut32.dll` BUILTIN: `CoInitialize`/`CoInitializeEx` (STA/MTA, nesting -> S_FALSE), `CoCreateInstance`/`CoGetClassObject` dispatch to registered CLSID personalities with an unregistered CLSID failing REGDB_E_CLASSNOTREG *with the GUID in the log* (that line names which COM class the game needs next), CoTaskMem*, BSTR (SysAllocString/Free/ReAlloc/Len/ByteLen with the real length header), VariantInit/Clear/Copy, 1D SafeArrays (create/lock/access/get/put/bounds/destroy), `OleInitialize`/`OleUninitialize`, PropVariantInit |
| ole32/oleaut32 (M7) | **BUILT** — minimal COM boot: CoInitializeEx/Uninitialize (nesting), CoCreateInstance dispatching to registered CLSID personalities (unregistered = REGDB_E_CLASSNOTREG with the GUID in the log — fail-visible, and the console log discovers what games actually need), CoTaskMem*, OleInitialize; OLEAUT: SysAllocString/FreeString/StringLen (real BSTR layout), Variant head, 1D SafeArrays — no more SYSTEM route for the pair |
| OpenAL 1.1 (M7) | **BUILT** — `openal32.dll` BUILTIN: the Minecraft-generation audio API (alcOpenDevice/context, buffers with real PCM, sources, queue/play/stop, AL_BUFFERS_PROCESSED polling, unqueue-refill streaming) onto the SAME console XAudio2 sink DirectSound and XAudio2 drive; a deterministic manual pump (`trt_openal_pump`) instead of a background thread |
| stb_image (M7) | **BUILT** — vendored public-domain stb_image as the image-fallback loader: JPG/GIF/PSD/PIC/PNM plus every exotic PNG/TGA/BMP variant the hand parsers reject, forced to RGBA; DDS stays hand-rolled (block formats) |
| Small-gap probes (M6) | **BUILT** — `msacm32` (PCM identity streams; unknown codecs fail visibly so engines use PCM assets), `msvfw32` (no AVI codecs — engines skip logo videos), `winspool.drv` (no printers) |
| winmm (the frame clock) | **BUILT** — `timeGetTime`/`timeBeginPeriod`/`timeGetDevCaps`; legacy waveOut/midi/joy probes answer "no devices"; MCI fails visibly (263) |
| Console audio output | **BUILT** — `src/host/audio.cpp`: real XAudio2 2.8 mastering + per-format source voices on console, 8-slot PCM ring, wired to the guest bridge via `trt_audio_set_hooks` |
| Launcher setup UI | **BUILT** — first-run live checklist (storage → D3D11 → gamepads → audio → transfer server → shelf scan → self-test); A opens the shelf |
| Pre-boot compat scanner | **BUILT** — the shelf reads each game's PE (machine + import table) and flags `[steam-auth]` / `[d3d9 OK]` / `[d3dx9 OK]` / `[dsound OK]` / `[ddraw OK]` / `[d3d11 OK]` BEFORE you launch |
| Audio (M3) | **DONE** — bridge (guest PCM) + console sink (real XAudio2 output) both built |
| Console run | **PENDING** — `tools/deploy.sh install` then look at the TV |
| 32-bit | dead by console evidence (compat-mode `ljmp 0x23` = 0xc0000005 on Series S); needs an x86 engine later |

v0.2 ceiling while a game runs: the guest owns the host thread; its builtin
`GetMessageW` pump keeps input flowing, but the shelf UI freezes until the
guest exits. One game at a time.

## Build (Linux, all cross)

```sh
tools/build-host.sh   # offline loader test gate -> out/layout/tiramisu.exe
tools/pack.sh         # makemsix pack + osslsigncode sign + round-trip verify
```

Needs: LLVM 19 (`clang-cl`, `lld-link`), xwin splat at
`~/.cache/uwp-crossbuild/xwin`, `makemsix`, `osslsigncode`, and the
uwp-crossbuild wrapper (defaults to `~/gow-uwp/ref/uwp-crossbuild`; override
with `UWP_CROSSBUILD`).

## Deploy (console)

```sh
DP_HOST=<console-ip> DP_PASS=<portal-pass> tools/deploy.sh install out/Tiramisu.msix out/dev.cer
# then: tools/deploy.sh launch <FamilyName>!App <FullName>   # from ps output
# and:  tools/deploy.sh screenshot tv.png                    # LOOK at the TV
```

Dev Home prerequisites: Device Portal enabled, and **"Treat UWP apps as games
by default" ON** (5 GB not 1 GB — missing this fails everything silently).
The log lands in LocalState `tiramisu.log`; pull it via Device Portal.

## Rules of the road

Legitimate paths only: games you own, no DRM bypass, no Xbox security bypass.
No game files or credentials in the repo. MIT for our code; see
`docs/03-evidence.md` for what is proven vs assumed.
