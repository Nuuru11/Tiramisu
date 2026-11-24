# Xbox One / One S / One X support map

Status: **planning document — One-family hardware is not yet verified.** The
Series S is the only console with on-device proof (`docs/title-matrix.md`).

This file answers "what do we need to support Xbox One, Xbox One S and Xbox One
X?" without claiming support we have not measured. Every item below is either
in the tree today or an explicit test/write item.

## 1. Hardware envelopes (published specs, not measured by Tiramisu)

| Console | CPU | GPU | RAM | Notes |
|---|---|---|---|---|
| Xbox One (2013) | 8-core Jaguar @ 1.75 GHz | GCN, ~1.31 TFLOPS | 8 GB DDR3, ~68 GB/s | baseline target |
| Xbox One S | 8-core Jaguar @ 1.75 GHz | GCN, ~1.4 TFLOPS | 8 GB DDR3, ~68 GB/s | same class as One |
| Xbox One X | 8-core Jaguar @ 2.3 GHz | GCN, ~6 TFLOPS | 12 GB GDDR5, ~326 GB/s | strongest one-family target |
| Xbox Series S (proven) | 8-core Zen 2 @ 3.6 GHz | RDNA2, ~4 TFLOPS | 10 GB GDDR6, ~224 GB/s | current reference build |

All four are x86-64, so PE32+/PE32 guests do not need CPU emulation. The
One-family bottleneck is **CPU throughput and memory bandwidth**, not the
instruction set. One S and One X still cannot be assumed to behave like Series
hardware; the OS/API surface differs too.

## 2. What Tiramisu requires from the console today

From `package/AppxManifest.xml` and the host boot path (`src/host/main.cpp`):

- UWP AppContainer activation with `Windows.Universal` device family.
- `expandedResources` capability (the Dev Home **"Treat UWP apps as games by
  default"** setting). Without it the app is capped much lower and may be
  killed at startup.
- `codeGeneration` capability; the boot probe currently reports
  `jit RWX alloc -> DENIED, W->RX -> ALLOWED` on Series S. The one-family OS
  must be probed separately.
- D3D11 device + CoreWindow swapchain (`src/host/swapchain.cpp`). A guest
  requesting FL 11_1 is already named-and-failed if the host cannot provide it.
- XAudio2 2.8 mixer (`src/host/audio.cpp`).
- GameInput / CoreWindow pointer + XInput fallback (`src/host/main.cpp`,
  `src/trt/bridge_gameinput.cpp`, `src/trt/bridge_xinput.cpp`).
- WinRT networking (`Windows.Web.Http`, `MessageWebSocket`) for Steam.
- LocalState is package-family scoped. Switching identity (public vs Edge/KBM)
  loses the library unless migrated with `tools/migrate-localstate.py`.

## 3. Known one-family risks and required work

### 3.1 Startup crash (the current One S report)

A user reported instant crash after installing on an Xbox One S. Do not assume
one cause until the log is pulled. Checklist:

1. Trust the right certificate before installing; do **not** put the `.cer` in
   Dependencies. `Tiramisu.msix` pairs with `dev.cer`;
   `Tiramisu-EdgeSpoof.msix` pairs with `edge-dev.cer`.
2. Enable Dev Home → Settings → **Treat UWP apps as games by default** before
   launch (required for `expandedResources`).
3. Pull `LocalState\tiramisu.log` through Device Portal after one failed
   launch. The first boot lines name which probe/API failed.
4. If the log is empty, activation itself failed: suspect manifest capability,
   signature/publisher mismatch, or an identity collision with a real
   `Microsoft.MicrosoftEdge` package (use the public Xan package on One).

Work item: add a startup crash-safe log flush that always writes the boot
probe line before touching D3D11/GameInput/Steam. The current logger already
flushes, but a filter should capture the first fault address on One hardware.

### 3.2 Memory + performance profiles

`tools/console_type.py` already detects `one_s` / `one_x` from Device Portal
and emits starting tweaks:

```json
"one_x": { "memory_budget_mb": 4096, "os_build": 22621 },
"one_s": { "memory_budget_mb": 3072, "os_build": 19045 }
```

These are **starting points, not measured results**. Required work:

- Add a host-side profile applied at boot (not just a PC-side tool), keyed by
  console family, that sets `memory_budget_mb`, frame cap, render scale and a
  conservative input mode.
- Verify the 5 GB concession on each one-family console; if it is lower, clamp
  to the measured headroom instead of failing allocations.
- Re-run the corpus + RACCOIN/Go Next depth gates with the reduced budget.

### 3.3 API surface differences

- GameInput may be unavailable on older OS builds. The host already logs
  `GameInput ... hr=` and falls back to pad-as-mouse; verify the fallback on
  One S and add XInput-only mode if needed.
- D3D12 is experimental everywhere; One-family D3D12 paths should stay OFF in
  the One profile until a hardware probe says otherwise.
- Vulkan is implemented as SPIR-V→HLSL→D3D11, so no Vulkan driver is needed,
  but shader model limits follow the D3D11 feature level.
- XAudio2 2.8 is expected on all Xbox One OS builds; confirm with the boot log.

### 3.4 KBM / mouse spoof

The Edge-identity KBM package is deliberately shipped, but on older retail OS
builds a real Edge package may already be registered, causing install error
`0x80073CF3`. One-family guidance:

- Default to the public `Tiramisu.msix` + pad2kbm / phone trackpad.
- Offer `Tiramisu-EdgeSpoof.msix` only after the user accepts the PFN migration
  and collision risk.
- Keep `XBOX-MOUSE.md` warnings intact.

## 4. Test matrix to call one-family "supported"

A row is added to `docs/title-matrix.md` only with artifacts:

1. **M0 install/activate**: `Tiramisu.msix` installs, `http://<ip>:9127/api/status`
   answers `{"running":true}` and a TV screenshot is captured.
2. **Boot probes**: log shows D3D11 device OK, XAudio2 OK, mouse/keyboard
   fallback named, JIT probe result, memory budget.
3. **One light title**: e.g. Ultra Pool or a small D3D11 indie reaches pump and
   renders a frame; screenshot + `proof.json`.
4. **Save round-trip**: backup/restore `.zip` works on that PFN.
5. **One X repeat** of 1–4; One S repeat of 1–4.
6. **Steam**: QR sign-in + a small owned depot download (already proven on
   Series S with HoloCure 2420510) on each console family.

Until these exist, `docs/known-issues.md` must keep the one-family entry as
**untested**.

## 5. File/status index

- Console detection helper: `tools/console_type.py`, `docs/console-type.md`
- Host boot probes: `src/host/main.cpp`
- Memory/settings: `src/host/settings.cpp`, `docs/console-type.md`
- Manifest capabilities: `package/AppxManifest.xml`
- Evidence rules: `TIRAMISU-MASTER-PROMPT.md`, `docs/title-matrix.md`
- Known issues: `docs/known-issues.md` → "Xbox One S|X"
