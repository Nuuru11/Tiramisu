# Spike-parity Steam / FMOD stubs (Path B step 1)

Celeste.exe P/Invokes 489 FMOD/FMOD Studio functions and Steamworks.NET
P/Invokes 872 CSteamworks/sdkencryptedappticket functions. Offline, under
Mono, those bind to the two stub libraries built here:

| Library | Covers | Exports |
|---|---|---|
| `out/libfmod-stub.so` | `fmod` (320) + `fmodstudio` (169) | 489 |
| `out/libcsteamworks-stub.so` | `CSteamworks` (864) + `sdkencryptedappticket` (8) | 872 |

The embedding (`src/trt/mono_host.cpp`) installs them with
`mono_dllmap_insert` — no game file is touched and no `.dll.config` in the
game folder is needed.

## These are stubs — documented, not disguised

- **No audio.** FMOD calls return `FMOD_OK` and fake handles; nothing
  plays. The game proceeds because every call "succeeds".
- **No Steam.** `Init`/`IsSteamRunning` report success; there is no auth,
  no callback pump, no network, no DRM interaction of any kind.
- **Next step replaces them** with real dllmaps to Tiramisu's 64-bit FMOD
  bridge (`src/trt/bridge_fmod.cpp`) and Steam bridge
  (`src/trt/bridge_steam.cpp`).

## How the stubs are built

- `celeste.implmap` / `steamworks.implmap`: `monodis --implmap` dumps of
  Celeste.exe / Steamworks.NET.dll (symbol-name metadata produced read-only;
  not game content). They are the source of truth for the entry lists.
- `gen-stubs.py`: generates one default stub per entry. Defaults are
  type-aware: `FMOD.RESULT` → 0 (OK), `bool` → true, `native int` → a fake
  non-NULL pointer (a zeroed buffer, so a string read yields `""`),
  numerics → 0, enums → 0. A generated stub is declared `f(void)` and
  ignores its arguments — ABI-safe on SysV x86-64 (caller cleans the
  stack; the return travels in RAX/XMM0).
- `*_overrides.c` (+ `overrides-*.txt`): hand-tuned functions where boot
  progress depends on the value. The generator verifies every override
  names a real implmap entry, and the build verifies the final `.so`
  exports cover every implmap entry.

## The boot-critical overrides (from the spike)

- **Steam**: `Init` and `IsSteamRunning` must return true or Celeste aborts
  with 'Steam not found!'. (Both would also be true under the generated
  bool default; they are overridden explicitly so the gate never depends
  on a default.)
- **FMOD**: `FMOD_Studio_EventDescription_GetPath` must write a real path
  and a plausible `retrieved` length — a zero retrieved crashes Celeste's
  `GetEventName` with count -1. Bank/event loading states report LOADED
  (a stub that forever reports UNLOADED would hang a load-wait poll).
- **Handles**: every `Create`/`Get*` that hands out a system/bank/event/
  bus/channel handle writes a fake non-NULL handle; a NULL handle would
  fail the wrapper's validity checks.
- **Identity**: SteamID `76561197960265729` (Tiramisu's offline placeholder),
  AppID `504230` (Celeste), language `english`.
- **Struct returns**: the three Steam Controller data functions return
  structs by value (2 / 16 / 40 bytes) and are hand-written with the
  correct SysV struct-return ABI (RAX / RAX:RDX / hidden sret pointer).

## Deliberate bool-default exceptions

The generated `bool` default is true, which is right for `IsValid` and the
Steam gates — except:
- `RestartAppIfNecessary` → **false**: true tells the game "Steam is
  relaunching you, exit now", and Celeste.Main returns silently before
  doing anything. (This was the first boot failure: the game exited
  cleanly with no output at all.)
- `ISteamGameServer_WasRestartRequested` → false (same "exit now"
  semantics for a server).
- `ISteamRemoteStorage_FileExists` / `FilePersisted` → false (no cloud
  files exist, so nothing is there to read back).
- `SteamEncryptedAppTicket_BUserIsVacBanned` → false (the fake player is
  not VAC banned).
