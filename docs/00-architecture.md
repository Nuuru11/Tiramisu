# Tiramisu UWP — Architecture

## The one decision that shapes everything

A UWP AppContainer process cannot spawn child processes, cannot load desktop
images, and gets 5 GB RAM max (as a Game package). Therefore a game can never
be a real process — **it must be mapped into Tiramisu's own process**, and
every Win32 thing it touches must be served by code inside that same process.
This is the xboxnative-proven model; full-trust entry points remain unproven
on console and are not our path.

## Layers

```
+--------------------------------------------------------------+
| UWP host (tiramisu.exe, AppContainer, WINAPI_FAMILY_APP)     |
|  CoreWindow + D3D11 FLIP_SEQUENTIAL + XInput + LocalState log|
+--------------------------------------------------------------+
|  trt runtime (same process, plain C/C++)                     |
|   loader: validate -> alloc -> sections -> relocs -> imports |
|           -> TLS -> seal (W^X) -> entry()                    |
|   module table: name -> route -> {system | builtin | stub}   |
|   bridges: kernel32 (builtin), user32 (skeleton), fs, vreg   |
+--------------------------------------------------------------+
|  console OS: real d3d11/dxgi/ws2_32/bcrypt/kernelbase DLLs   |
+--------------------------------------------------------------+
```

### Loader (src/trt/pe_loader.cpp)
Windows-header-free on purpose: the identical core compiles for UWP (clang-cl,
Win32 backend) and for the Linux test (mmap backend). Every stage is
bounds-checked and fails with a named error (`TRT_ERR_*`) — a malformed game
PE must never crash the host, because the host IS the app on the TV.

W^X discipline: everything maps RW; `trt_seal_image()` flips exec sections to
RX via `VirtualProtectFromApp`. Zero RWX at any instant. This matches what the
console allows (W^X via the *FromApp APIs is the proven xboxnative lane).

### Module table (src/trt/module_table.cpp)
Routing is DATA, not code — per-DLL policy set at boot:
- **SYSTEM** — the console ships the real DLL (d3d11, dxgi, ws2_32, bcrypt,
  kernelbase, ole32...). Bound via `LoadLibraryW` + `GetProcAddress`. This is
  the load-bearing shortcut xboxnative proved on hardware.
- **BUILTIN** — Tiramisu implements it to Wine's observable semantics.
  v0: kernel32 core (heap, memory, sync, clock, error, TLS basics).
- **STUB** — exports resolve, calls log, return 0. Fail-visible: every STUB
  hit in the log is roadmap work, never silent breakage.

### VFS (src/trt/vfs.cpp)
Guest paths are UTF-16 Windows paths; mounts translate: `C:\` ->
`LocalState\drive_c` (writable), `Z:\` -> package install dir (read-only game
files). Per-game overrides ship as data files later, not code changes.

### Host (src/host)
The C++/WinRT shape proven to install and run on console: single
`implements<IFrameworkViewSource, IFrameworkView>`, `CoreApplication::Run`
from `wWinMain`, manifest `EntryPoint` string non-fatal, flip-model swapchain
on the CoreWindow (no exclusive fullscreen exists; ~5-10% compositor cost).

### Memory backend
`VirtualAllocFromApp` / `VirtualProtectFromApp` on UWP; mmap/mprotect in the
Linux test. The loader sees only the `trt_backend` vtable.

## What the offline test proves (and what it cannot)

`tools/build-test.sh` builds a real PE64 guest (clang cross, freestanding,
imports against `tiramisu_host.dll`) and executes it on the Linux host
through the same loader core: both base modes, reloc correctness verified
semantically (guest reads a pointer table; wrong relocs = wrong checksum),
imports bound to host functions, W^X seal applied, entry returns a checksum
matching the host model.

It cannot prove: console API availability (CreateProcess refusal, AppContainer
memory quirks), D3D11 swapchain behavior under the Xbox compositor, XInput
through XINPUTUAP. Those are the console run's job — the deploy command is in
README.
