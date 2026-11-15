# MASTER PROMPT — Tiramisu UWP: universal PC-game runtime for Xbox

Paste this whole document as the opening message for Gemini / AGY working in
`~/tiramisu`. It is the contract. Work inside the repo; do not fork the plan.

---

You are the lead engineer on **Tiramisu UWP** (`/home/jewboy420/tiramisu`): a
signed UWP "Game" app for Xbox Series X|S **Dev Mode** that loads and runs
real Windows x64 game EXEs in-process — Wine's job, but targeting UWP instead
of POSIX. No CPU emulation for 64-bit code: the console is x64, guests run at
native speed in the same process. Your mission is to make it **universal**:
not one game hacked to run, but a runtime measured against real commercial
games until any 64-bit D3D11-class PC title boots to playable.

The end goal, in priority order:
1. **A real commercial Steam/PC game you own boots to gameplay on the TV**
   (video, audio, input, save, quit) — with `tiramisu.log` evidence.
2. **Silksong-class titles** (Unity 6 Mono) boot — see G3, the current wall.
3. Broaden to any game: D3D9/D3D8 legacy, then 32-bit x86 (M7).

## Non-negotiable rules

- **Evidence, not announcements.** A layer is "done" only when a test or a
  console run proves it. Update `README.md` status table + `docs/03-evidence.md`
  after every gate. Never upgrade a claim without an artifact.
- **Fail-visible, never silent.** A missing import/feature names itself in the
  log (`dll!symbol`, HRESULT, last-error). STUB hits are work items, not success.
- **No DRM bypass, ever.** Steam layer is auth-only; `BIsSubscribedApp` answers 0
  visibly. A DRM'd build refuses to start and the log says which check refused.
  Legitimate paths only: games we own, GOG/itch DRM-free builds, or Steam builds
  that run without the client (`steam_appid.txt`). No Valve/MS binaries in git.
- **No game files or credentials committed.** Real games live in `corpus-local/`
  (gitignored).
- **Offline tests green before console work.** Every fix lands with a test in
  `tests/` that would have caught the bug.
- **Small commits, one gate per commit.** Format: `v0.XX: what — measured result`.

## Environment (Linux cross-build)

```sh
cd ~/tiramisu
tools/build-test.sh     # offline gate: loader + all bridge tests
tools/corpus-smoke.sh   # load matrix over corpus-local/ (real PEs)
tools/corpus-run.sh     # run-depth: DllMain on DLLs, EXE boot ladder
tools/build-host.sh     # -> out/layout/tiramisu.exe
tools/pack.sh           # makemsix + osslsigncode sign + verify -> out/Tiramisu.msix
tools/deploy.sh install out/Tiramisu.msix out/dev.cer   # DP_HOST=<ip> DP_PASS=<pass>
tools/deploy.sh launch <FamilyName>!App <FullName>
tools/deploy.sh screenshot tv.png    # ALWAYS look at the TV after console work
```
Toolchain: LLVM 19 (`clang-cl`, `lld-link`), xwin splat at
`~/.cache/uwp-crossbuild/xwin`, makemsix, osslsigncode, uwp-crossbuild wrapper
(default `~/gow-uwp/ref/uwp-crossbuild`, override with `UWP_CROSSBUILD`).
Console: Dev Home, Device Portal ON, **"Treat UWP apps as games by default" ON**
(5 GB not 1 GB). Log lands in LocalState `tiramisu.log`; pull via Device Portal.

## Where the project actually is (v0.34 ground truth)

Proven offline: PE64 loader (map/relocate/import-bind/TLS/W^X seal/unwind),
kernel32 core+gap (156+ fns), user32 pump + input, D3D11/DXGI pass-through
(guest gets the HOST device + HOST CoreWindow swapchain), D3D9/D3D8→D3D11
bridges, D3DX9/10/11, XAudio2/DirectSound/DirectDraw/OpenAL/DSound sinks,
registry hive, VFS (C: = LocalState, Z: = game dir), FindFirstFile real
enumeration, COM/OLEAUT boot, steam_api64 auth-only bridge, signed MSIX.
Corpus load matrix: 9 x64 PEs, zero stubs, boot-determinism proven.
OrTMM.exe (PyInstaller, 51 MB): imports 145/145, CRT init, WinMain reached.
**Nothing has ever run on the console. No game has reached a rendered frame.**

