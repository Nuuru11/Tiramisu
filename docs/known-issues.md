# Tiramisu — known issues

Tracked limitations. Every entry here is a *measured* or *honestly unmeasured* fact,
not a guess. Rows are removed only when verified fixed on-device or in the gate.

## Console (on-device)

- **Xbox One S|X: untested and not a supported target.** Xbox One S/X are x86‑64, so
  PE32 and PE32+ guests are both in the PE space, and Dev Mode + Device Portal
  should install Tiramisu. Expected practical limits: One S is ~1.3 TFLOPS / 8 GB
  DDR3 and One X ~6 TFLOPS / 12 GB GDDR5; neither matches Series X|S headroom, so
  do not expect the Series‑S playables (RACCOIN, Celeste, God of War) or 60 FPS
  targets to reproduce. D3D12/Vulkan/Compute features and CPU‑bound titles are the
  first to fail. Treat the next step as an experiment until on-console proof exists.
- **Celeste v1.4.0.0 — audio silent.** FSB5 Vorbis decode is disabled on console after
  it heap-corrupted on a Series S. Gate defaults off (`fsb_decode = false`); per-game
  override via `tiramisu.profile.ini` (`fsb_decode=1`) is available for testing but
  unproven on-device. See `src/trt/bridge_fmod_studio.cpp`.
- **Factorio — untested on console.** Named target for 60 UPS + audio; not yet run.
  No `proof.json` exists for it.
- **No per-run `proof.json` for legacy titles.** RACCOIN / Celeste / God of War
  evidence is flat legacy artifacts (`docs/proof/*.png/mp4/webp`), not the new
  structured `docs/proof/<name>/proof.json` format.

## Recently added (this session)
- **DirectStorage (dstorage.dll)** — stubs return E_NOTIMPL; games fall back to standard I/O.
- **Bink 2 video (bink2w64.dll)** — stubs return null/false; games skip video.
- **Oodle (oo2core_6_win64.dll)** — stubs return -1; games fall back to standard decompression.
- **Geo/locale (GetGeoInfoW, GetUserGeoID)** — honest stubs returning 0.
- **Auto-compat scanner** (`tools/auto-compat.py`) — scans PE32+ (PE32 is refused); reports per-DLL API coverage.

## Engine walls (open)

- **ARM64 is the current guest-arch wall.** `game-stage.py` stages PE32 (x86-32)
  via the host x86-32 JIT (`tools/game_stage`/`src/host/main.cpp`) but refuses
  ARM64 today; native x86 execution needs a license/interpreter decision
  (docs/console-type.md and docs/04-support-roadmap.md).
- **Xbox GDK / GameOS packages are not Windows targets.** An x64 Scarlett/GDK
  build (`MicrosoftGame.Config`, `gameos.xvd`, `xgameruntime`/`gamingservices`
  imports) is a console package, not a Win32 desktop game. The LAN dashboard and
  `tools/auto-compat.py` now flag it as unsupported instead of showing a
  misleading D3D11 badge. Tiramisu does not bypass platform or DRM checks.
- **D3D12 execution beyond the triangle gate.** Device/queue/list/PSO/draw/fence/present
  gate passes (`test_d3d12_triangle`); real game draw paths (descriptor tables,
  compute, barriers) untested.
- **Video decode** — unimplemented (Bink stubs added; real decode still missing).
- **VFS case sensitivity** — FIXED: `trt_vfs_resolve_case_insensitive()` walks path components case-insensitively as a fallback when exact path doesn't exist. Fast path (exact match) unchanged for performance.
- **Universal stub fallback** — NEW: unknown DLLs and missing symbols now auto-register as STUB (fail-visible, returns 0) instead of failing the import table. Games boot instead of crashing on missing exports. Calls are logged as STUB for visibility.
- **Real Bink video decode** — stubbed only (returns null). Full decode requires RAD Game Tools SDK (proprietary). Cutscenes unavailable.
- **D3D12 game draw paths** — the triangle/presentation gate passes, but real game draw paths (descriptor tables, compute, barriers) remain untested.
- **ARM64 / non-x86 guest archs** — not feasible today (same wall as game-stage; x86-32 JIT is in the tree and gated).
- **Wwise audio** — stubbed (returns AK_Success). Audio playback unavailable but games proceed past init.
- **Crash reporting network POST** — setting toggle + local crash.log writing implemented. Network POST requires maintainer endpoint setup (see docs/crash-reporting-plan.md). Local payload shape is exercised offline via `tools/crash_report_preview.py`.
- **MinGW-compiled games** — low API coverage (e.g. botw-native 41.9%) due to GCC CRT function names not in bridge tables.
- **Performance profiling** — no baseline measurements exist; needs real hardware.
- **Wwise audio banks** — unhandled.

## Host / tooling

- **VCLibs dependency path (Windows) unverified** in `console.py install`.
- **Full `build-test.sh` gate** must complete; last run was interrupted by a
  concurrent sibling run. Re-run after that finishes.
- **Crash reporting** — local crash bundles implemented and gated (`crash_report_write`, dashboard bundle export); network POST is plan-only until a maintainer endpoint is set up (see `docs/crash-reporting-plan.md`).

## Honesty contract

Nothing is removed from this list without a measured artifact or a gate pass. If a
wall moves, update `GAP-MAP.md` and this file in the same change.