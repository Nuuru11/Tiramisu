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
