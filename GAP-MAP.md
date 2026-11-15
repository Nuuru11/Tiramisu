# GAP-MAP — what is needed to boot real PC games (paste with TIRAMISU-MASTER-PROMPT.md)

> Companion work order for AGY/Gemini sessions. Read `TIRAMISU-MASTER-PROMPT.md` (the
> contract) first; this file is the measured map of everything left.
> Produced 2026-11-14 ~14:15 EST from tool runs on THIS tree + the corpus +
> donor research (links inline). Nothing here is from memory.
>
> **Before trusting any number: re-run the two audit commands in §1. The tree
> moves hourly — two sibling sessions are actively editing it.**

---

## 0. How to work (unchanged from the contract)

- **Measure first** — reproduce with a corpus PE + the depth ladder/audit before
  writing any bridge.
- **Solo gates** — `tools/build-test.sh` etc. run ONE at a time; check for live
  writers first (`find ~/tiramisu -type f -mmin -15 -not -path './.git/*'
  -not -path './tests/out/*'`). Two sessions are live in this repo right now.
- **Fail named, never silent.** STUB hits are work items, not success.
- Do not commit unless the user asks; iteration reports are the deliverable.

---

## 1. Ground truth (measured 2026-11-14)

**Gates on the current tree (2026-11-15, ui+settings wave):**
`build-test.sh` → gate_rc=0, 38× `RESULT: ALL PASS` (incl. the new
funclet-rethrow + boundary-escape fixtures + test_settings) · corpus-run EXE
stage: RACCOIN 1 booted/0 failed (7/7 ladder), Go Next demo.exe 1/0 with
UNITYMAIN2 REACHED, OrTMM 1/0, GoWLauncher 1/0.

**RACCOIN.exe** (real Unity IL2CPP Steam game, now in `corpus-local/raccoin/`,
gitignored) — **BOOTS (2026-11-15, ehfix16)**:

```
CRT-STARTUP OK / WINMAIN ENTERED / WINDOW CREATED (hwnd=0x10101, title 'RACCOIN')
UnityPlayer.dll DllMain -> returned 0x1
GameAssembly.dll (96MB IL2CPP) DllMain -> returned 0x1
D3D11 DEVICE ACQUIRED / PUMP ENTERED / PUMP QUIT
kernel32: RaiseException(0xe06d7363) x2 -> swallowed at host->guest call boundary
                                          (UnityPlayer wrapper-catch emulation)
CORPUS-EXE: 1 booted, 0 failed, 0 skipped (x86)
```

The game then exits through its own clean shutdown path
(`IDXGISwapChain::SetFullscreenState(0) -> S_OK` → `TerminateProcess(0)`).
23→all C++ exceptions handled; the full causal chain (pass-1/pass-2 splice
clobber, stale-record windows, boundary escape semantics) is measured in
`/tmp/opencode/raccoin-ehfix9..19.log` and locked by two new fixtures in
`tests/test_guest_eh.cpp` (funclet rethrow caught; funclet rethrow escaping to
the boundary → synthetic 0 return). The `UNITYMAIN2 CALLED` marker is fixed
(ehfix19): the guest calls UnityMain2 guest→guest — no bridge line ever names
the call — so the harness now patches the EXE's IAT slot with a NAKED asm shim
(flag store + tail-jump; a C shim's prologue corrupts the guest's return
address — measured ehfix18 SIGSEGV). RACCOIN ladder: 7/7 REACHED. Go Next
demo.exe also reports `UNITYMAIN2 CALLED: REACHED (IAT-probed)` — two real
Unity games through the same gate.

### History — the one wall (now broken): our own SEH/unwind implementation.

**Import audit of the full RACCOIN stack** (`tools/audit_boot.py` — approximate;
the C++ resolver in `tests/audit_boot.cpp` is authoritative):

| PE | imports | audit gaps | real gaps after reconciliation |
|---|---|---|---|
| RACCOIN.exe | 72 | 1 | 0 (the 1 = game-dir export artifact) |
| UnityPlayer.dll | 546 | 52 | **dwmapi (2 fns)** — rest are false ws2_32 alarms |
| GameAssembly.dll | 252 | 48 | **GetWriteWatch family** — rest are baselib game-dir artifacts |
| baselib.dll | 129 | 27 | 0 real (ws2 false alarms) |

