# PC Games on Xbox

Tiramisu runs PC games natively on Xbox Series X|S in Dev Mode. No CPU emulation for 64-bit games — the console is x64, so the game runs at native speed while Tiramisu translates the Windows API calls around it.

## What works right now

- **RACCOIN** — fully playable
- **Celeste** — fully playable, in color
- **Sekiro** — in progress (deep into loading)
- **Ultrapool** — in progress (first frame rendered)

## What a game needs to run

- x64 Windows .exe (32-bit support is in progress)
- DirectX 11 — D3D9 and D3D8 get translated up; D3D12 is not there yet
- No Denuvo, kernel DRM, or driver-level anti-cheat
- Under ~5GB of memory (the Dev Mode app cap)
- A Dev Mode Xbox Series S/X and your own legit copy of the game
- You copy the game files onto the Xbox yourself — there is no Steam download (Steam just handles the login check)

Every game uses a different set of Windows APIs, so compatibility is game-by-game right now — each game tested grows the bridge.

## Start here

- [README](../README.md) — what Tiramisu is and how it works
- [Compatibility](../COMPATIBILITY.md) — what boots and what does not
- [Releases](https://github.com/xanstomper/Tiramisu/releases) — download the latest build
