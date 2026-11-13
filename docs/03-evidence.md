# Tiramisu UWP — Evidence log

What is proven, by what, and what is only assumed. Never upgrade a claim
without an artifact.

## Proven OFFLINE (this repo, reproducible)

| Claim | Artifact |
|---|---|
| PE64 loader maps a real clang-built guest at preferred base | `tools/build-test.sh` case 1, ALL PASS |
| Relocation pass is semantically correct (not just non-crashing) | case 2: delta != 0, 2 DIR64 fixups, guest reads pointer table, checksum identical across bases |
| Import binding works through the module table | "import tiramisu_host.dll bound=2" + import call reaches host fns |
| W^X seal flips code to RX, no RWX ever | "SEALED" log + mprotect return codes |
| Malformed PE fails named, no crash | corrupt-MZ case: TRT_ERR_BAD_DOS, no image leaked |
| Win64 ABI execution of mapped guest code | entry returns 0x6494cb0d matching host-side model |
| Input bridge: key/mouse/pad feed -> user32 getters + pump | `tests/test_input.cpp` ALL PASS (17 checks) |
| First absolute mouse event positions without a false delta | mouse_init flag (found by the test) |
| Host is a proper UWP image | objdump: Subsystem Windows GUI, DllCharacteristics 0x9160 (APPCONTAINER 0x1000 set) |
| Package integrity | osslsigncode verify OK (trust anchor); makemsix round-trip unpack; manifest + guest present in packed msix |
| api-set contract imports resolve to the DLL that implements them | `tests/test_real_load.cpp`: "apiset api-ms-win-core-synch-l1-2-0.dll -> kernel32.dll", call reaches the real function |
| A missing optional DLL does not kill the load | delay import d3dcompiler_47 bound to a fail-visible stub; load continues; the call site names itself |
| A game can load the DLLs it ships | `tests/test_libload.cpp`: guestdll.dll loaded on demand from the game's directory, its own imports bound, DllMain ran with the right arguments, its export called across the image boundary, LoadLibraryW + GetProcAddress work |
| Guest-call ABI hazards are closed | the loader's Win64 call helper reads its arguments through memory constraints and before touching %rsp (a register-constrained version silently corrupted the guest's arguments; the test caught it) |
| The guest's D3D11 frame reaches the host's GPU | `tests/test_d3d.cpp`: the guest's `D3D11CreateDevice` gets the host's device+context, the factory enumerates adapters through the console's real DXGI factory, `CreateSwapChainForHwnd` hands over the HOST's swapchain, and the guest's clear/draw/present land on the host's objects |
| The engine-API batch works | `tests/test_engine.cpp`: the guest maps a file it wrote (the mapping carries the file's real bytes), starts a real worker thread and joins it through `WaitForSingleObject`, converts strings both ways, reads the OS version and system info, and drives the quit/focus lifecycle |
| DirectInput 8 works | `tests/test_dinput.cpp`: factory, mouse + keyboard devices, acquire, poll; the polled state carries deltas and buttons the harness fed through the real input ring |
| XAudio2 works | `tests/test_audio.cpp`: engine, mastering + source voices, PCM submit; the host sink received the guest's 48kHz stereo 16-bit buffer and `GetState`/`Flush` track the queue |
| Steamworks auth works | `tests/test_steam.cpp`: the boot lifecycle succeeds for a non-DRM'd build; ownership checks answer 0 *visibly* (no license server, no DRM bypass) |
| The guest has a working registry | `tests/test_reg.cpp`: HKCU/HKLM keys open and create, a string value round-trips, a missing value fails with ERROR_FILE_NOT_FOUND (not a silent zero), subkeys enumerate, and the hive is persisted to the guest's C: drive in a form that survives UTF-16 |
| winmm timing works (the frame clock) | `tests/test_winmm.cpp`: timeBeginPeriod accepted, timeGetTime nondecreasing AND advancing during a busy-spin, waveOutGetNumDevs answers 0, TIMECAPS sane, MCI fails visibly with 263 |
| The launcher has a real setup UI | `src/host/ui.cpp` `ui_set_setup` + `main.cpp` `boot_all`: live checklist (storage → D3D11 → gamepads → audio → transfer → shelf → self-test), A opens the shelf |
| The launcher scans compatibility before launch | `main.cpp` `scan_compat`: reads each game's PE machine type + full import table (RVA→file-offset through the section table), flags [steam-auth]/[d3d9 OK]/[d3d11 OK] on the shelf |
| The console has real audio output | `src/host/audio.cpp`: XAudio2 2.8 mastering + per-format source voices, 8-slot PCM ring wired to `trt_audio_set_hooks`; offline side proven by `tests/test_audio.cpp` sink receipt |
| The D3D9 bridge works end-to-end | `tests/test_d3d9.cpp`: the guest creates a D3D9 device through `Direct3DCreate9`/`CreateDevice` (SDK-exact 17/119-slot vtables), locks a texture and vertex buffer and writes through the locked pointers, sets render state/transforms/FVF, and the host hooks observe Clear, Present, both draws (TRIANGLELIST, FVF 0x42, viewport 1280x720) — the guest returned the full 0x3ff bitmask |
| DirectSound reaches the console sink | `tests/test_dsound.cpp`: the guest creates a DS8 device, sets the cooperative level, creates a secondary buffer, locks and writes PCM, plays, stops — the host sink observed the guest's 44.1kHz stereo 16-bit bytes (guest 0x3ff) |
| DirectDraw reaches the backend present | `tests/test_ddraw.cpp`: the guest creates an IDirectDraw7, sets display mode, creates the primary surface, locks and writes a gradient, creates an offscreen surface, BltFast copies it onto the primary, Flips — the host present hook fired twice (Unlock + Flip) (guest 0xfff) |
| D3DX9 math is exact and textures upload | `tests/test_d3dx9.cpp`: MatrixIdentity/Multiply/PerspectiveFovLH/Inverse/Transpose verified against hand-computed values, Vec3 normalize/cross/dot exact, a 32-bit BMP in memory became a texture through the d3d9 hooks (2 create_tex + 1 lock observed), shader source round-tripped through a real ID3DXBuffer, sprite Begin/End (guest 0x3ff) |
| Codec/printer boot probes answer the no-hardware shape | `tests/test_mmedia.cpp`: ACM PCM->PCM streams open and convert, ADPCM fails ACMERR_NOTPOSSIBLE (engine falls back to PCM), ICInfo/ICOpen/DrawDibOpen report no codecs (AVI logos skipped), EnumPrinters/OpenPrinter report no printers (guest 0x1ff) |
| PNG + DDS parse to RGBA8 with named failures | `tests/test_img.cpp`: a hand-built 2x2 RGBA PNG and a gray PNG inflate (miniz) + unfilter to exact pixels, truncated/bad-signature fail named; DDS X8R8G8B8 with bitmask extraction decodes exact colors; a DXT5 4x4 block decodes to opaque red (alpha indices 001, color idx 0); GIF fails named |
| D3DX10/11 drives the guest device's real vtable | `tests/test_d3dx.cpp`: a fake ID3D11Device records CreateTexture2D (slot 5) + CreateSRV (slot 7) — a PNG in memory becomes a texture with exact desc (2x1, R8G8B8A8=28, IMMUTABLE, SHADER_RESOURCE, pitch 8) and exact pixel bytes snapshotted inside the call; SRV desc is TEXTURE2D (4); image-info pre-scan fills width/height/format/resource-type; DDS DXT5 and the file-path variants work; missing file = 0x80070002; d3dx10 aliases route identically; JPG fails named |
| stb_image fallback (public domain, vendored) | `tests/test_img.cpp` still ALL PASS with stb wired as the fallback after the hand PNG/DDS parsers — JPG/GIF/PSD/PIC/PNM and exotic PNG/TGA/BMP variants now parse instead of failing named; the truncated-GIF named-failure check stays green |
| COM boot + OLEAUT through the IAT | `tests/test_ole.cpp`: a freestanding guest runs the boot sequence a COM-heavy engine runs (guest 0x3ff) — CoInitializeEx S_OK then S_FALSE on nesting, CoTaskMemAlloc/Free round-trip, CoCreateInstance with an unregistered CLSID returns REGDB_E_CLASSNOTREG with the GUID logged (the console log discovers which COM classes games need), BSTR alloc/len/free round-trip, VariantInit/Clear, SafeArrayCreateVector + GetUBound/PutElement/GetElement exact + Destroy, OleInitialize, triple CoUninitialize unwind without crash |
| OpenAL streaming loop lands PCM on the sink | `tests/test_openal.cpp`: a freestanding guest runs the Minecraft-class loop through the IAT (guest 0xfff) — alcOpenDevice, context, 4 buffers filled with 44100-byte PCM (rate 22050, mono 16-bit), source, queue 4, play (AL_PLAYING), AL_BUFFERS_QUEUED = 4, pump drains all (AL_BUFFERS_PROCESSED >= 4), unqueue retires 4, queue back to 0, stop, close; the shared audio sink observed >= 4 submissions with exact bytes/rate/channels/bps preserved. Caught+fixed: OpenAL is a C API — the al_* entry points have NO leading context parameter (a COM-style self arg segfaults the guest); log_line must live OUTSIDE extern "C" or it collides across bridge .cpp files |
| D3D8 lifecycle on the shared backend hooks | `tests/test_d3d8.cpp`: a clang-built freestanding guest runs the full D3D8 lifecycle through the IAT (guest 0xfff) — Direct3DCreate8, CreateDevice (slot 15), texture LockRect (tex slots 16/17 of 19), VB Lock (slot 11 of 14), render state/transform, FVF via a DWORD shader handle (slot 76 < 0x10000), BeginScene/EndScene/Clear/DrawPrimitiveUP/Present, index buffer + SetIndices base-vertex (slot 85) + DrawIndexedPrimitive D3D8 signature (slot 71, no base-vertex arg), D3DCAPS8 layout (MaxTextureWidth at 0x54, PixelShaderVersion at 0xEC), CreateVertexShader returns a DWORD handle, CopyRects; host hooks observed: 1 tex, 2 bufs, clear/present/RS marshalled, base vertex = 16, viewport 1280x720 |
| COM boot + OLEAUT surface (ole32/oleaut32 BUILTIN) | `tests/test_ole.cpp`: a freestanding guest imports ole32+oleaut32 through the IAT (guest maps, W^X-sealed) — CoInitializeEx MTA S_OK, nesting S_FALSE, CoUninitialize unwinds the count, CoTaskMemAlloc/Free round-trip, CoCreateInstance with an unregistered CLSID returns REGDB_E_CLASSNOTREG with the GUID in the log (fail-visible), SysAllocString/SysStringLen/SysFreeString round-trip through the real BSTR header (length-in-bytes at ptr-4), VariantInit→VT_EMPTY + VariantClear, SafeArrayCreateVector/Lock/AccessData/Put/Get/UBound/Destroy round-trip, OleInitialize answers S_FALSE when already initialized |

## Proven on CONSOLE by the reference projects (we inherit the envelope, not the code)

- A UWP `IFrameworkView` app with `TargetDeviceFamily=Windows.Universal`
  (Min 19041 / Max tested 22621) installs and renders D3D11 on Series Dev Mode
  (MinecraftLCE source-audited; xboxnative d3tri: 300 presents, hr=0).
- In-process PE mapping with W^X (`VirtualAllocFromApp`/`VirtualProtectFromApp`)
  executes donor PE code on console (xboxnative donor lane: hid/setupapi/
  version/shcore/avrt initialized + focused behavior checks).
- `CreateProcess` of arbitrary children is blocked in the sandbox
  (xboxnative hit the same wall; Microsoft Q&A states it).
- 32-bit native compat mode is DEAD on Series S console: `ljmp 0x23` ->
  0xc0000005, CS stays 0x33, packaged WoW64 DLLs error 126
  (xboxnative cs32-console-evidence.log).
- Console memory: 5 GB as a Game package, 1 GB as App ("Treat UWP apps as
  games" toggle in Dev Home).
- No ETW on console: LocalState log files + crash dumps are the only diagnosis
  (Device Portal /api/etw/* 404).

## Assumed until the console run (M0 acceptance)

- AppContainer alloc behavior under `VirtualAllocFromApp` matches the test
  backend's semantics (fixed bases unavailable — loader handles via relocs).
- XInput poll works in this app shape (xinput.lib -> XINPUTUAP.dll).
- `LoadLibraryW`+`GetProcAddress` resolves the SYSTEM-route DLLs in-package.
- The guest's return value and log lines land in `tiramisu.log` pullable via
  Device Portal.

## Known name history

Project renamed mid-build: Longan -> **Tiramisu** (Longan collided with an
existing small emulator/project namespace; user's call). Code identifiers
`trt` (Tiramisu runtime) and `TRT_ERR_*` are the durable prefixes.

## Sources

- xboxnative (jobucaldas) README + research/WINE-BASE-REPORT.md (Wine 11
  source closure analysis; cs32 console evidence; donor execution evidence)
- MinecraftLCE-Xbox (manifest shape + gotcha table, source-audited)
- skill: xbox-devmode-uwp (toolchain recipe, manifest envelope, probe set)
- Microsoft Learn: UWP-on-Xbox limitations, FullTrustProcessLauncher PC-only,
  D3D9->D3D11 feature mapping
