# Tiramisu UWP — Bridge surface (the map)

Source of truth for per-DLL routing. Derived from xboxnative's console
routing evidence + Wine spec analysis; update after every console run.

## v0 routing table (as built)

| DLL | Route | Rationale |
|---|---|---|
| kernel32.dll | BUILTIN | 14-function core on UWP backend (heap/memory/sync/clock/error/TLS) |
| kernelbase.dll | BUILTIN | same table; Wine-style forwarders |
| api-ms-win-core-rtlsupport-l1-1-0.dll | STUB | console resolves to system; stub until measured |
| user32.dll | BUILTIN | CoreWindow message pump (M1, proven by RACCOIN / God of War / Celeste console boots) |
| gdi32.dll | BUILTIN (M7 corpus) | GDI object handle table: CreateFontIndirectW (LOGFONTW record), SelectObject/DeleteObject, GetStockObject, state setters; DCs shared with the user32 bridge. Real gdi32 is NOT loadable in a console AppContainer — the corpus test models that and games must bind the BUILTIN surface |
| comctl32.dll | BUILTIN (M7 corpus) | InitCommonControls/Ex + ordinal #380 (the x64 export OrTMM imports by bare ordinal); the requested dwICC flags are logged |
| winmm.dll | BUILTIN | timeGetTime/timeBeginPeriod/timeGetDevCaps (bridge_winmm); legacy waveOut/midi/joy probes answer "no devices", MCI fails visibly (263) |
| ole32.dll | BUILTIN | M7 COM boot (bridge_ole); console UWP ole32 lacks the full PC surface |
| d3d11.dll | SYSTEM | real on console (DX11 FL 11.0 both modes) |
| dxgi.dll | SYSTEM | real on console |
| ws2_32.dll | SYSTEM | real on console |
| bcrypt.dll | SYSTEM | real on console |

## The numbers that rank the work (xboxnative, Series S, Megabonk closure)

- 628 direct+delay DLL/symbol pairs for a Unity 64-bit game; console already
  satisfies ~440 of them; the 188-symbol gap is 56% user32/win32u-bound.
- 32-bit native compat mode: measured DEAD on console (0xc0000005 on
  `ljmp 0x23`, CS stays 0x33; packaged WoW64 DLL probes all error 126).
- D3D9/OpenGL: absent from UWP; `bridge_d3d9` / `bridge_d3d8` / `bridge_ddraw` translate
to D3D11 and `bridge_opengl32` (gl4es-derived) does the same for GL. DX11 games use
the real device passthrough; DX12 command recording exists but does not execute
(see `docs/04-support-roadmap.md` W5).

## Import audit before any game attempt (skill: pc-game-import-audit)

A PC exe dies at LOAD time if any statically-imported DLL fails to resolve —
before our first line runs. For every candidate game: true import table ->
{shipped | system-routed | bridge | redist-gap}. The redist gap
(msvcp140/vcruntime140/d3dcompiler_47/xinput1_4) is the hidden boot-killer:
the game's own `_CommonRedist` installers cannot run on console; those must
ship as bridge-builtins or forwarders.

## Delay imports (IMPLEMENTED)

Both the 64-bit and PE32 paths process the delay-load directory: eager bind,
4-byte guest thunks, and unresolvable symbols become named traps that log on
first call instead of failing the load (`pe_loader.cpp` step 8). Measured on
the corpus: 6 of 53 PEs carry delay descriptors (56 symbols) —
`UnityPlayer.dll` delay-loads `d3d12.dll`, `binkw32.dll` delay-loads Miles
(`mss32.dll`). `tools/pe_readiness.py` reports the count per file.
