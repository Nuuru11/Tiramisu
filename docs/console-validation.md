# Console validation — captured 2026-11-22 (Series S, 10.0.0.23)

Real Device Portal session (`XBOX_IP=10.0.0.23`, OS 26100.9662, Xbox Series S),
not an offline simulation. Evidence files:

- `logs/20261122-020206-tiramisu.log` — real failed boots of **Sekiro**.
- `logs/20261122-021634-tiramisu.log` — real boot of **ultrapool**.
- `tests/fixtures/logs/real-console-sekiro-failure.log` — extracted failure.
- `tests/fixtures/logs/real-console-ultrapool-boot.log` — extracted boot.
- `docs/proof/ultrapool-autopilot-console.png` — TV screenshot during the run.

## Crash containment — validated 2026-11-22 (build 0.2.2.197)

Deployed freshly packed `out/Tiramisu.msix` (0.2.2.197), launched Sekiro via
`/api/launch/Sekiro`, waited 45 s. Result: **the app survived** — `/api/status`
still returned `running:true`, dashboard responsive, `guest_running:false`.
The log shows `guest fault: 0xc0000005 at sekiro.exe+0x115c66b ... read address
FFFFFFFFFFFFFFE8` followed by the GPR/stack dump and a clean return to shelf.
Extracted as `tests/fixtures/logs/real-console-seh-contained.log`.

## What is proven on hardware

- Signed `Xan.Tiramisu_0.2.2.192` is installed and launches via Device Portal.
- The in-console web dashboard (`http://10.0.0.23:9127`) is live and serves the
  staged shelf (Sekiro, stage-ultrapool, ultrapool).
- `POST /api/launch/ultrapool` queued the title; `/api/status` reported
  `guest_running=true, running_game=ultrapool, last_launch_result=started`.
- The guest reached **real D3D11 swapchain creation** (`1920x1080`, 3 images),
  Vulkan 1.3 ICD initialization (`AMD - Tiramisu Vulkan->D3D11`), a real render
  pass (`Draw #1 pipe=1230 idx=1 vtx=6`) and `vkQueuePresentKHR frame=1`.
- WASAPI initialized a real 48 kHz stereo float client (`IAudioClient::Start -> S_OK`).

## Named blockers from the same session (fail-visible, not hidden)

1. **Sekiro** repeatedly launches, then faults:
   - `kernel32: CreateFileW open failed on host` for `Z:\S\pack.dat` — required
     game data is not staged; no bridge can repair missing content.
   - Later `FATAL VEH: unhandled exception 0xc0000005 ... at ntdll.dll`, with
     `p1=ffffffffffffffff` — a bogus guest pointer reaching a host API; the
     host VEH returns `EXCEPTION_CONTINUE_SEARCH`, so the app terminates.
   Both signatures are now in `tools/fix_db.json` as named work items.
2. **ultrapool** presents exactly one frame (`frame=1` never advances) and the
   TV screenshot is blank apart from the cursor. Rendering path is proven, but
   the frame loop stalls after the first present — next console work item.

## What this means for the autopilot

The transfer -> audit -> generate -> launch -> log -> fix -> retry loop is live
on the console (autopilot fields are exposed in `/api/status`). Console
validation now has real evidence for both a successful boot path and a named
failure path. Remaining console work is the frame-loop stall and the Sekiro
missing-content / invalid-pointer faults.
