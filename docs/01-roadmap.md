# Tiramisu UWP — Roadmap

Each milestone is gated by evidence, not announcement. "Console pass" means:
log pulled from LocalState + TV screenshot, matching the milestone's
acceptance line. The rung ladder (cheap -> hard) mirrors xboxnative's
universal-boot ladder.

## M0 — skeleton on hardware (current)
- Offline loader test PASS (done). Offline input-bridge test PASS (done).
- Signed Tiramisu.msix produced and verified, now with the transfer server,
  D2D shelf UI, QR, miniz, qrcodegen (done).
- **Acceptance:** on console — host boots, swapchain HRESULT 0, selftest guest
  maps + seals + returns 0x6494cb0d in `tiramisu.log`, screenshot shows the
  shelf + QR. Phone test: open the URL, upload a small zip game (e.g. a
  freeware DX11 indie), hit LAUNCH.

## M1 — user32 message pump (the keystone)
Wine's spec files as the checklist: RegisterClassExW/CreateWindowExW as
window RECORDS first (no CoreWindow coupling), then GetMessageW/
PeekMessageW/PostMessageW/SendMessageW/TranslateMessage/DispatchMessageW
driven by the CoreWindow dispatcher.
- **Acceptance:** a test guest pumping >= 300 message iterations; a second
  guest drawing through the bridge to the real swapchain.

## M2 — input
GetAsyncKeyState/GetKeyState/GetKeyboardState from CoreWindow key events;
Raw Input registration (mouse usage 0x02, keyboard 0x06) — if WM_INPUT never
arrives on console, KBM needs a translation layer (console decides).
- **Acceptance:** controller A-press arrives as a real WM_ event in the guest.

## M3 — audio
WASAPI render endpoint on console; XAudio2 path for xaudio games (FAudio
donor candidate).
- **Acceptance:** non-zero samples in the log over >= 5 s.

## M4 — fs/registry completeness
CreateFileW/ReadFile/WriteFile/SetCurrentDirectory/GetEnvironmentStringsW on
the VFS; virtual registry hive in LocalState (Wine registry semantics).
- **Acceptance:** a guest that writes a save file via C:\ reads it back.

## M5 — D3D9 translation layer
UWP has NO D3D9 and no OpenGL (Microsoft feature-mapping doc). D3D9On12 is a
desktop component. Options: own small D3D9->D3D11 shim (fixed-function/SM3
subset first) — gates every older 32-bit title anyway.
- **Acceptance:** a D3D9 triangle test exe presents 300 frames, hr=0.

## M4.5 — the D3D11/DXGI bridge (DONE, v0.7)
Not in the original ladder, but it is the gate for every 64-bit D3D11 title
(rung 1) and it had to come before M3/M5:
- The guest's `D3D11CreateDevice` returns the HOST's device and context, so
  the guest's resources live in the device that owns the swapchain.
- The guest's DXGI factory is a shim: adapter enumeration forwards to the
  console's real factory, `CreateSwapChainForHwnd` returns the HOST's
  CoreWindow swapchain (the console has no window for the guest's fake
  HWND), everything else is named-failure.
- Evidence: `tests/test_d3d.cpp` (offline, fake host GPU, real SDK vtable
  slots) — the guest's whole frame lands on the host's objects.

## M6 — Steam path
Legitimate only: resident client (Steam QR login + downloader like
xboxnative's launcher) or clean-room steam_api replacement that passes the
game's own checks. ~500 MB of the 5 GB budget. Every game's DRM check is the
only true test — an "init PASS" from an emulator DLL is NOT evidence (proven:
gbe_fork false-PASS on Megabonk).

## M7 — 32-bit (x86 engine)
Console evidence: native compat mode is DEAD (ljmp 0x23 -> 0xc0000005, CS
stays 0x33; WoW64 DLLs unloadable). Path: embed an x86 interpreter/JIT
(Boxedwine core is the known-works candidate; GPL-2 licensing decision
required) behind 32->64 thunks at the syscall boundary.
- **Acceptance:** Spacewar (32-bit, D3D9) reaches its window.

## Rung ladder (each = console pass with evidence)
0. own tiny PEs (done offline; M0 on console)
1. a 64-bit D3D11 indie game we own, no anti-cheat (M1-M4, M6)
2. 32-bit D3D9 title (M5 + M7)
3. AAA class — memory is the wall: lowest textures first, then the
   >5 GB swap allocator experiment (aerisarn/xbox-swap, untested for AAA)

## Non-negotiables throughout
- Every new offline test green before console work.
- STUB hits are work items; never silent success.
- No game files, no DRM circumvention, no Valve/MS binaries committed.
