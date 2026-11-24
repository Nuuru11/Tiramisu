# Changelog

## [0.2.4.0]
- **Pre-launch PE compatibility badges (`/games` & TV UI):** Dashboard scans every game executable (`x64` vs unsupported `32-bit x86`, D3D11/D3D9/Vulkan/OpenGL/D3D12, EasyAntiCheat/BattlEye detection) and flags unsupported builds clearly before launch. Xbox GDK/GameOS console payloads (`MicrosoftGame.Config`, `gameos.xvd`, `xgameruntime`) are now flagged as unsupported instead of looking like D3D11 games.
- **One-click Save Backups & Restore (`/api/backup-saves`, `/api/restore-saves`):** Download or restore `drive_c`, `profiles`, and `settings.ini` as a `.zip` directly from the LAN dashboard before Dev Mode package updates wipe `LocalState`.
- **One-click Debug Bundle (`/api/debug-bundle`):** Export `tiramisu-debug-bundle.zip` (`tiramisu.log`, `settings.ini`, crash reports, PE compatibility summary) for fast GitHub bug/compatibility reports.
- **Mobile & Safari Folder Upload Guidance (Issue #1):** Detect unsupported `webkitdirectory` pickers on mobile browsers and guide users cleanly to `.zip` upload or Xbox USB drives.
- **KBM edition preserved in every release:** `tools/release.sh` builds and packs both `Tiramisu.msix` + `dev.cer` (public `Xan.Tiramisu`) and `Tiramisu-EdgeSpoof.msix` + `edge-dev.cer` (experimental Edge identity for Xbox hardware-mouse passthrough). The Edge/KBM artifact is never dropped from the release; `tools/pack.sh EDGE_SPOOF=1` now actually stages, signs, and verifies it again (the support had regressed out of pack.sh).
- **Documentation & Community Templates:** Added Windows/browser Xbox Device Portal install guide, Game Compatibility & Bug Report `.github/ISSUE_TEMPLATE` forms, and scope FAQ in `README.md`.

## [0.2.3.9]
- **Obsidian & Champagne UI + XMB Silk Waves:** Rebuilt Direct2D/D3D11 console UI with 4-card widescreen cinema shelf, 1920px HD Steam hero banners, 0.34s hero crossfades, animated 5-ribbon XMB wave background, procedural XAudio2 UI SFX, and 2-step safe game uninstall.
- **HD Boot Animation & Chrome App Branding:** Added `1280x720 @ 24 fps` stereo startup animation (`boot_anim.tanim`), transparent metallic chrome icon (`package/branding/logo.png`), and `#000000` splash handoff.
- **Xbox Storage & Transfer Reliability:** Initialized `LocalState\games` and `LocalState\incoming` at boot, preserved USB drive games across rescans, hardened `/api/download-url` (`.zip` + `.iso/.bin/.img`), and auto-persisted Steam AppIDs.
- Fixed Celeste (managed XNA titles) crashing with `Could not load Microsoft.Xna.Framework.Game`: package FNA substitute assemblies into `<install>/fna` in `tools/build-host.sh` and default Mono's `assemblies_path` to them in `src/trt/mono_host.cpp`.
- Added DirectStorage (`dstorage.dll`, `dstoragecore.dll`), Bink 2 (`bink2w64.dll`, `binkw64.dll`), Oodle (`oo2core_*`), and geo/locale bridge stubs, plus `tools/auto-compat.py`.

Format: [Keep a Changelog](https://keepachangelog.com/). Versions follow the
AppxManifest `Version` field (`Major.Minor.Patch.Build`).

## [Unreleased]

### Added
- Offline console pipeline rehearsal: `tests/py_tools/fake_portal.py` (TLS, CSRF,
  basic auth) + `tests/py_tools/test_console_e2e.py` (install → upload →
  rerun-skip → launch → proof, plus error paths).
- Generated title matrix: `tools/title-matrix.py` (`--check`) +
  `docs/proof/titles.json`, with `<!-- title-matrix:measured -->` markers in
  `docs/title-matrix.md`. `console.py proof` now points at `proof.json` +
  the generator.
- FSB5 hardening: `trt_fsb5_header_sane` / `trt_fsb5_sample_sane` envelope
  checks, 64-bit name-table compare, `bad_alloc` guards in `fsb_parse` and
  `get_decoded`. Offline `tests/test_fsb5_sanity.cpp` (15 checks) wired as
  build-test §43c.
- Console-usable FSB5 toggle: `fsb_decode` setting (ini key + per-game
  `tiramisu.profile.ini`), pushed from `main.cpp` via `trt_fsb5_set_console_decode`.
- `game-stage.py --set KEY=VALUE` for per-game ini + `settings` in `staging.json`.
- `docs/known-issues.md`, `docs/troubleshooting.md`, `docs/crash-reporting-plan.md`.

### Fixed
- `console.py` now prints `ERROR: …` on connect failure instead of a traceback.
- `devportal.py` misleading 401 message corrected.

### Changed
- `docs/title-matrix.md`, `GAP-MAP.md`, `docs/04-support-roadmap.md`, `README.md`
  updated to reflect measured vs. unmeasured state.

## [0.2.2.171] — current shipped build

Baseline. See `git log` for prior history.