# Tiramisu — support roadmap (measured)

> Written 2026-11-21 from tool runs on THIS tree (`tools/pe_readiness.py`,
> `tests/test_corpus` over all 53 corpus PEs, bridge inventory). Every number
> below is reproducible with the command in its section. Nothing here is from
> memory.
>
> Read `TIRAMISU-MASTER-PROMPT.md` (the contract) first. This file answers one
> question: **what stands between "a few titles boot" and "any PC game runs"**,
> ranked by how much of the PC library each wall covers.

---

## 1. Where support actually stands (measured 2026-11-21)

```
$ mapfile -t PES < <(find corpus-local -type f | grep -iE '\.(exe|dll)$'); \
  ./tests/out/test_corpus "${PES[@]}" | tail -1
CORPUS: 53 ok, 0 failed, 0 skipped          # 0 unresolved, 0 stub-routed imports
$ python3 tools/pe_readiness.py "${PES[@]}" | tail -1
SUMMARY: 53 PEs | 3 PE32 / 50 PE32+ | 6 with delay imports (56 delay symbols)
```

- **Load/bind is complete** for the corpus: every imported symbol in all 53
  PEs resolves to SYSTEM, BUILTIN or a game-dir export. There is no remaining
  `FAIL IMPORT` class of gap in the corpus, and no PE is refused.
- **Delay imports are processed** (both 64-bit and PE32 descriptors): eager
  bind, 4-byte guest thunks, unresolvable symbols become named traps instead
  of failing the load. Measured: 6 of 53 corpus PEs carry delay descriptors
  (56 symbols) — `UnityPlayer.dll` delay-loads `d3d12.dll`, `binkw32.dll`
  delay-loads Miles (`mss32.dll`).
- **PE32 is mapped and bound, but not executed.** 3 of 53 corpus PEs are
  32-bit (`SDL2.dll`, `binkw32.dll`, `discord_game_sdk.dll`): they map, seal,
  bind and their DllMains are collected, but `images.cpp` skips 32-bit code
  (`M7 Phase 1: no 32-bit code runs natively`).
- 56 bridge translation units, 56 offline suites, 49 gate stages.

What that means: the classic emulator-project failure mode ("it dies at load")
is gone for this corpus. Everything left is **capability depth**, and the
capability gaps are now the product.

---

## 2. The walls, ranked by library share

Each row is a class of games that today reaches the window and then cannot
proceed. Ordered by how many shipped PC titles each one covers.

### W1 — native x86-32 execution (largest wall by far)

Every 32-bit native game — most of the pre-2015 PC library, and most 32-bit
indies including D3D9-era titles — maps and binds but cannot run a single
32-bit instruction. Mono games already work (Celeste, via the embedded Mono
interpreter, no native x86 needed); native C/C++ 32-bit does not.

- **Measured**: `images.cpp` defers PE32 entry/TLS/DllMain to "the M7
  emulator"; console evidence in `docs/01-roadmap.md` M7 (WoW64 probes all
  fail, CS stays 0x33) rules out native 32-bit compat.
- **Path**: an x86 interpreter/JIT behind 32→64 thunks at the bridge
  boundary. Boxedwine's core is the known-works candidate; it is GPL-2, so
  the license decision (`docs/01-roadmap.md`) comes first, or a clean-room
  interpreter with Wine's semantics as reference only.
- **Acceptance**: a real 32-bit D3D9 title (Spacewar-class) reaches its
  window and presents frames through the existing D3D9 bridge; a 32-bit guest
  suite in `build-test.sh` runs CRT + Win32 calls through the thunks.

### W2 — audio content decoding (silent games)

The mixer is real (one XAudio2 2.8 master voice); what is missing is sample
decode for the containers games actually ship.

| Container | Legacy FMOD Ex (`bridge_fmod`) | FMOD Studio (`bridge_fmod_studio`) |
|---|---|---|
| WAV PCM | **real** | real |
| OGG Vorbis | **real** (stb_vorbis) | real (FSB5 → Ogg rebuild) |
| FSB5 Vorbis | silent, named | **real** (Celeste) |
| FSB5 FADPCM | silent, named | silent, named |
| MP3 | silent, named | silent, named |

- **Measured**: `bridge_fmod.cpp` classifies and names each container; the
  FMOD Studio path parses FSB5 sample tables and rebuilds Vorbis streams.
- **Next increment (cheap, high share)**: FADPCM decode + legacy-Ex FSB5
  reuse, so FMOD-Ex titles (thousands of indies) are audible.
- **Acceptance**: `test_fmod` and `test_fmod_studio` assert non-zero PCM for
  an FSB5-FADPCM fixture and an MP3 fixture, not a named-silent log.

### W3 — Wwise (zero coverage today)

`grep -ri wwise src/trt` returns nothing. Wwise (Audiokinetic) is the audio
middleware of a large share of AA/AAA PC titles; God of War's named gap is
"audio validation", which is this wall.

