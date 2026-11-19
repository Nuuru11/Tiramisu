<div align="center">

<img src="docs/banner.jpg" alt="Tiramisu — PC games on Xbox Series X|S" width="100%"/>

### Run PC games on Xbox Series X|S — natively.

**A Windows PE64 application runtime for Xbox Dev Mode.**
Wine's job, targeting UWP instead of POSIX.

![Status](https://img.shields.io/badge/status-console--proven-brightgreen)
![Proof](https://img.shields.io/badge/proof-RACCOIN%20playable%20%7C%20God%20of%20War%20forest-blue)
![Gates](https://img.shields.io/badge/offline%20gates-49%2F49%20pass-brightgreen)
![Platform](https://img.shields.io/badge/platform-Xbox%20Series%20X%20%7C%20S-107C10?logo=xbox&logoColor=white)
![Guests](https://img.shields.io/badge/guest-PE64%20%2B%20Unity%20%2B%20Godot%204%20%2B%20PyInstaller-blue)
![Audio](https://img.shields.io/badge/audio-XAudio2%20%2F%20FMOD%20%2F%20OpenAL%20%2F%20DirectSound-8A2BE2)
![License](https://img.shields.io/badge/license-MIT-green)
![Language](https://img.shields.io/badge/C%2B%2B-17%20%2F%20clang--cl-00599C?logo=cplusplus&logoColor=white)
![DRM](https://img.shields.io/badge/DRM%20bypass-none%2C%20ever-red)

[How it works](#-how-it-works) · [Proof](#-proof--console-run-2026-11-19) · [What this proves](#-what-this-proves) · [Status](#-status-honest) · [Build](#-build-linux-all-cross) · [Deploy](#-deploy-console) · [Evidence](docs/03-evidence.md) · [Gap map](GAP-MAP.md)

**[⬇ Download the release](https://github.com/xanstomper/tiramisu/releases)** · **[🎮 Compatibility](#-what-boots-today)**

</div>

---

## What is this?

One signed UWP app that loads Windows `.exe`s **in-process** on Xbox
Series X|S Dev Mode. No CPU emulation for 64-bit games: the console is x64,
so guests run at **native speed** on the same core, the same ABI, the same
process — Tiramisu is the operating system around them.

```text
Tiramisu UWP host (CoreWindow + D3D11 flip + XInput, 5 GB as a Game package)
  └── trt — Tiramisu runtime
        ├── PE64 loader      map → sections → relocate → bind imports → TLS → delay imports
        ├── Win64 EH         .pdata personality, RtlUnwindEx, funclets, VEH
        ├── module table     every import routed: SYSTEM | BUILTIN | STUB
        ├── bridges          25+ Win32 families implemented on UWP
        │   ├── graphics     D3D11/DXGI (real device passthrough), D3D9/8/DDraw→D3D11,
        │   │                D3DX9/10/11, OpenGL→D3D11 (gl4es-derived), Vulkan 1.0–1.3 ICD
        │   │                (SPIR-V→HLSL→D3D11), GDI32, GDI+ (launcher UI)
        │   ├── audio        XAudio2 / DirectSound / WASAPI / OpenAL / winmm / FMOD
        │   │                → one real XAudio2 2.8 mixer (48 kHz stereo master voice)
        │   ├── system       kernel32 core+mem+thread, ntdll, advapi32 registry, ole32/oleaut,
        │   │                shell32/shlwapi, comctl32, crypt32, bcrypt, wininet, psapi,
        │   │                iphlpapi, imm32, dbghelp, setupapi/HID, winrt (Gamepad…)
        │   └── steam        QR auth (your own account) — no DRM bypass, ever
        ├── VFS              C:\ → LocalState\drive_c, D:\ → mounted .iso, Z:\ → package
        ├── budget governor  one 5 GB ledger for VirtualAlloc commit + heap + mapped views
        └── async I/O        overlapped reads → GQCS packets, mmap fast path (TRT_IO_MMAP_MIN)
```

## 🏆 Proof — console run (2026-11-19)

This is the milestone the project was built for: **two commercial 64-bit
Windows games, running in-process on Xbox Dev Mode, through the signed
Tiramisu UWP host.** No CPU emulation, no streaming, no Linux PC in the loop.

| Game | Engine / class | Console result |
|---|---|---|
| **RACCOIN** | Unity 6 IL2CPP, Steam, D3D11 | **Fully playable** — main menu, a live run, coin physics, HUD, score/round progression |
| **God of War** (PC) | AAA in-house engine, x64, D3D11 | **Boots, runs, renders** — the opening forest scene, four independent boots, each one closer than the last |

The rest of this section is the detailed evidence: what each capture contains,
which runtime stage it closes, how the four God of War boots fit together, and
what the images still do not prove. The short version is below; the full
argument starts at [What this proves](#-what-this-proves).

### Milestone timeline

| Date | Milestone |
|---|---|
| **2026-11-15** | Offline RACCOIN reaches the full 7/7 boot ladder; the mock-render gate counts **2.7M device-context calls / ~268k DrawIndexed** in 10 s |
| **2026-11-17** | First real hardware run on the **Series S** development console (OS 26100.9662): the MSIX installs and activates; **70,501 UnityPlayer relocations** are fixed at a non-preferred base; Unity's GC suspend loop is found and fixed |
| **2026-11-19** | RACCOIN reaches its own menu and plays a live run; God of War boots and renders the forest across four independent boots |

### Capture manifest

| Capture | File | Format | What it is |
|---|---|---|---|
| RACCOIN main menu | `docs/proof/raccoin-main-menu.png` | 1280×720 PNG | The game's own Unity menu: Play Game, Collection, Profile, Quit, version 1.0.15, newsletter panel |
| RACCOIN gameplay | `docs/proof/raccoin-gameplay.png` | 1280×720 PNG | A live run at Round 8/15: Current 392, Combo 35, a full coin bed and populated HUD |
| RACCOIN gameplay clip | `docs/proof/raccoin-gameplay.gif` | 800×450 GIF, 36 frames / 7.2 s | The same run in motion, downscaled for inline README playback |
| RACCOIN gameplay clip | `docs/proof/raccoin-gameplay.mp4` | 1280×720 MP4, 36 frames / 7.2 s | Full-quality capture of the same run; click the GIF to open it |
| God of War, boot 1 | `docs/proof/gow-forest-01.webp` | 256×144 WebP | First forest render: scene recognizable, large black occluders, missing foliage and broken material regions |
| God of War, boot 2 | `docs/proof/gow-forest-02.webp` | 256×144 WebP | Character and materials resolve: skin, tattoo, cloth, handprint, bark; equipment and foliage still sparse |
| God of War, boot 3 | `docs/proof/gow-forest-03.webp` | 256×144 WebP | More geometry and depth: leather belt/skirt, axe in hand, denser layered trees and foliage |
| God of War, boot 4 | `docs/proof/gow-forest-04.webp` | 256×144 WebP | Full forest composite: god rays, layered foliage, full character, axe and handprint together |

The RACCOIN stills and clip are from the **same run**: the last frame of the
clip matches the gameplay still (Current 392, Combo 35, Round 8/15). The four
God of War images are **four different boots**, ordered by increasing render
coverage — not four frames from one sequence.

### 🦝 RACCOIN — main menu → playable run

<table>
<tr>
<td width="50%"><img src="docs/proof/raccoin-main-menu.png" width="100%" alt="RACCOIN main menu rendered on Xbox through Tiramisu"/><br/>
<sub><b>Main menu.</b> Unity UI composited through the guest's D3D11 swapchain:
Play Game, Collection, Profile, Quit, version 1.0.15. This is a real frame,
not a launcher mock-up.</sub></td>
<td width="50%"><img src="docs/proof/raccoin-gameplay.png" width="100%" alt="RACCOIN gameplay with a live coin bed, round counter, score and combo"/><br/>
<sub><b>Gameplay.</b> Round 8/15, Current 392, target 10,000, combo 35, a full
coin bed. The simulation, HUD and renderer are all live at once.</sub></td>
</tr>
</table>

[![RACCOIN gameplay — 7.2 s live capture](docs/proof/raccoin-gameplay.gif)](docs/proof/raccoin-gameplay.mp4)

<sub>7.2 seconds of continuous play (36 frames; 1280×720 MP4, inline GIF
800×450). The board runs from Current 116 to 392 and the top-right counter
from 221 to 250 (combo 35, Round 8/15) — the same run as the gameplay still.
The coin bed moves, the multipliers update, and the sequence never drops to a
static frame. **[▶ Open the MP4](docs/proof/raccoin-gameplay.mp4)**</sub>

### 🪓 God of War — the forest renders

Four separate boot attempts, in order. Each one cleared a different class of
failure; the scene converges instead of producing one lucky frame.

<table>
<tr>
<td width="50%"><img src="docs/proof/gow-forest-01.webp" width="100%" alt="God of War forest scene, first boot: heavy occlusion and missing geometry"/><br/>
<sub><b>Boot 1 — the scene appears.</b> Kratos and the forest are recognizable,
but large black occluders, missing foliage and broken texture/material regions
still dominate the frame.</sub></td>
<td width="50%"><img src="docs/proof/gow-forest-02.webp" width="100%" alt="God of War forest scene, second boot: character and materials resolve"/><br/>
<sub><b>Boot 2 — materials resolve.</b> Skin, cloth, the axe, the yellow
handprint and the tree bark come through; the scene is stable enough to read
as the opening forest.</sub></td>
</tr>
<tr>
<td width="50%"><img src="docs/proof/gow-forest-03.webp" width="100%" alt="God of War forest scene, third boot: foliage and depth return"/><br/>
<sub><b>Boot 3 — depth returns.</b> Foliage density, tree layering, sky and
lighting composite; the frame gains the depth and silhouette of the real
scene.</sub></td>
<td width="50%"><img src="docs/proof/gow-forest-04.webp" width="100%" alt="God of War forest scene, fourth boot: full composite with god rays and character detail"/><br/>
<sub><b>Boot 4 — full composite.</b> God rays, layered trees, alpha-tested
foliage, character skinning, the axe and the handprint all render together.
This is the forest scene end-to-end.</sub></td>
</tr>
</table>

<sub>God of War captures are preview-resolution stills (256×144); they prove the
scene composition and the render path, not final output resolution or frame
pacing. The interactive front-end/menu layer and sustained gameplay are the next
gate.</sub>

## 🔬 What this proves

The short version: the project has crossed from *"the runtime can execute
pieces of a game"* to *"real games complete their own boot and draw their own
frames."* The claim ledger below is deliberately narrow; the rest of this
section explains the chain behind each row and the limits of the evidence.

### Claim ledger

| Claim | Evidence | Status |
|---|---|---|
| RACCOIN's own main menu renders on console | 1280×720 still with the game's UI, version 1.0.15 | **Proven** |
| RACCOIN is playable | Live-run still + 36-frame clip; HUD state changes while the coin bed moves | **Proven for the captured session** |
| God of War boots and runs | Four independent boots, each reaching the forest scene | **Proven** |
| God of War renders a real composite scene | Boot 4 shows character, equipment, foliage, lighting and god rays together | **Proven at preview resolution** |
| God of War is playable | No menu/gameplay input evidence yet | **Not claimed** |
| Audio works for either title | No audio artifact in this set | **Not demonstrated** |
| Frame rate is acceptable | Capture is 5 fps; no frame-time data | **Not measured** |
| Long-session stability | 7.2 s clip; no soak test | **Not yet** |

### The proof chain, stage by stage

| Stage | Offline evidence | Console evidence |
|---|---|---|
| Signed UWP host | `pack.sh` makemsix + osslsigncode verify; host self-test | MSIX installs, activates and serves the dashboard; both games launch |
| PE64 load + relocation | 49-suite gate, loader fuzzer, real PE guests | **70,501 UnityPlayer relocations** fixed at a non-preferred base; RACCOIN and God of War boot |
| Import binding + guest DLLs | module table, `test_libload`, real corpus STUBs 0 | UnityPlayer, GameAssembly (96 MB IL2CPP) and game-shipped DLLs load recursively; both engines initialize |
| Win64 EH | `test_guest_eh`: funclet rethrow + boundary escape | RACCOIN's Unity/IL2CPP startup and managed-exception path do not terminate the process |
| D3D11 device + swapchain | `test_d3d` gets the host device/swapchain; D3D9/8/DDraw tests | RACCOIN menu/gameplay frames and God of War forest frames reach the TV |
| Render loop | mock-render gate: **2.7M device-context calls / ~268k DrawIndexed in 10 s** | 36-frame clip with a changing HUD; a composite God of War scene |
| Input | `test_input` (17 checks) + `test_dinput` | RACCOIN menu selection and live run interaction |
| Memory governor | suite 41, one 5 GB ledger | two commercial titles load and render inside the Game package |
| VFS / file I/O | real directory enumeration, async I/O, mmap, ISO tests | game data loads; save/profile round-trip not yet shown |
| Audio | XAudio2/DirectSound/OpenAL/FMOD sink tests | **not demonstrated by this capture set** |

### What had to be fixed along the way

These are the console-path fixes that turned the offline boot ladder into
visible frames. They are worth naming because each one maps to a failure a
normal compatibility layer would hide behind a black screen:

- **Native TLS preservation during guest execution.** Without it, RACCOIN's
  native D3D11 resource creation stalled and the splash alternated with black
  frames. Preserving native TLS lets the guest's graphics path keep its
  per-thread state while Tiramisu runs its own code.
- **Stable render target + host flip copy.** The CoreWindow backbuffer accepts
  the guest's requested sRGB view format, the guest renders into one stable
  target, and the host copies that target into the Xbox flip chain. This is
  what removed the alternating-black-frame failure mode.
- **GC thread adoption / safe suspend.** Unity's Boehm-class collector needs to
  suspend guest threads. The first console run died with *"Fatal error in GC:
  SuspendThread loop failed"* because the runtime handed the collector a
  pseudo-handle. The fix adopts the real current thread and uses a
  Boehm-style park/verify/resume loop so the collector can suspend safely.
- **Native GameInput path.** Xbox keyboard/mouse input reaches foreground
  guests through the native GameInput path instead of a desktop-only input
  assumption.
- **Memory-budget bridge + host manifest tweaks.** The God of War debug line
  exercised the 5 GB ledger and host manifest changes needed to keep a larger
  title inside the Game-package envelope.

The point is not the individual patches; it is that none of them were papered
over. The failure was named, the smallest layer was fixed, and the next boot
showed the difference.

### RACCOIN: what the menu frame had to do

The menu is not a splash screen. Before that frame can be presented, the
runtime has already:

1. Activated the signed UWP host, created the CoreWindow and the flip chain.
2. Mapped `RACCOIN.exe`, applied relocations, bound its imports, run TLS
   callbacks and executed `DllMain(PROCESS_ATTACH)`.
3. Recursively loaded the Unity engine (`UnityPlayer.dll`), the 96 MB IL2CPP
   assembly (`GameAssembly.dll`) and the rest of the game's shipped DLLs.
4. Initialized IL2CPP metadata, managed threads, the garbage collector and the
   Win64 exception path Unity uses when managed code throws through native
   frames.
5. Created the D3D11 device and swapchain, then built the UI pipelines,
   textures and buffers the menu needs.
6. Delivered input events so the menu can be navigated — the pointer is over
   **Play Game**, and the game is not sitting on a static title card.
7. Presented the frame through the host swapchain to the TV.

The screenshot itself carries the game's own proof of life: **version 1.0.15**,
the **Play Game / Collection / Profile / Quit** menu, the **Newsletter
Sign-ups** panel and the social icons. Those are RACCOIN's assets, drawn by
RACCOIN's UI, through Tiramisu — not a launcher mock-up.

### RACCOIN: what the gameplay frame and clip add

The gameplay still is a different class of proof: the menu proves *boot and
UI*, while the run proves **the game loop is actually running**. The HUD is
populated with live run state:

| HUD element | Value in the still | What it shows |
|---|---|---|
| Round | 8/15 | the run has advanced through multiple rounds |
| Current | 392 | the score/progress counter is populated |
| Target | 10,000 | the game's goal state is initialized |
| Combo | 35 | the combo system is tracking live play |
| Top 1 | 61 | a run statistic has changed from its starting value |
| Score Rate | 2.8 | the multiplier is active, not a static label |
| Hand In | 19 | the held-coin/queue state is live |
| Spin | 16 / 8 | the spin counters are populated |
| Prizes / Run Data / Exchange | visible | the game's own panels and controls are composited |

The exact rules do not matter for the proof. What matters is that these are
**values from the game's state machine**, rendered by the game's HUD, while the
coin bed is being simulated underneath.

The clip turns that into motion. Across its 36 frames (7.2 s at a 5 fps
capture cadence):

| Counter | First frame | Last frame |
|---|---|---|
| Current | 116 | 392 |
| Top-right counter | 221 | 250 |
| Top 1 | 22 | 61 |
| Hand In | 44 | 19 |
| Score Rate | 1.0 | 2.8 |

The last frame also shows **Combo 35**, which matches the gameplay still — the
still and the clip are the same run. The coin bed moves, coins cascade, the
multipliers update and the HUD advances while the frame keeps presenting.
That is the difference between a render-loop harness and a game.

**The 5 fps number is the capture cadence, not the game's frame rate.** The clip
is evidence of live state changes, not a performance benchmark; frame-time
capture is still on the list.

### God of War: what the forest frame had to do

The forest is a composite scene, not a clear screen and not a single triangle.
For that frame to exist, the runtime had to carry the game through:

- mapping and relocating a large x64 executable plus its shipped DLLs;
- binding the game's import surface;
- starting the engine's worker threads and file/asset path;
- creating the D3D11 device and swapchain through the host proxies;
- creating textures, buffers, samplers, shaders and render targets;
- issuing the draw calls for terrain, trees, alpha-tested foliage, the
  character, cloth, the axe and the handprint;
- compositing lighting and atmosphere — visible as the light shafts and depth
  layering in boot 4;
- presenting the result through the console flip chain.

The four images are the same scene at four different levels of render
coverage. That is why they are more useful than a single "it works" screenshot:
they show *which* classes of work were still missing.

### The four-boot ladder, frame by frame

| Boot | What is visible | What it indicates | What is still missing |
|---|---|---|---|
| **1** | Kratos and the forest are recognizable, but large black occluders, patchy handprint texture, missing foliage cards and broken bark/rock regions dominate | The scene graph and some draws are live; the render path is reaching the TV | Most materials, foliage coverage, equipment and lighting |
| **2** | Skin, red tattoo, cloth, crisp yellow handprint and tree bark resolve; the character reads clearly | Character textures/materials and key scene materials are bound | Equipment meshes, foliage density, depth and atmosphere |
| **3** | Leather belt/skirt, the axe in hand, denser foliage, layered trees and a readable sky appear | More geometry/material coverage, alpha foliage and equipment are online | Final lighting/atmosphere and full scene density |
| **4** | God rays, layered foliage, full character detail, the axe and the handprint all render together | The composite scene is working end-to-end | Interactive front end, gameplay, audio and performance validation |

The visual difference between boots is consistent with progressive render-path
fixes — missing surfaces, then materials, then geometry/foliage, then lighting.
It is not a controlled A/B experiment: each boot is a separate test with its
own fix set. That is exactly why the sequence is credible — the scene
**converges** instead of producing one lucky frame.

### Why four boots is stronger than one frame

- A single frame can be a cached shader, a lucky asset state or a screenshot
  from a different build. Four boots are four independent runs.
- Each boot removes a different visible failure class. The later frames are not
  just "prettier"; they contain geometry, materials and lighting that were
  absent earlier.
- The sequence shows the renderer is **repeatable enough to iterate on**: the
  same scene comes back each time, with the fix visible in the next capture.
- It also demonstrates the project's **fail-visible** method. The earlier God
  of War boot died with a named `0xc0000005`; the next boots did not hide the
  corruption, they removed it class by class. That is slower than claiming
  success, and it is why the captures are trustworthy.

### How to read the evidence like a reviewer

1. **RACCOIN menu:** check the version string (`1.0.15`), the four menu items,
   the newsletter panel and the pointer over **Play Game**. A generic launcher
   mock-up would not carry the game's own UI version and newsletter panel.
2. **RACCOIN gameplay:** check the HUD values against the still. Round 8/15,
   Current 392, Combo 35 and a full coin bed are game state, not painted-on
   labels.
3. **RACCOIN clip:** compare the first and last frames. Current 116 → 392,
   Top 1 22 → 61, Hand In 44 → 19. A static image or a frozen build would not
   change those values.
4. **God of War:** compare boots 1 → 4 in order. Watch the handprint, the axe,
   the foliage cards, the tree depth and the light shafts. Each later boot adds
   visible coverage rather than repeating the previous frame.
5. **Resolution caveat:** the God of War stills are 256×144 preview captures.
   They are enough to identify the scene and the progression, but they are not
   evidence of final output resolution, HDR or frame pacing.

### Reproduce the proof

The console path is the same one in [Quick start](#-quick-start-console):

```sh
tools/build-test.sh                       # offline gates, real PE guests
tools/build-host.sh                       # UWP host cross-build
tools/pack.sh                             # makemsix pack + sign + verify

DP_HOST=<console-ip> DP_PASS=<portal-pass> \
  tools/deploy.sh install out/Tiramisu.msix out/dev.cer

# upload the owned game folder/zip through the TV dashboard, pick the exe,
# launch, then capture the TV:
tools/deploy.sh screenshot tv.png
```

Re-capturing will not produce pixel-identical frames — the coin bed and the
forest are live scenes — but it should reproduce the same classes of frame:
RACCOIN's menu and a populated run, and God of War's forest at successively
higher render coverage as the remaining fixes land.

### What is still missing

**God of War**

- **Interactive front end / menu layer.** The forest scene renders; the normal
  menu/title flow and its input/focus handling are the next gate.
- **Sustained gameplay.** Camera, combat, animation blending, streaming and
  save/profile behavior are not exercised by these stills.
- **Audio.** No audio artifact in this set.
- **Performance.** Frame time, GPU/CPU utilization, memory headroom under the
  5 GB Game-package budget and shader-compile stutter are unmeasured.
- **Output quality.** The captures are 256×144; 720p/4K/HDR output is not
  qualified.
- **Stability.** No soak test, no crash-loop test, no long-session memory data.

**RACCOIN**

- **Soak test.** The clip is 7.2 s; hours-long stability is not yet measured.
- **Frame-time capture.** The 5 fps clip is a capture cadence, not a game
  frame-rate measurement.
- **Audio verification.** The game is playable; audio is not demonstrated in
  this evidence set.
- **Save/profile round-trip.** The menu exposes Profile and Collection; those
  flows still need a persistence check.
- **Shutdown hygiene.** The offline CrashReporter shutdown gap is named in the
  compatibility DB; it does not block the playable run but remains cleanup.

### What this does **not** prove

- **Not universal compatibility.** Two titles prove two engine classes, not
  every game. The compatibility DB is still the source of truth.
- **God of War is not "playable" yet.** The forest renders; the interactive
  front-end/menu layer and sustained gameplay are still being brought up.
- **RACCOIN's clip is 7.2 seconds.** It proves live play, not hours of
  stability or a frame-time target. Soak testing is next.
- **A still image is not a frame-rate or audio measurement.** The claim rests
  on the console run path, the changing HUD across the clip and the four
  independent God of War boots — not on the images alone.
- **No DRM bypass, ever.** These are games the user owns. Anti-cheat titles,
  32-bit x86 games and D3D12-only AAA remain out of scope or unproven.

## ⚙️ How it works

### 1. Load — the PE64 loader

A real Windows PE64 is mapped from the game directory (or a mounted `.iso`):
DOS → NT headers → section mapping with per-section protections → DIR64 /
HIGHLOW relocations → import binding → TLS callbacks → **delay imports bound
eagerly** (resolvable symbols get real addresses; unresolvable ones get a
fail-visible thunk so the game boots and the log names the gap) → the whole
image is **W^X sealed** (code pages RX, no RWX ever — a two-phase JIT
fallback arms RW and flips RX on `FlushInstructionCache` for engines that
JIT, like Unity mono).

### 2. Bind — the module table

Every import routes to one of three classes:

- `SYSTEM` — the console's real DLL (proven-present families only; the host
  binds via `LoadLibraryW`+`GetProcAddress`),
- `BUILTIN` — Tiramisu implements it with Wine-observable semantics
  (kernel32 core/memory/thread, user32 window layer, graphics, audio, COM,
  registry, ~25 families, ~2,000 functions),
- `STUB` — exported, logs when called, returns 0: **fail-visible, never a
  silent lie**. Every stub hit in a log is roadmap work.

Guest-shipped DLLs (UnityPlayer, GameAssembly, SDL3, libstdc++, FMOD…)
load recursively as real PEs and their `DllMain(PROCESS_ATTACH)` actually
executes.

### 3. Run — native execution

The guest's entry point is called on a host thread at native speed. Threads
with Win32 semantics, sync primitives, TLS, real file enumeration, async
file I/O with completion ports, and the memory budget governor (one ledger
for commit + heap + mapped views; over-budget refusals are **named**, which
is what keeps the console's 5 GB Game-package limit survivable) are provided
by `trt`.

### 4. Draw — the console passthrough

The guest's `D3D11CreateDevice` gets the **host's real console device**;
`CreateSwapChainForHwnd` (and every other swapchain-creation variant) gets a
proxy over the **host's real CoreWindow swapchain** — `Present`,
`GetBuffer`, `ResizeBuffers` forward to real console DXGI. D3D9/D3D8/DDraw
games are translated onto the same backend. Vulkan games run through the
built-in Vulkan 1.0–1.3 ICD: SPIR-V → HLSL → D3D11 on the console GPU.
OpenGL games ride the gl4es-derived GL→D3D11 path. Shader execution is
**the console's real D3D11** — no shader translation needed for D3D11 guests.

### 5. Survive — Win64 exception handling

Full Win64 C++ EH — the part that took the longest: `.pdata` personality
search, two-pass `RtlUnwindEx`, MSVC funclet rethrow splicing, VEH
first-chance, and the host→guest boundary pad for exceptions that escape the
guest. Measured on real Unity IL2CPP games whose engines raise managed
exceptions through native frames.

### 6. Prove — boot ladders are measured, not promised

Every claim in the status table traces to a test or a captured log — see
[docs/03-evidence.md](docs/03-evidence.md). The offline harness can inject
mock GPU hooks so a real game's **render loop executes end-to-end** with
every vtbl call counted:

```text
RACCOIN.exe (real Unity IL2CPP Steam game) — offline boot ladder:
  CRT-STARTUP → WINMAIN → UNITYMAIN2 (IAT-probed) → WINDOW →
  D3D11 DEVICE ACQUIRED → PUMP ENTERED → PUMP QUIT
  mock-render gate: 2,730,468 device-context calls, ~268,000 DrawIndexed
  in the 10 s window, clean ExitProcess(0).                     7/7 markers
```

## 🎮 What boots today

| Game (corpus, gitignored) | Type | Result |
|---|---|---|
| **RACCOIN** | Unity 6 IL2CPP, Steam, D3D11 | **CONSOLE-PROVEN — fully playable.** Main menu → live run → coin physics → HUD/score/round progression; 7.2 s capture. [Proof](#-proof--console-run-2026-11-19) |
| **God of War** (PC) | AAA in-house engine, x64, D3D11 | **CONSOLE-PROVEN — boots, runs, renders.** Four independent boots converge on the opening forest scene; interactive front-end/menu layer next. [Proof](#-proof--console-run-2026-11-19) |
| **Ultra Pool** | Godot 4.3 + GodotSteam (Vulkan) | **Full lifecycle** — scene load → pump → Godot's own quit chain, exit(0) |
| **Go Next (demo)** | Unity IL2CPP | boots, reaches `UNITYMAIN2` |
| OrTMM | PyInstaller / commercial | full CRT→WinMain→window→pump lifecycle |
| BotW-Native (launcher + recomp exe) | BOTW Wii U static recomp | whole MinGW/SDL3/OpenSSL/glslang binary set loads + `DllMain` clean |
| GoWLauncher | Win32 launcher | loads 1/0 — launcher only, not the game above |

The console rows are the ones that matter: RACCOIN's 7/7 offline ladder,
2.7M-call mock-render gate and the playable capture are three independent
proofs of the same path; God of War adds a second, much heavier engine class.

**Engine classes covered:** Unity IL2CPP + **mono** (the W^X two-phase JIT
fallback enables runtime JIT), **AAA in-house D3D11 (God of War)**, Godot 4.x
(Vulkan), PyInstaller/Python, static-recomp ports, Win32 launchers, GDI+ UI
apps, FMOD-audio games.

**Not yet:** 32-bit x86 (PE32) games — needs the M7 x86 engine; D3D12 AAA
titles offline (console D3D12 path exists, hardware-verification pending);
Wwise/Miles-audio games (named next bridges). See
[GAP-MAP.md](GAP-MAP.md).

## 🖥️ Dashboard (TV) & phone/web dashboard

The launcher on the TV has three screens — cycle with **RB/LB** (or `Tab`):

- **Shelf** — game list (U/D pick, L/R choose the exe when a game has several),
  A launches, Y opens the Steam QR link, R3/F1 toggles gamepad-as-mouse.
  The LAN URL + QR for game transfer is always on screen.
- **Settings** — editable rows: default input mode (`auto`/`controller`/`kbm`/
  `pad2kbm`), pad-as-mouse + wheel speed, pad vibration on/off, the **Windows
  version reported to games** (major.minor.build, Win11 personality toggle),
  write-watch GC on/off (GetWriteWatch kill-switch), Steam link on/off,
  QR overlay on/off, log tail size. Every change persists to
  `LocalState\settings.ini` and applies to guests immediately.
- **Live log** — the same ring the web dashboard shows, streamed from memory.

The phone/PC web dashboard (open the LAN URL shown on the TV, or scan the QR)
adds a Connect card, the game manager (**upload folder/zip → extract → pick
exe**, per-game args/appid/input, launch/stop/delete), the live log, and the
**same settings editor** (`GET/POST /api/settings`) — all offline-gated in
`tests/test_webxfer.cpp`.

## 🚀 Quick start (console)

```sh
# 1. install the runtime (from a PC on the same LAN)
DP_HOST=<console-ip> DP_PASS=<portal-pass> \
  tools/deploy.sh install out/Tiramisu.msix out/dev.cer

# 2. open the dashboard shown on the TV (or scan the QR) and upload
#    your game as a folder or .zip — pick its .exe — press launch

# 3. watch the TV
tools/deploy.sh screenshot tv.png
```

Prereqs on the console: Device Portal enabled and **"Treat UWP apps as games
by default" ON** (5 GB limit, not 1 GB). Games you **own**; DRM-free builds
or your own Steam account auth. **No DRM bypass, ever.**

## 🔨 Build (Linux, all cross)

```sh
tools/build-test.sh    # full offline gate: 49 suites, real PE guests executed
tools/build-host.sh    # the UWP host cross-build (clang-cl + lld-link)
tools/pack.sh          # makemsix pack + sign + verify → out/Tiramisu.msix
```

Needs: LLVM 19 (`clang-cl`, `lld-link`), xwin splat at
`~/.cache/uwp-crossbuild/xwin`, `makemsix`, `osslsigncode`, and the
uwp-crossbuild wrapper (defaults to `~/gow-uwp/ref/uwp-crossbuild`; override
with `UWP_CROSSBUILD`). Python + Pillow for the branding assets
(`package/branding/` is the source of every packaged logo; regenerate with
`python3 tools/gen-assets.py`).

## 📊 Status (honest)

> Every layer is "done" only when a test or a console run proves it.
> Failures are **visible and named** — never silent.

| Layer | State |
|---|---|
| Offline loader (real PE guest executed on Linux) | **PASS** — mapping, DIR64/HIGHLOW reloc, import binding, delay imports, W^X seal, entry execution |
| Win64 C++ EH (SEH) | **PASS** — pass-1 personality search, RtlUnwindEx pass-2, funclet rethrow splice, geometric purge, VEH, boundary swallow pad |
| Real-game corpus | **PASS** — 29 PEs load; **RACCOIN playable on console**; **God of War renders the forest on console**; UltraPool/GoNext/OrTMM boot; render loop proven (2.7M GPU calls) |
| D3D11/DXGI guest | **PASS on console** — real device + CoreWindow swapchain handed to RACCOIN and God of War; full swapchain proxy |
| D3D9/D3D8/DDraw → D3D11 bridges | **BUILT** — SDK-exact vtables |
| Vulkan 1.0–1.3 ICD (SPIR-V→HLSL→D3D11) | **BUILT** — Godot 4.3 boots through it |
| Audio (XAudio2 / DirectSound / WASAPI / OpenAL / winmm / **FMOD**) | **BUILT** — one real XAudio2 2.8 sink; FMOD WAV/OGG decode for real |
| Memory + budget governor | **BUILT** — 5 GB ledger, W^X two-phase JIT (mono enabler), write-watch |
| Guest-shipped DLLs, delay imports, api-set contracts | **BUILT** — recursive bind, DllMain run, UCRT C-library real answers |
| Registry, COM/OLEAUT, shell, COMCTL, GDI, **GDI+** | **BUILT** |
| Steam | **BUILT (auth only)** — QR login; `BIsSubscribedApp` answers 0 (no DRM bypass) |
| ISO / disc images | **BUILT** — `.iso` → read-only `D:` drive, lazy per-file extraction |
| Input (keyboard/mouse/gamepad, DInput8, XInput, Raw Input) | **PASS (RACCOIN)** — CoreWindow events → guest getters + pump; pad-as-mouse; menu and run are interactive |
| Console run | **PASS (2026-11-19)** — signed MSIX deploys; RACCOIN fully playable; God of War boots/renders the forest; front end + sustained play next |
| 32-bit x86 (PE32) | **not supported** — needs an x86 engine; see GAP-MAP W5 |

<details>
<summary><b>Full layer-by-layer table (60+ rows)</b> — click to expand</summary>

| Layer | State |
|---|---|
| UWP host (CoreWindow/D3D11/XInput/LocalState log) | **BUILDS** — signed `Tiramisu.msix` produced and verified |
| Phone transfer (QR on TV → upload zip → launch) | **BUILT** — ws2_32 HTTP server, miniz zip extraction, launch handoff; console verdict pending |
| DirectInput 8 | **BUILT** — SDK-exact vtables, mouse deltas + 256-key map from the real input ring |
| Real file enumeration (FindFirstFile family) | **BUILT** — dual-platform REAL (Linux readdir / console native passthrough), SDK-exact 592-byte `WIN32_FIND_DATAW` |
| Temp-path boot chain (PyInstaller) | **BUILT** — `GetTempPathW/2W`, VFS-relative resolution |
| /GS cookie killer | **FIXED** — `GetFileAttributesExW` layout bug found by hardware watchpoint; regression test in `test_engine.cpp` STAGE 9 |
| shlwapi / shell32 / shfolder | **BUILT** — Path* helpers, SHGetFolderPath W/A, KnownFolders, real IMalloc |
| COM boot + OLEAUT | **BUILT** — CoInitialize (STA/MTA nesting), CLSID personalities with GUID-named failures, BSTR, SafeArrays, Variants |
| COMCTL32 + GDI32 + GDIPLUS | **BUILT** — InitCommonControlsEx + ordinal `#380`, subclass family, GDI object table, GDI+ flat API (launcher UI, recorded draws) |
| UCRT C-library surface | **BUILT** — 115 ms_abi host-libc wrappers (heap/str/wcs/math/qsort/_initterm) — MinGW guests survive DllMain |
| Boot determinism — UNCERTAIN-7 all BUILTIN | **BUILT** — wininet/psapi/iphlpapi/imm32/crypt32/dbghelp honest-runtime bridges; MiniDumpWriteDump writes a REAL text crash report through the VFS |
| D3DX9 / D3DX10/11 | **BUILT** — exact math, texture load (BMP/TGA/PNG/DDS), ID3DXSprite quad batching |
| Image parsers + stb_image | **BUILT** — hand PNG/DDS decoders + vendored stb_image fallback |
| GLSL/SPIR-V → HLSL | **VENDORED** — mojoshader, d3d8to9, gl4es, stb_vorbis (licenses kept) |
| msacm32/msvfw32/winspool | **BUILT** — honest no-codec / no-printer answers |
| Pre-boot compat scanner | **BUILT** — shelf reads each PE and flags steam-auth/d3d9/d3dx9/dsound/ddraw/d3d11 before launch |
| Launcher setup UI | **BUILT** — first-run live checklist; A opens the shelf |
| Async file I/O + mmap | **BUILT** — overlapped reads → GQCS packets, `TRT_IO_MMAP_MIN` mmap fast path, suite 42 |
| Budget governor | **BUILT** — one 5 GB ledger; over-budget refusals named; suite 41 |

</details>

## 🗺️ Roadmap

- **M0 console PoC — DONE (2026-11-19).** Signed MSIX deploys; RACCOIN
  reaches its menu, starts a run and plays; God of War boots and renders the
  forest. Evidence: [Proof](#-proof--console-run-2026-11-19).
- **M1 front end + sustained play — in progress.** God of War's normal
  front-end/menu path; RACCOIN soak test and frame-time capture; audio and
  save validation.
- **Any-game breadth** — x86-32 PE32 loader + translator ladder; streaming
  maps; IOCP/dstorage; D3D12 (see [GAP-MAP.md](GAP-MAP.md))
- **Audio breadth** — MP3/ADPCM/FSB decoders for FMOD; Wwise; Miles
- **Steam depot download (M6)** — play *your* Steam library from your own
  account

## 🧱 Rules of the road

- Games you **own**; DRM-free builds or your own Steam account auth.
- **No DRM bypass, ever.** No Xbox security bypass. No game files or
  credentials in the repo.
- MIT for our code; vendored third-party keeps its licenses
  (see `THIRD-PARTY.md` and vendored dirs).

## 📚 Documentation

| Doc | Contents |
|---|---|
| [docs/03-evidence.md](docs/03-evidence.md) | what is proven vs assumed — every fix traces to a measurement |
| [docs/proof/](docs/proof/) | console capture evidence — RACCOIN menu/gameplay/clip, God of War four-boot forest ladder |
| [GAP-MAP.md](GAP-MAP.md) | live gap table, W-lanes, dependency order, triggers |
| [docs/](docs/) | architecture notes, stage logs, console playbook |

---

<div align="center">

**🍰 Tiramisu** — because the console already has the hardware; it just needed
the cake.

*Built for Xbox Dev Mode. Homebrew the honest way.*

⭐ Star this repo if you like native-speed PC games on Series X|S.

</div>
