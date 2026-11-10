# Tiramisu UWP — Bridge surface (the map)

Source of truth for per-DLL routing. Derived from xboxnative's console
routing evidence + Wine spec analysis; update after every console run.

## v0 routing table (as built)

| DLL | Route | Rationale |
|---|---|---|
| kernel32.dll | BUILTIN | 14-function core on UWP backend (heap/memory/sync/clock/error/TLS) |
| kernelbase.dll | BUILTIN | same table; Wine-style forwarders |
| api-ms-win-core-rtlsupport-l1-1-0.dll | STUB | console resolves to system; stub until measured |
| user32.dll | STUB | M1 — the keystone (message pump on CoreWindow) |
| gdi32.dll | STUB | after user32; CoreWindow-backed to Wine semantics |
| winmm.dll | STUB | donor candidate once user32 exists |
| ole32.dll | SYSTEM | real on console |
| d3d11.dll | SYSTEM | real on console (DX11 FL 11.0 both modes) |
| dxgi.dll | SYSTEM | real on console |
| ws2_32.dll | SYSTEM | real on console |
| bcrypt.dll | SYSTEM | real on console |

## The numbers that rank the work (xboxnative, Series S, Megabonk closure)

- 628 direct+delay DLL/symbol pairs for a Unity 64-bit game; console already
  satisfies ~440 of them; the 188-symbol gap is 56% user32/win32u-bound.
- 32-bit native compat mode: measured DEAD on console (0xc0000005 on
  `ljmp 0x23`, CS stays 0x33; packaged WoW64 DLL probes all error 126).
- D3D9/OpenGL: absent from UWP; any D3D9 title needs our own translation
  layer (M5). DX11/DX12 games are unaffected.

## Import audit before any game attempt (skill: pc-game-import-audit)

A PC exe dies at LOAD time if any statically-imported DLL fails to resolve —
before our first line runs. For every candidate game: true import table ->
{shipped | system-routed | bridge | redist-gap}. The redist gap
(msvcp140/vcruntime140/d3dcompiler_47/xinput1_4) is the hidden boot-killer:
the game's own `_CommonRedist` installers cannot run on console; those must
ship as bridge-builtins or forwarders.

## Delay imports (v0 limitation)

The loader detects a delay-import directory and logs a NOTE (not processed in
v0). Real games use delay-loads constantly; this is an M1-adjacent item, small
but blocking: such guests will load, then fail visibly when a delay-loaded
DLL is first touched.
