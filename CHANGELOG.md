# Changelog

## Unreleased
- Added DirectStorage bridge stubs (dstorage.dll, dstoragecore.dll) — games fall back to standard I/O.
- Added Bink 2 video bridge stubs (bink2w64.dll, binkw64.dll, bink2w32.dll) — games skip video gracefully.
- Added Oodle bridge stubs (oo2core_6_win64.dll, oo2core_8_win64.dll, oo2core_5_win64.dll) — games fall back to standard decompression.
- Added geo/locale stubs (GetGeoInfoW, GetUserGeoID) to kernel32 gap bridge.
- Added `tools/auto-compat.py` — scans any PE64 and reports per-DLL API coverage vs. Tiramisu bridges.
- Improved auto-compat DLL→bridge alias mapping (advapi32, d3d11, ole32, steam_api, fmod, etc.).
- Updated `docs/known-issues.md` with new bridges and VFS case-sensitivity issue.

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