Why most gaps are false alarms: the Python tool's regex misses the ws2_32
registration style and can't know game-dir DLL exports (`UnityMain2`, the 12
C++-mangled `?Baselib_...` symbols) — those resolve at load from the game dir
(proven v0.29). **Fix the tool: its `__main__` guard is misspelled `'main'`**
so the CLI path never runs; the live session ran it via import (see
`tools/__pycache__`).

**Structural fact that changes the EH plan:**
- NO `__CxxFrameHandler*` / `__C_specific_handler` imports anywhere in the
  stack. The CRT and the C++ EH personality are **statically linked inside
  GameAssembly**. The guest brings its own EH — we must supply only faithful NT
  primitives.
- Every PE imports `KERNEL32!RtlVirtualUnwind` / `RtlUnwindEx` / `RtlUnwind`
  directly. All three ARE registered in both the kernel32 and ntdll bridge
  tables — but "registered" ≠ "faithful". That is the work.
- **IL2CPP raises EVERY managed exception as a C++ exception**
  (`il2cpp_codegen_raise_exception` → `Il2CppExceptionWrapper` — Unity's own
  IL2CPP internals doc). So C++ EH is load-bearing for gameplay, not an edge
  case: every `try/catch` in C# game code goes through our unwinder.

**In flight RIGHT NOW (orchestrated parallel agents, 2026-11-15 — do not duplicate):**
- **Agent C**: GDI boot-path audit + CreateCompatibleBitmap/GetStockObject fill
  (`bridge_gdi.cpp`) — STILL RUNNING.
- Orchestrator: working tree commits after all three report; then TRIANGLE GATE
  (Sprint-2 item 21) is next. Master prompt renamed to
  `TIRAMISU-MASTER-PROMPT.md` (commit 9e259c6). Re-check `git diff --stat`
  before starting anything.

**LANDED (same orchestrated wave, 2026-11-15, all verified offline):**
- **BUG-1 FIXED** (`bridge_d3d12.cpp`): CommandQueue vtbl rebuilt 19 -> 13
  entries in exact ABI order — ExecuteCommandLists at slot 8 (was a no-op
  UpdateTile at slot 8, exec at 10), Signal 9, Wait 10, ts-freq 11,
  clock-cal 12. ECL now records submitted lists (TRIANGLE GATE-ready).
  static_assert(13) + runtime `d3d12_vtbl_selfcheck()` fail-named.
