# FNA as Microsoft.Xna.Framework (Path B, Celeste)

FNA — the accuracy-focused XNA 4.0 reimplementation — built as the
strong-named XNA assemblies Celeste.exe references, so the game runs under
64-bit Mono without the real (mixed-mode, 32-bit-only) XNA Framework.

## What lives here

| File | What it is |
|---|---|
| `build-fna.sh` | The build: core + forwarders + binding probe. Idempotent; `--force` rebuilds. |
| `graphics-types.txt` | 95 top-level public types of real `Microsoft.Xna.Framework.Graphics` 4.0.0.0 |
| `game-types.txt` | 16 top-level public types of real `Microsoft.Xna.Framework.Game` 4.0.0.0 (minus `GamerServicesComponent`) |
| `AssemblyInfo.core.cs` | Substitute AssemblyInfo: version 4.0.0.0 (FNA's own is 26.11.0.0) |
| `AssemblyInfo.graphics.cs`, `AssemblyInfo.game.cs` | Forwarder AssemblyInfos, version 4.0.0.0 |
| `gen/` | Generated forwarder sources + binding probe (build artifacts) |
| `out/` | **Kept outputs**: the three DLLs + `Microsoft.Xna.Framework.dll.config` |
| `LICENSE-MS-PL` | FNA's license (Microsoft Public License), copied from upstream |

Source clone: `../fna-src/` (FNA master `852e4af`, the commit the Path B
spike tested). The clone is never modified by the build.

## Recipe (proven by the Path B spike, 2026-11-20)

1. **Core**: upstream `make release` via mcs (classic .NET 4.x build,
   ~342 files + 8 embedded stock-effect `.fxb` resources), retargeted to
   `-out:Microsoft.Xna.Framework.dll` (mcs names the assembly after the
   output file) with the substitute AssemblyInfo above. The netstandard2.0
   build does NOT run under mineplex Mono 6.8.0.105 (no netstandard facade).
2. **Forwarders**: `Microsoft.Xna.Framework.Graphics.dll` (95 types) and
   `Microsoft.Xna.Framework.Game.dll` (16 types) contain only
   `[assembly: TypeForwardedTo]` entries pointing at the FNA core. Type
   lists were enumerated from the real XNA 4.0 GAC assemblies (from
   `xnafx40_redist.msi`) with `monodis --typedef`, filtered to top-level
   public types (`(flags & 7) == 1`).
3. **Binding**: Mono 6.8.0.105 binds the unsigned substitutes to Celeste's
   strong-named references (name + 4.0.0.0 + MS public key token) with no
   strong-name complaint, and ignores Celeste.exe's ILONLY+32BITREQUIRED
   flags. The build script's probe verifies this on every build.

## Exclusions (deliberate)

- `GamerServicesComponent` is NOT forwarded: FNA has no GamerServices
  namespace, and Celeste has 0 references to it. A future game that
  references it needs a real GamerServices type first.
- Nested public types (collection `Enumerator`s etc.) forward with their
  declaring types; C++/CLI artifacts (`<CrtImplementationDetails>`) and
  the real Game.dll's Win32 interop structs (`NativeMethods/*`) are not
  part of the XNA API surface and are not forwarded.
- The other seven XNA assemblies (`.GamerServices`, `.Net`, `.Storage`,
  `.Video`, `.Xact`, `.Avatar`, `.Input.Touch`) are not built: Celeste
  references only the three built here.

## Runtime notes

- FNA's platform backend is chosen by env var: `FNA_PLATFORM_BACKEND=SDL2`
  (default is SDL3, which is not installed on mineplex). The harness test
  sets it. The next step (Tiramisu_FNAPlatform backend) replaces SDL
  entirely on the console.
- The `.dll.config` maps `FNA3D`/`FAudio`/`SDL2`/`SDL3` to their Linux
  `.so.0` names; the harness puts the FNA3D/FAudio out dirs on
  `LD_LIBRARY_PATH`.

## Verification (spike values, 2026-11-20)

- Core DLL ~1,168,896 bytes (exact size varies per build: fresh MVID).
- Core assembly refs: only `mscorlib` + `System`, both 4.0.0.0.
- All three assemblies report Name + Version 4.0.0.0 via `monodis --assembly`.