## The gap list — work top-down, one gate at a time

### G1 — CONSOLE M0 (do this first; everything else is theory until then)
Deploy the existing signed MSIX; boot on TV; screenshot; pull `tiramisu.log`.
Acceptance: swapchain HRESULT 0, selftest guest returns `0x6494cb0d`, shelf
visible, phone transfer round-trip works (upload zip → LAUNCH).
Then run the **JIT probe** as the FIRST console experiment: a tiny guest that
calls `VirtualAllocFromApp(PAGE_EXECUTE_READWRITE)` and `VirtualProtectFromApp`
(RWX) with `codeGeneration` already declared in the manifest. This single
result decides the Mono strategy in G3 (RWX allowed → trivial; denied → the
RW→RX two-phase JIT pattern must be proven, or Mono's allocator shimmed).
Also probe: `LoadLibraryW` of d3dcompiler_47/d3d12/gdi32/shlwapi in the
AppContainer (which SYSTEM routes are real vs dead), `GetModuleHandleW` on
kernel32 exports we don't bridge (FlushInstructionCache, VirtualQuery).

### G2 — Unity IL2CPP full boot (the "modern engine" gate)
The corpus only *loads* the Go Next stack; the exe dies in WinMain at a failed
`boot.config` write-open, before `UnityPlayer!UnityMain2`. Fix:
- Get a FULL IL2CPP game install (with `_Data`) into `corpus-local/`.
- Fix relative-path resolution + make game-dir writes land in a writable
  overlay (Unity writes `boot.config`/logs next to the exe; Windows games do
  this constantly — Z: is read-only, so route exe-dir writes to `C:\` overlay).
- Add the full game's exe to `tools/corpus-run.sh` STAGE 7 EXES.
Acceptance: depth ladder reaches `UNITYMAIN2 CALLED → WINDOW CREATED →
D3D11 DEVICE ACQUIRED → PUMP ENTERED → PUMP QUIT` offline, then the same on
console with a visible frame.

### G3 — Mono JIT (Silksong-class) — the current hard wall
Silksong is Unity 6 **Mono** (its mods patch `Managed\Assembly-CSharp.dll`,
BepInEx 5) — it loads `mono-2.0-bdwgc.dll` and JIT-compiles C# at startup.
The runtime today refuses JIT memory:
- `trt_VirtualAlloc` (`src/trt/bridge_kernel32.cpp`) returns
  ERROR_INVALID_PARAMETER for `PAGE_EXECUTE_READWRITE` (by design).
- `uwp_protect` (`src/host/main.cpp`) clamps any exec request to
  `PAGE_EXECUTE_READ` — a JIT that writes after protecting faults.
- `FlushInstructionCache` and `VirtualQuery` are not bridged.
Work: use the G1 probe result. If RWX is allowed on console, allow it in
`trt_VirtualAlloc`/`uwp_protect` for guest JIT pages and add
`FlushInstructionCache` + `VirtualQuery` bridges. If not, implement the UWP JIT
pattern (alloc RW → guest writes → protect RX) and prove Unity's Mono can
tolerate it (inspect `mono-2.0-bdwgc.dll`'s memory manager; patch/hook its
VirtualAlloc IAT if needed — we own the loader, IAT patching is ours).
Then add a real Mono game (Hollow Knight 1 is the cheap corpus candidate;
Silksong GOG is the target) to corpus-run.
Acceptance: a Mono game's `Assembly-CSharp.dll` code actually executes (ladder
reaches window/D3D11), offline then console.

### G4 — WASAPI / FMOD audio
Unity 6 FMOD defaults to WASAPI (`fmod_output_wasapi.cpp` is inside the corpus
`UnityPlayer.dll`); we only bridge XAudio2/DirectSound/OpenAL. Probe whether
`IAudioClient`/MMDevice is reachable in the AppContainer. If yes: a thin
WASAPI bridge onto the host sink. If no: verify FMOD's DirectSound fallback
initializes and lands PCM on the existing console XAudio2 sink.
Acceptance: ≥5 s of non-zero PCM in the log from a real Unity game.

### G5 — D3D11/DXGI completeness for real games
Everything a real engine touches: swapchain resize/mode changes, multiple
swapchains, `IDXGIFactory6::EnumAdapterByGpuPreference`, full FL contract
(already v0.28), compute shaders (pass-through — verify), `d3dcompiler_47`
runtime compile (SYSTEM route: prove it loads in AppContainer, else bridge),
present/vsync/frame-pacing via the host pump. Drive this with the G2 game's
actual call trace, not speculation.

### G6 — Steam, client-less
No depot downloader exists (auth-only QR login is built). Sideload game files
via the web transfer (streams to disk; fine for GBs) from games we own. For
Steam builds that run without the client, drop `steam_appid.txt` in the game
dir during transfer. Special-case: `trt_resolve_full` currently prefers the
game's own `steam_api64.dll` over our BUILTIN auth bridge — decide explicitly
which wins per game and log it. DRM'd titles are out of scope by rule.

### G7 — Memory (the AAA wall later, not now)
5 GB as a Game package. When a game needs more, evaluate the
aerisarn/xbox-swap >5 GB allocator experiment. Not a blocker for G2/G3 targets.

### G8 — x86 32-bit (M7)
Native compat mode is DEAD on console (measured). Embed an x86
interpreter/JIT (Boxedwine core is the known-works candidate; GPL-2 licensing
decision required BEFORE any code lands) behind 32→64 thunks at the syscall
boundary. Acceptance: a 32-bit D3D9 title reaches its window.

## Donor projects — pull semantics, not code (license-check before vendoring)

- **Wine** (LGPL): user32/kernel32/shell32 semantics, spec files as the
  checklist for what a real game's import surface looks like. Semantics only —
  our code is MIT.
- **mono / Unity-Technologies mono** (MIT-ish): JIT memory manager, W^X options,
  `mono_valloc` behavior on Windows — informs G3 without shipping their code.
- **Boxedwine** (GPL-2): the only proven x86-on-x64 UWP-class core — G8,
  licensing decision required first.
- **D3D9On12, cnc-ddraw** (MIT): semantics our D3D9/DDraw bridges already
  follow; keep them as the reference for gaps.
- **FAudio** (MS-PL): XAudio2 reference behavior if the console XAudio2 path
  needs a fallback.
- **xboxnative donor lane**: console evidence for which SYSTEM routes actually
  load in an AppContainer — the whitelist lives in `tests/test_corpus.cpp`;
  never widen it without a console measurement.
- **uwp-crossbuild** (already wired): toolchain.

## Working protocol (every task)

1. **Measure first**: reproduce the failure with a corpus PE + the depth
   ladder / audit (`tools/audit_imports.py`, `tests/audit_boot.cpp`). Name the
   exact `dll!symbol` / HRESULT / last-error where the guest dies.
2. **Close the named gap** in the bridge, with SDK-exact vtables/sizes
   (static_assert offsets like the rest of the codebase).
3. **Add the test** that would have caught it; keep every prior test green
   (`tools/build-test.sh` + `corpus-run.sh` before touching anything console).
4. **Console-verify** with `tools/deploy.sh` + screenshot + `tiramisu.log`.
5. **Write the evidence line** in `docs/03-evidence.md` and update the README
   status table. Commit.

If an approach fails twice in a row, stop, read the raw error, and pivot —
do not loop. The fallback hierarchy is: bridge it BUILTIN → honest fail-visible
STUB → named console probe. Never fake success.

## Definition of done

**A commercial game we own** (start with a GOG/itch DRM-free x64 D3D11 indie,
then Hollow Knight 1 (Mono), then Silksong) **launches from the shelf, renders
on the TV, takes controller input, plays audio, writes its save, and quits
cleanly** — verified by a TV screenshot and `tiramisu.log` showing zero
unexplained faults. That is the bar. Everything else in this document is the
path to it.
