# Mouse support on Xbox (Tiramisu host)

Two separate gates decide whether a pointer reaches a UWP app on Xbox. Only the
first one is in our control.

## 1. Input plumbing inside the app (ours, already in place)

`src/host/main.cpp` and `src/trt/input.cpp` already consume every pointer source
the shell is willing to deliver:

- `CoreWindow::PointerPressed` / `PointerMoved` / `PointerReleased` with
  `PointerUpdateKind` mapped to guest button bits.
- `GameInputKindMouse` via `native_gameinput.h` (relative deltas + buttons),
  which takes priority over synthetic pad-as-mouse movement.
- `webxfer.cpp` pad-as-mouse as the fallback for controller-only sessions.

So the guest side (`trt_msg` ring, `GetCursorPos`, `WM_MOUSEMOVE`) is wired. If
the shell delivers pointer events, Tiramisu already forwards them to the game.

Observed on Series S: `input: GameInput mouse callback failed hr=0x80004001`
(`E_NOTIMPL`). `webxfer.cpp` logs this and falls back to controller input. That
is the shell refusing the gamepad-adjacent mouse subscription, not a Tiramisu
bug.

## 2. The per-title allowlist (Microsoft's, not ours)

Microsoft's position, unchanged since 2020:

> Xbox supports the use of mouse and keyboard in some games and apps, but it
> doesn't work for all content. **The game or app publisher must enable this
> feature for their content.**
> — Xbox Support, "Navigate with a mouse and keyboard"

Enabling is a per-title publisher opt-in. For a sideloaded dev-mode package
there is no supported switch. Everything below is an unsupported experiment.

## What "spoofing the app name" can and cannot do

The relevant identifier is the **Package Family Name**:

```
PFN = <Identity Name> + "_" + <PublisherId>
```

- `Identity Name` is free text you choose (`Xan.Tiramisu` today).
- `PublisherId` is a 13-character hash **derived from the `Publisher` string**,
  e.g. `CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond,
  S=Washington, C=US` → `8wekyb3d8bbwe`.
- The manifest `Publisher` must match exactly the subject of the signing
  certificate, field order included, or SignTool fails with `0x8007000B`.

Consequences:

1. You *can* set `Identity Name="Microsoft.MicrosoftEdge"` in a copy of the
   manifest — nothing stops that.
2. You can also make a **self-signed** certificate whose subject is exactly
   `CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond,
   S=Washington, C=US`, and then `PublisherId` computes to `8wekyb3d8bbwe`.
   Anyone can mint a cert with any subject; dev-mode sideload only needs the
   cert trusted on the target.
3. Therefore the *string* `Microsoft.MicrosoftEdge_8wekyb3d8bbwe` is
   reproducible. What is **not** reproducible is Microsoft's signature — if the
   gate ever validates the signer chain rather than the PFN string, the spoof
   fails at install or at launch.
4. If a package with that identity is already registered on the console,
   deployment refuses with `0x80073CF3` ("a different package with the same name
   is already installed"). In Dev Mode retail apps are not registered, so a
   collision is unlikely but possible.

**Unverified premise:** there is no public documentation that the mouse
allowlist is keyed on PFN at all. The documented mechanism is a publisher
opt-in tied to the title, which is server/store side. It is equally plausible
that the console checks a signed title configuration and ignores local identity
entirely. Treat the spoof as a hypothesis to test, not a known bypass.

The "channel/release" part has no documented per-app knob either. The switches
that exist are the console OS Insider ring (Settings → Insider) and Store app
flight rings; neither is settable for a sideloaded package.

## Experiment (unsupported, own console, dev mode)

1. Copy `tiramisu-woowop-engine-vk1/package/AppxManifest.xml`.
2. Set `Identity Name="Microsoft.MicrosoftEdge"` and
   `Publisher="CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond,
   S=Washington, C=US"`.
3. Create a self-signed cert with that exact subject, install it as trusted,
   sign the package with it.
4. Deploy through Device Portal and launch.
5. Watch the host log for either
   `input: physical mouse button received` (allowlist was satisfied), or
   `input: GameInput mouse callback failed hr=0x80004001` (still refused).

Do not ship this identity. It cannot be published, it can collide with a real
Edge package, and a future console build may reject the signature outright.

## What actually helps without spoofing

- Keep the `Chromium`-style pointer paths alive (`PointerPressed` already
  fires for touch and for mouse when the console allows it).
- Pad-as-mouse (`webxfer.cpp`) remains the only guaranteed pointer for
  sideloaded titles today.