- **Path**: `AkSoundEngine.dll` is usually game-shipped, so the loader maps
  it; what it needs from us is the Win32/COM surface it calls plus
  `AkSoundEngine_*` real backends (`RenderAudio` → mixer). Start with the
  init + render + bank-load path and name everything else.
- **Acceptance**: a game shipping Wwise reaches "audio initialized, N banks
  loaded, samples submitted" in the log; silence is named, never faked.

### W4 — video playback

`bridge_mf.cpp` is an honest COM surface: `MFStartup` → S_OK, `IMFSourceReader`
hands out stream metadata and a clean EOS, but nothing decodes frames. Unity
cutscenes (`mfplat`) and Bink titles (OrTMM ships `binkw32.dll`) show black or
skip.

- **Path**: a real `IMFSourceReader` decode path over the host's decoder
  where one exists (D3D11 video processor), Bink failing named.
- **Acceptance**: a cutscene posts ≥30 real frames to a texture instead of
  EOS-with-no-data.

### W5 — D3D12 execution depth

`bridge_d3d12.cpp` has a real descriptor heap, real PSO validation and a
command-list **recording** surface (suite 39, 77 checks). D3D12 draws are
recorded, not executed. UE5-class titles and any engine that refuses D3D11
land here.

- **Acceptance**: a recorded frame becomes a D3D11-equivalent submission
  (or a named refusal) — measured by a triangle fixture, like the D3D12
  triangle gate does today for the recording path.

### W6 — platform/lifecycle surfaces (small, frequent)

- `mscoree.dll` (.NET Framework games, not Mono) — currently STUB.
- DStorage streaming for Unity 6 titles (`corpus-local/gonext`).
- `CreateDirectoryW`-less CrashReporter paths (RACCOIN's plain-GPU gap).
- Steam/EOS/GDK handshakes — auth-only by contract; no DRM bypass, ever.

### Excluded by contract (never work items)

Anti-cheat, Denuvo/Steam DRM circumvention, Valve/MS binary redistribution.
A title gated on those is Tier X in `COMPATIBILITY.md`, not a gap.

---

## 3. The detail layer — what makes this the most complete project of its type

Support breadth is won by measurement, not by bridge count. Four artifacts,
three of which exist:

1. **Per-PE readiness audit** (`tools/pe_readiness.py`, new): machine type,
   import/delay-import counts, symbols, bound-import and TLS presence, per
   file. This is how a candidate title is triaged before anyone writes code.
2. **Load/bind gate** (`tests/test_corpus` + `corpus-smoke.sh`): 53/53 with
   0 unresolved. Any regression is a named failure, not a silent skip.
3. **Boot-depth ladder** (`tests/test_corpus_run.cpp`): CRT → WINMAIN →
   UNITYMAIN2 → WINDOW → D3D11 → PUMP → QUIT → EXIT(0), per title, enforced
   in `corpus-run.sh`.
4. **Per-title compatibility rows** (`COMPATIBILITY.md`): tier, engine,
   ladder reached, and the exact named gap for anything below Tier S.

**DONE 2026-11-21 — symbol-level coverage classification + generated matrix:**

- `trt_named_fn` carries an `impl_class` (`TRT_IMPL_REAL` / `RECORDED` /
  `NAMED_SILENT`); `trt_module_mark_impl()` lets bridges override it and
  `trt_symbol_class()` resolves it through the same path as the loader.
  D3D12 is seeded as RECORDED; mfplat/mfreadwrite/mscoree/Steam as
  NAMED_SILENT; the honest-empty shell32/winmm answers are per-symbol.
- `test_corpus --coverage` prints one `COVERAGE <pe>: imports=N real=X
  recorded=Y named-silent=Z delay=N` line per PE, with a self-test that fails
  the gate if the seed table drifts. Delay-load descriptors are walked too
  (VA-style and RVA-style), so UnityPlayer's d3d12 and binkw32's Miles are
  counted just like static imports.
- `tools/support-matrix.sh` runs the corpus and writes
  `docs/support-matrix.md` from those lines — measured, never hand-written.

---

## 4. Ordered next work (each with a gate)

| # | Item | Why first | Gate |
|---|---|---|---|
| ~~1~~ | ~~Symbol-level implementation classes + per-title coverage report~~ | **DONE 2026-11-21** — see `docs/support-matrix.md` | `COVERAGE SELFTEST: OK` + generated matrix |
| 2 | FADPCM + legacy-Ex FSB5 decode (W2) | audible games for the largest middleware class | `test_fmod` asserts real PCM for FSB5-FADPCM |
| 3 | Wwise init/render/bank path (W3) | AAA audio class, GoW's named gap | fixture DLL call sequence reaches "samples submitted" |
| 4 | D3D12 recording → submission (W5) | UE5-class titles | triangle fixture executes, not just records |
| 5 | x86-32 interpreter decision + spike (W1) | largest library share; needs the license call | a 32-bit CRT-only guest runs through thunks |

Items 1–2 are offline-verifiable today. Item 5 is the only one that changes
the project's class, and it should not start before the license decision is
recorded in `THIRD-PARTY.md`.
