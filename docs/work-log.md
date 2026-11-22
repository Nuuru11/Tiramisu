# Work log (timestamped, evidence-backed)

Entries are appended as work lands; every claim points at a gate or a file.

## 2026-11-22 — autofix / console / title build
- Added `tools/titlebuild.py`, `tools/autofix.py`, `tools/autopilot.py`,
  `tools/launch-with-autofix.sh`, fix DB and gates. Evidence:
  `tools/autofix-test.sh`, `tools/titlebuild-test.sh`, `tools/host-autopilot-test.sh` → ALL PASS.
- Added explicit host-side autopilot callbacks in `src/host/main.cpp` with a
  header-only state machine; gate `tools/host-autopilot-test.sh` → ALL PASS.
- Console proof on 10.0.0.23: ultrapool boots to `vkQueuePresentKHR frame=1`;
  Sekiro real failed boot captured; `docs/console-validation.md`.

## 2026-11-22 — x86-32 clean-room engine
- `src/trt/x86_32.cpp` + `x86_image.cpp` execute real PE32 entries and import
  traps through real host bridges. `tools/x86-test.sh`, `tools/x86-image-test.sh`.
- Real corpus DLLs measured in `docs/pe32-execution-report.md`.
- Fixed from real-guest evidence (each gated): `0x0D` instruction length,
  push/pop opcode mask (`&f0` matched POPs as PUSHes), `0x81`, `INC/DEC`,
  `RET imm16`, `FF /6`, LOCK prefix, 16-bit operand prefix, FS/GS TEB bases.
- Result: `tools/pe32-completion-test.sh` → SDL2 and discord_game_sdk clean,
  binkw32 named stop (`2/3`), RESULT ALL PASS.

## 2026-11-22 — D3D12
- Submission descriptors + host submit hook in `src/host/swapchain.cpp`;
  triangle gate prints `HOST SUBMIT PASS`, `host submit of 1 draw(s)`.
- Console finding: `d3d12.dll` routes to the real UWP device (passthrough), so
  the hook is exercised offline only — documented in `docs/console-validation.md`.

## 2026-11-22 — integration
- Preserved concurrent stable work (steam/x86 sources) into a single commit,
  added missing build-list entries; full gate links with 0 undefined refs,
  `CORPUS: 40 ok, 0 failed`, final `RESULT: ALL PASS`.

## 2026-11-23 — fuller PE32 process model
- `trt_x86_run_cdecl` now seeds a real 32-bit TEB: ExceptionList chain,
  StackBase/StackLimit, Self pointer, stack guard. Gates stay green; binkw32
  still returns through its inherited EBP (exact wrapper at 0x2240 analyzed).

## Current named gaps (not hidden)
1. `binkw32.dll` entry: `0x2240` wrapper pops an inherited EBP the synthetic
   cdecl frame does not provide; needs fuller process/SEH setup.
2. RACCOIN mock-render: two measured faults in `GameAssembly.dll`
   (`+0x72b56e` null allocator result, `+0x774060` bad unwind stack).
3. D3D12 submit hook: offline-gated only due to real-device passthrough.
4. Console content: missing `.pck` / `S\pack.dat` on the staged titles.
