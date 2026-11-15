<div align="center">

# 🍰 Tiramisu UWP

### Run PC games on Xbox Series X|S — natively.

**A Windows PE64 application runtime for Xbox Dev Mode.**
Wine's job, targeting UWP instead of POSIX.

![Status](https://img.shields.io/badge/status-PoC%20boots%20real%20Steam%20games-brightgreen)
![Gates](https://img.shields.io/badge/offline%20gates-38%2F38%20pass-brightgreen)
![Platform](https://img.shields.io/badge/platform-Xbox%20Series%20X%20%7C%20S-107C10?logo=xbox&logoColor=white)
![Guests](https://img.shields.io/badge/guest-PE64%20%2B%20Unity%20IL2CPP-blue)
![License](https://img.shields.io/badge/license-MIT-green)
![Language](https://img.shields.io/badge/C%2B%2B-17%20%2F%20clang--cl-00599C?logo=cplusplus&logoColor=white)
![DRM](https://img.shields.io/badge/DRM%20bypass-none%2C%20ever-red)

[How it works](#-how-it-works) · [Status](#-status-honest) · [Build](#-build-linux-all-cross) · [Deploy](#-deploy-console) · [Evidence](docs/03-evidence.md) · [Gap map](GAP-MAP.md)

**[⬇ Deploy to your console](#-deploy-console)** · **[🎮 Compatibility](#-what-boots-today)**

</div>

---

## What is this?

One signed UWP "Game" app that loads Windows `.exe`s **in-process** on Xbox
Series X|S Dev Mode. No CPU emulation for 64-bit games: the console is x64,
so guests run at **native speed** on the same core, the same ABI, the same
process — Tiramisu is the operating system around them.

Named after the fruit-emulator tradition (yuzu, citra, papaya).

```
Tiramisu UWP host (CoreWindow + D3D11 flip + XInput, 5 GB as a Game package)
  └── trt — Tiramisu runtime
        ├── PE64 loader     map → sections → relocate → bind imports → TLS
        ├── Win64 EH        .pdata personality, RtlUnwindEx, funclets, VEH
        ├── module table    every import routed: SYSTEM | BUILTIN | STUB
        ├── bridges         20+ Win32 families implemented on UWP
        ├── VFS             C:\ → LocalState\drive_c, D:\ → mounted .iso, Z:\ → package
        └── Steam           QR auth (own account) — no DRM bypass, ever
```

## ⚙️ How it works

1. **Load.** A real Windows PE64 is mapped from the game directory (or a
   mounted `.iso`), sections relocated, imports bound through a *module
   table* that routes every symbol to one of three routes:
   - `SYSTEM` — the console's real DLL (proven present families only),
   - `BUILTIN` — Tiramisu implements it (kernel32 core, user32, d3d9/d3d8/
     ddraw→D3D11, dsound/openal/winmm→XAudio2, ole32/oleaut, advapi32
     registry, and ~20 more families),
   - `STUB` — answers honestly-named failure; **never a silent lie**.
2. **Run.** The guest's entry point is called on a host thread at native
   speed. Threads, sync, TLS, memory (including write-watch for the
   Boehm/IL2CPP incremental GC) and the Win32 pump are provided by `trt`.
3. **Draw.** The guest's `D3D11CreateDevice` gets the host's real device;
   its `CreateSwapChainForHwnd` gets the **host's CoreWindow swapchain**.
   D3D9/D3D8/DDraw games are translated onto the same backend.
4. **Survive.** Full Win64 C++ exception handling — the part that took the
   longest: `.pdata` personality search, two-pass `RtlUnwindEx`, MSVC
   funclet rethrow splicing, VEH first-chance, and the host→guest boundary
   pad for exceptions that escape the guest (UnityPlayer's native wrapper
   catch). Measured on real Unity IL2CPP Steam games.
5. **Boot ladders are measured, not promised.** Every claim in the status
   table traces to a test or a captured log — see
   [docs/03-evidence.md](docs/03-evidence.md).

```text
RACCOIN.exe (real Unity IL2CPP Steam game) — offline boot ladder, v0.35:
  CRT-STARTUP → WINMAIN → UNITYMAIN2 (IAT-probed) → WINDOW → D3D11
  → PUMP ENTERED → PUMP QUIT          CORPUS-EXE: 1 booted, 0 failed
```

## 🎮 What boots today

| Game (corpus, gitignored) | Type | Result |
|---|---|---|
| **RACCOIN** | Unity IL2CPP Steam game | **7/7 ladder — boots, runs, clean self-shutdown** |
| **Go Next (demo)** | Unity IL2CPP | boots, reaches `UNITYMAIN2` |
| OrTMM | PyInstaller/commercial | runs to watchdog (child-spawn path open) |
| GoWLauncher | Win32 launcher | loads 1/0 |

Support targets: **any PE64 game** — Steam titles (auth = your own account),
DRM-free builds, and `.iso` disc images (read-only mounted drive with lazy
per-file extraction under the 5 GB console budget). 32-bit x86 games are not
supported yet (see [GAP-MAP](GAP-MAP.md)).

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
adds a Connect card, the game manager (upload folder/zip, pick exe, per-game
args/appid/input, launch/stop/delete), the live log, and the **same settings
editor** (`GET/POST /api/settings`) — all offline-gated in `tests/test_webxfer.cpp`.

## 🚀 Quick start (console)

```sh
tools/build-host.sh    # offline gate → out/layout/tiramisu.exe
tools/pack.sh          # makemsix pack + sign + verify → out/Tiramisu.msix
DP_HOST=<console-ip> DP_PASS=<portal-pass> \
  tools/deploy.sh install out/Tiramisu.msix out/dev.cer
tools/deploy.sh screenshot tv.png    # LOOK at the TV
```

Prereqs on the console: Device Portal enabled and **"Treat UWP apps as games
by default" ON** (5 GB limit, not 1 GB).

## 🔨 Build (Linux, all cross)

```sh
tools/build-test.sh    # full offline gate: 38 suites, real PE guests executed
tools/build-test-cached.sh   # fast variant
```

Needs: LLVM 19 (`clang-cl`, `lld-link`), xwin splat at
`~/.cache/uwp-crossbuild/xwin`, `makemsix`, `osslsigncode`, and the
uwp-crossbuild wrapper (defaults to `~/gow-uwp/ref/uwp-crossbuild`; override
with `UWP_CROSSBUILD`).

## 📊 Status (honest)

> Every layer is "done" only when a test or a console run proves it.
> Failures are **visible and named** — never silent.

| Layer | State |
|---|---|
| Offline loader (real PE guest executed on Linux) | **PASS** — mapping, DIR64/HIGHLOW reloc, import binding, W^X seal, entry execution |
| Win64 C++ EH (SEH) | **PASS (v0.35)** — pass-1 personality search, RtlUnwindEx pass-2, funclet rethrow splice, geometric purge, VEH, boundary swallow pad. Fixtures ALL PASS |
| Real-game corpus | **PASS** — 9 x64 PEs load; RACCOIN + Go Next boot the full ladder; boot-determinism proven (zero unproven console routes) |
| D3D11/DXGI guest | **BUILT** — host device + CoreWindow swapchain handed to the guest; FL-contract honest E_INVALIDARG |
| D3D9/D3D8/DDraw → D3D11 bridges | **BUILT** — SDK-exact vtables (D3D9On12/cnc-ddraw semantics, MIT) |
| Audio (XAudio2 / DirectSound / OpenAL / winmm) | **BUILT** — one real console XAudio2 sink behind all four APIs |
| Guest-shipped DLLs, delay imports, api-set contracts | **BUILT** — recursive bind, DllMain run, on-demand load, fail-visible call sites |
| Registry (advapi32) | **BUILT** — full hive tree persisted next to saves; missing values fail visibly |
| Memory surfaces | **BUILT** — file mappings, threads/sync/TLS, W^X two-phase JIT, **write-watch (GetWriteWatch) honest: console passthrough / Linux soft-dirty+probe** |
| WinRT offline classes | **BUILT** — Gamepad, DispatcherQueue, DisplayInformation, Language, AppData (+ console passthrough first) |
| Steam | **BUILT (auth only)** — QR login via IAuthenticationService (own account); `BIsSubscribedApp` answers 0 (no DRM bypass) |
| ISO / disc images | **BUILT** — `.iso` → read-only `D:` drive, lazy per-file extraction, 44-check gate |
| Input (keyboard/mouse/gamepad, DInput8, XInput) | **BUILT** — CoreWindow events → guest getters + pump; pad-as-mouse |
| Console run | **PENDING** — MSIX built+signed; deploy script ready; needs Device Portal creds |
| 32-bit x86 (PE32) | **not supported** — needs an x86 engine; see GAP-MAP W5 |

<details>
<summary><b>Full layer-by-layer table (60+ rows)</b> — click to expand</summary>

| Layer | State |
|---|---|
| UWP host (CoreWindow/D3D11/XInput/LocalState log) | **BUILDS** — signed `Tiramisu.msix` produced and verified |
| Phone transfer (QR on TV → upload zip → launch) | **BUILT** — ws2_32 HTTP server, miniz zip extraction, launch handoff; console verdict pending |
| DirectInput 8 | **BUILT** — SDK-exact vtables, mouse deltas + 256-key map from the real input ring |
| Real file enumeration (FindFirstFile family) | **BUILT (v0.33)** — dual-platform REAL (Linux readdir / console native passthrough), SDK-exact 592-byte `WIN32_FIND_DATAW` |
| Temp-path boot chain (PyInstaller) | **BUILT (v0.33)** — `GetTempPathW/2W`, VFS-relative resolution |
| /GS cookie killer | **FIXED (v0.33)** — `GetFileAttributesExW` layout bug found by hardware watchpoint; regression test in `test_engine.cpp` STAGE 9 |
| shlwapi / shell32 / shfolder | **BUILT** — Path* helpers, SHGetFolderPath W/A, KnownFolders, real IMalloc |
| COM boot + OLEAUT | **BUILT** — CoInitialize (STA/MTA nesting), CLSID personalities with GUID-named failures, BSTR, SafeArrays, Variants |
| COMCTL32 + GDI32 | **BUILT** — InitCommonControlsEx + ordinal `#380`, GDI object table, font state |
| Boot determinism — UNCERTAIN-7 all BUILTIN | **BUILT** — wininet/psapi/iphlpapi/imm32/crypt32/dbghelp honest-runtime bridges; MiniDumpWriteDump writes a REAL text crash report through the VFS |
| D3DX9 / D3DX10/11 | **BUILT** — exact math, texture load (BMP/TGA/PNG/DDS), ID3DXSprite quad batching |
| Image parsers + stb_image | **BUILT** — hand PNG/DDS decoders + vendored stb_image fallback |
| GLSL/SPIR-V → HLSL | **VENDORED** — mojoshader, d3d8to9, gl4es (licenses kept) |
| msacm32/msvfw32/winspool | **BUILT** — honest no-codec / no-printer answers |
| Pre-boot compat scanner | **BUILT** — shelf reads each PE and flags steam-auth/d3d9/d3dx9/dsound/ddraw/d3d11 before launch |
| Launcher setup UI | **BUILT** — first-run live checklist; A opens the shelf |

</details>

## 🗺️ Roadmap

- **M0 console PoC** — deploy → TV screenshot + `tiramisu.log` pull → JIT
  W^X probe → RACCOIN on the TV *(MSIX is built and signed; awaiting Device
  Portal creds)*
- **Any-game breadth** — x86-32 PE32 loader + translator ladder; streaming
  maps; IOCP/dstorage; D3D12 (see [GAP-MAP.md](GAP-MAP.md))
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
| [GAP-MAP.md](GAP-MAP.md) | live gap table, W-lanes, dependency order, triggers |
| [docs/](docs/) | architecture notes, stage logs, console playbook |

---

<div align="center">

**🍰 Tiramisu** — because the console already has the hardware; it just needed
the cake.

*Built for Xbox Dev Mode. Homebrew the honest way.*

⭐ Star this repo if you like native-speed PC games on Series X|S.

</div>