- **BUG-6 FIXED**: Device QI chain extended to ID3D12Device5-9 (SDK-verbatim
  GUIDs; the master prompt's example Device5 GUID was wrong, headers win).
- **Sprint-1 1B/1C LANDED** (`bridge_kernel32.cpp`): 9 Interlocked* exports
  (fixed-width i32/i64, correct new/old-value semantics) + RtlCopy/Move/
  Zero/FillMemory, registered on kernel32+kernelbase, Rtl* merged into ntdll.
- **Sprint-1 1D VERIFIED already-registered** (GetSystemInfo/NativeSystemInfo/
  GetEnvironmentVariableW/VirtualAllocEx/VirtualFreeEx were in the merged
  table; the master prompt's gap list was stale here).
- **Sprint-1 1J LANDED** (`bridge_res.cpp` NEW): PE resource directory walk —
  FindResourceW/A/ExW, SizeofResource, LoadResource, LockResource (returns
  real mapped-image pointer), Unlock/FreeResource, EnumResourceNamesW,
  LoadStringW (real string-table format). Section-relative offset rebasing
  measured the hard way (first walk SIGSEGV'd treating child offsets as
  image-relative). Registered on kernel32; wired into host `main.cpp` boot +
  `build-host.sh` + `build-test.sh` suite 35.
- **Test evidence**: `test_d3d12` 24/24 ALL PASS (new), `test_wave1` 112/112
  (28 new), `test_res` 20/20 (new; real guest PE with hand-built .rsrc via
  `tools/build_guest_res.sh` + `tools/gen_guest_res.py`), `test_wave2` ALL
  PASS after slot-constant update (10->8, 14->9), test_engine/test_stage5/
  test_libload ALL PASS, **RACCOIN re-booted `1 booted, 0 failed`** with the
  whole wave linked (log `/tmp/opencode/raccoin-reswave.log`).

---

## W0 — hygiene first (fast, no design, high value)

1. **Commit the WIP as v0.35** (EH machinery + 9 new bridges + wave1 +
   UNITYMAIN2 IAT probe). ~27.8k insertions across 133 paths uncommitted
   after the RACCOIN boot — the single biggest risk to the project.
2. **gdb discipline:** never `gdb -batch -ex "commands 1" ...` — gdb reads the
   command-list body from the tty, wedges at the `>` prompt, and the guest never
   starts (that is exactly today's PID-301706 hang: no child process, stdin on
   `/dev/pts/179`). Use `gdb -batch -x script.gdb`.
3. **Ladder instrumentation fixes** (`tests/test_corpus_run.cpp observe()`):
   - D3D11 marker must also accept `"FL contract ->"` — today a real device
     acquisition reports `-` (false negative, hides progress).
   - UNITYMAIN2 marker must match only a *call* log line, not
     `"bind UnityPlayer.dll!UnityMain2"` (false positive).
4. **Adopt RACCOIN into the corpus:** copy to `corpus-local/raccoin/`
   (gitignored), add to `tools/corpus-run.sh` STAGE 7 EXES, so its depth is
   gate-enforced instead of ad-hoc.
5. `tools/audit_boot.py`: `'main'` → `'__main__'`.

## W1 — the EH wall — **BROKEN THROUGH (2026-11-15, ehfix9–19; DONE for C++ EH)**

Was: "the guest throws and we cannot walk/unwind". Now: faithful x64 SEH on
the `.pdata` walk is IMPLEMENTED and gated —
`bridge_kernel32.cpp`: `trt_RaiseException`, `trt_RtlUnwindEx`,
`call_pe_eh_handler`, `trt_RtlVirtualUnwind`, VEH pass-1, PLUS the measured
hard parts: pass-2 funclet resume records with geometric stale-purge and
per-guest-call retirement, and the host→guest boundary-escape swallow
(UnityPlayer wrapper-catch). Acceptance evidence: RACCOIN + Go Next boot 1/0
with the 7/7 ladder; `tests/test_guest_eh.cpp` ALL PASS including
funclet-rethrow and boundary-escape fixtures. What remains open below is the
scoping record; the wall itself is down.

1. **`RtlVirtualUnwind` decode fidelity** — `UNWIND_INFO` (Version/Flags/
   SizeOfProlog/CountOfCodes/Frames), all `UWOP_*` opcodes with real prolog
   mimicry (push of RBP, big/small stack allocs, save XMM, `UWOP_SET_FPREG`),
   `UNW_FLAG_CHAININFO` chained entries, personality + LSDA extraction, the
   `Flags & UNW_FLAG_EHANDLER|UHANDLER` cases, epilogue-boundary cases.
2. **Phase 1 (search):** walk frames from the faulting RIP through `.pdata`,
   call each personality with a correct `EXCEPTION_RECORD` +
   `DispatcherContext` (FunctionEntry/EstablisherFrame/ContextRecord/
   HandlerData=LSDA). Honor CONTINUE_SEARCH / CONTINUE_EXECUTION / HANDLER.
   VEH first-chance pass already exists — keep it ahead of the frame walk.
3. **Phase 2 (unwind):** `RtlUnwindEx` walks from the established frame to the
   target, runs every frame's unwind handlers/funclets (C++ destructors!), and
   restores the CONTEXT at the target rip. `call_pe_eh_handler` already pins
   the return PC so unwinding crosses the ELF host boundary — keep that.
4. **The ugly cases the logs already hit:** `flags=1` (NONCONTINUABLE),
   exceptions raised *during* unwinding, `RaiseException` from a guest worker
   thread (per-thread TEB — just fixed, keep its test), and the second
   exception with a null caller frame (unhandled → real termination path).

**Donors (license-checked):**

| Donor | License | Use |
|---|---|---|
| LLVM libunwind `Unwind-seh.cpp` + `UnwindCursor.hpp` | Apache-2.0 w/ LLVM exception | complete SEH-over-.pdata machinery incl. personality dispatch. Vendor with NOTICE, or port semantics line-by-line into MIT code — **license decision needed** (Apache patent grant + NOTICE retention) |
| Wine `dlls/ntdll/signal_x86_64.c` + unwinding | LGPL 2.1 | semantics only (repo rule: no vendoring) |
| MSDN "x64 exception handling" internals doc | free | the spec for UNWIND_INFO/UWOP decoding |

**Do NOT implement** `__CxxFrameHandler3/4` — measured absent from every import
table; the guest's statically-linked CRT provides its own personality. Implement
only NT primitives, faithfully.

**Test:** `tests/test_guest_eh.cpp` — clang-built freestanding guest exe with:
nested try/catch (C++), throw across guest→bridge→guest, catch-type matching +
rethrow, destructor unwinding during phase 2, SEH `__try/__except` AV handler,
raise-inside-funclet. All through the real IAT path. This is the test that
would have caught today's fault.

**Acceptance (UPDATED 2026-11-15):** ~~RACCOIN ladder prints `UNITYMAIN2
CALLED` (fixed marker) + `PUMP ENTERED`~~ → **DONE: full ladder 7/7 REACHED
(UNITYMAIN2 via naked-asm IAT probe, ehfix19), RACCOIN + Go Next demo.exe both
boot 1/0, all repo gates green.** Remaining for "any game": x86-32 (PE32)
loader + translator ladder.

## W2 — measured bridge gaps (small; do alongside W1)

1. **`GetWriteWatch` / `ResetWriteWatch`** — the hidden IL2CPP requirement.
   il2cpp's GC is **Boehm-Demers-Weiser (bdwgc)**, incremental by default in
   modern Unity. bdwgc *probes* at runtime:
   `GetProcAddress(kernel32, "GetWriteWatch")` + a
   `VirtualAlloc(NULL, page, MEM_WRITE_WATCH|MEM_RESERVE, PAGE_READWRITE)`
   probe call — GWW_VDB is enabled only if BOTH succeed; otherwise it falls
   back to MPROTECT_VDB (write-protect + fault) or DEFAULT_VDB (all pages
   dirty: correct, but incremental GC off → frame hitches).
   - **DONE 2026-11-15 (honest, dual-surface):** `VirtualAlloc` accepts
     `MEM_WRITE_WATCH` as a request modifier (real Windows semantics); on the
     console `_WIN32` passthrough to the real kernel32 GWW; on Linux gate,
     soft-dirty emulation via `/proc/self/pagemap` gated behind a one-time
     liveness probe (bit 55, and bit 63 present required — this 6.14 kernel
     marks fresh absent pages dirty; a dead bit or clear_refs wipe would
     silently lie "clean" to a GC = heap corruption, so it fails named →
     bdwgc falls back to MPROTECT_VDB). Known limitation: Linux
     `ResetWriteWatch` clear_refs "4" is process-wide (console: real
     per-range API). Gate: `test_wave1` Wave 6G (GC probe exact-shape,
     2-page dirty report, reset-clean).
2. **`dwmapi.dll`** (UnityPlayer: `DwmGetWindowAttribute`,
   `DwmSetWindowAttribute`) — honest record / fail-visible.
3. After W2: run the **C++-resolver audit** (`tests/audit_boot.cpp`) over all
   four RACCOIN PEs. Expect only game-dir exports outside builtin.

## W3 — console M0 + the W^X truth (research-backed expectations)

Research (xbox-uwp-lab `devmode-limitations.md`, quoting dosbox-pure-uwp /
xenia-canary-uwp / PPSSPP source): **UWP AppContainers enforce strict W^X** —
plain `VirtualAlloc(PAGE_EXECUTE_READWRITE)` fails; the `...FromApp` family
works; PPSSPP's `PlatformIsWXExclusive()` returns TRUE for UWP; the RW→RX flip
is mandatory, not defensive. (XBSX2 is the unverified outlier using
`codeGeneration`+`runFullTrust` with plain RWX.)

The current WIP *allows* RWX in `trt_VirtualAlloc`/`uwp_protect`. Keep it, and
let the already-coded probe decide (`main.cpp`: `probe: jit RWX alloc ->
ALLOWED/DENIED`, W→RX transition, DLL presence). Outcome table:

| Probe result | Consequence |
|---|---|
| RWX DENIED + W→RX ALLOWED (expected) | two-phase shim: guest RWX requests become RW + auto-protect-RX on `FlushInstructionCache`; patch mono/Boehm alloc at IAT level (we own the loader) |
| RWX ALLOWED | keep as-is (trivial path) |
| both DENIED | Mono/JIT games (G3) blocked on console — report honestly, do not fake |

**M0 order (from the contract):** deploy signed MSIX → boot on TV → swapchain
HRESULT 0 + selftest `0x6494cb0d` → screenshot → pull `tiramisu.log` →
phone-transfer round trip → **then** the JIT probe → **then** RACCOIN boot.
No console claim without a TV screenshot + the log pull.

## W4 — after first frame (the game loop, per master prompt G2/G4/G5)

- Pump: host-driven message pump at vsync, `Present(1)` pacing, FPS in log.
- Audio: FMOD→WASAPI (bridge exists, offline test green) — prove ≥5s of real
  game PCM in the log.
- Input: XInput (bridged) + keyboard; gamepad-as-mouse fallback.
- Save: VFS `C:\` writes — verify RACCOIN's save lands and round-trips.
- Cutscenes: OrTMM ships `binkw32.dll` (in corpus); Unity video = mfplat.
  Fail-visible skip first; build mfplat/IMFSourceReader only when a game's
  boot blocks on it.
- `discord_game_sdk.dll` (in OrTMM corpus): loads as game-shipped DLL; expect a
  named handshake failure and continue.
- `UnityCrashHandler64.exe`: `CreateProcessW` is console-denied — keep the
  named denial; ensure the guest treats it as non-fatal.

## W5 — gonna be needed next (dependency order + triggers)

| # | Item | Status | Trigger to start |
|---|---|---|---|
| 1 | Streaming/windowed `MapViewOfFile`, unmap reclamation, >64 maps | **IN FLIGHT** (test_wave1.cpp) | — |
| 2 | `DispatcherQueue` passthrough | **DONE 2026-11-15** (offline DQC/DQ factories + QI in bridge_winrt.cpp; test_console_pack §9) | UnityPlayer calls it early; on console combase is REAL — passthrough first |
| 3 | GetWriteWatch honest impl (W2) | **DONE 2026-11-15** (dual-surface honest: console passthrough / Linux soft-dirty+probe; test_wave1 Wave 6G) | frame hitches / GC assert in log |
| 4 | IOCP/OVERLAPPED async + dstorage | dstorage DllMain already runs | streaming asserts |
| 5 | Memory ceiling (5 GB game budget) | partly in flight (streaming maps) | any real game OOM |
| 6 | Perf: pump/present pacing, guest-on-render-thread | session-2 Wave-1 plan | first-frame stutter |
| 7 | D3D12 | not started | an engine that refuses D3D11 (RACCOIN negotiated D3D11 fine — UE-class is the trigger) |
| 8 | x86-32 (G8) | **license decision pending** | a real 32-bit target chosen |
| 9 | Steam depot downloader | `tools/steam_depot.py` exists (auth-only) | sideloading real GB-sized games |

## NOT needed yet (scope discipline — cut from any plan)

- Anti-cheat / DRM bypass — contract rule, always.
- OpenGL/gl4es — no corpus demand.
- Steam client emulation beyond auth — rule.

---

## The distance, honestly

- **Offline (Linux gate):** a real Unity IL2CPP Steam game boots through engine
  + IL2CPP init, window up, D3D11 device acquired — **one wall away from
  UnityMain2 + the pump: our own SEH unwinder.**
- **Console (the actual goal):** G1 has never run; G3's W^X answer is
  unmeasured; audio/input/save unproven on hardware. **3–4 unmeasured walls.**
  The probe sequence in W3 converts all of them from speculation to measurement
  in one deploy session.
