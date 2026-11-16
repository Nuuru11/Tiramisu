# Tiramisu — Game Compatibility Database

> Living document. Every entry is measured on the **offline Linux gate** unless
> marked `[XBOX]`. Status reflects the deepest boot-depth ladder rung reached.
>
> Ladder rungs: `CRT` → `WINMAIN` → `UNITYMAIN2` → `WINDOW` → `D3D11` →
> `PUMP` → `QUIT` → `EXIT(0)`. A game at `PUMP`+ is considered "boots".

## Tier definitions (Proton-like spectrum)

| Tier | Meaning |
|------|---------|
| **S** | Boots, runs sustained, playable with controller |
| **A** | Boots to pump, exits cleanly, untested for sustained play |
| **B** | Boots to window/D3D, dies during init (named gap) |
| **C** | Loads PE + DllMains, fails before window |
| **X** | Permanent exclusion (anti-cheat, Denuvo, W^X-denied JIT, OOM) |

## Verified games

### Ultra Pool (Godot 4.3 + GodotSteam)
- **Status:** Tier A (offline), unverified on Xbox
- **Engine:** Godot 4.3, Forward Mobile renderer, Vulkan→D3D11
- **Depth:** CRT ✓ WINMAIN ✓ WINDOW ✓ D3D11 ✓ PUMP ✓ QUIT ✓ EXIT(0) ✓
- **Notes:** Cleanest boot. Watchdog fires at 10s (expected). WM_CLOSE triggers
  Godot's own quit chain. Vulkan Forward Mobile initializes on the Tiramisu ICD.
  All shaders compile (maxFragmentInputComponents=128).
- **Gaps:** None known. First candidate for sustained-play testing.

### RACCOIN (Unity 6 IL2CPP, Steam)
- **Status:** Tier B (offline, mock-d3d11) / Tier C (plain --exe)
- **Engine:** Unity 6000.3.0f1, IL2CPP, DirectX 11
- **Depth (mock-d3d11):** CRT ✓ WINMAIN ✓ UNITYMAIN2 ✓ WINDOW ✓ D3D11 ✓ PUMP ✓ QUIT ✓ EXIT(0) ✓
- **Depth (plain --exe):** CRT ✓ WINMAIN ✓ UNITYMAIN2 ✓ WINDOW ✓ PUMP ✓ QUIT ✓ (dies in CrashReporter exception path during teardown)
- **Notes:** Runs healthy for 10s under mock-d3d11. Plain-GPU path dies in the
  Unity CrashReporter insights-crash-report.json write (CreateFileW fails on the
  CrashReporter dir — guest never calls CreateDirectoryW). Named gap.
- **Gaps:** CrashReporter shutdown exception attribution; insights JSON write path.

## Corpus load matrix (DLLs that map + seal cleanly)
| PE | Status |
|----|--------|
| baselib.dll | DllMain(PROCESS_ATTACH) → 0x1 |
| libgodotsteam.windows.template_release.x86_64.dll | DllMain → 0x1 |
| steam_api64.dll | Maps + seals (was FAIL IMPORT; closed) |
| UnityPlayer.dll | DllMain → 0x1 |
| GameAssembly.dll (96MB IL2CPP) | DllMain → 0x1 |
| RACCOIN.exe | Boots (see above) |
| Ultra Pool game.exe | Boots (see above) |

## Permanent exclusions
| Category | Examples | Reason |
|----------|----------|--------|
| Anti-cheat | EAC, BattlEye, Vanguard | Contract rule — never bypass |
| Denuvo multiplayer | Various | DRM bypass prohibited |
| JIT without W^X | Mono/IL2CPP (if W^X denied) | OS policy, not code |
| Memory pressure | Cyberpunk-class (>5GB) | Console RAM cap |
| 32-bit x86 | Legacy titles | License decision pending |
| OpenGL-only | GL-only engines | Cut from scope (no corpus demand) |

## How to add a game
1. Drop the game folder into `corpus-local/<name>/`
2. Run: `./tests/out/test_corpus_run --exe corpus-local/<name>/<exe>.exe`
3. Read the boot-depth ladder in the log
4. File a gap report naming the exact bridge/import/EH failure
5. Add an entry here with the measured depth

## Testing on Xbox
Requires Dev Mode credentials (`DP_HOST`, `DP_PASS`). See `TIRAMISU-MASTER-PROMPT.md`
for the deploy sequence. Games verified offline are Tier A candidates for M0.