# Tiramisu UWP

**A Windows PE64 runtime for Xbox Series X|S Dev Mode.** One signed UWP "Game"
app that loads Windows `.exe`s in-process — Wine's job, targeting UWP instead
of POSIX. No CPU emulation for 64-bit games: the console is x64, guests run at
native speed on the same core, same ABI, same process.

Named after the fruit-emulator tradition (yuzu, citra, papaya).

```
Tiramisu UWP host (CoreWindow + D3D11 flip + XInput, 5 GB as a Game package)
  └── trt — Tiramisu runtime
        ├── PE64 loader   map -> sections -> relocate -> bind imports -> TLS
        ├── module table  every import routed: SYSTEM | BUILTIN | STUB
        ├── bridges       kernel32 core implemented on UWP; user32 = skeleton
        └── VFS           C:\ -> LocalState\drive_c, Z:\ -> package (read-only)
```

## Status (honest)

| Layer | State |
|---|---|
| Offline loader test (Linux, real PE guest executed) | **PASS** — mapping, DIR64/HIGHLOW relocation, import binding, W^X seal, entry execution, checksum-verified |
| UWP host (CoreWindow/D3D11/XInput/LocalState log) | **BUILDS** — signed `Tiramisu.msix` produced and verified |
| Console run | **PENDING** — `tools/deploy.sh install` then look at the TV |
| user32 message loop / input / audio | skeleton only — the known keystone gap |
| 32-bit | dead by console evidence (compat-mode `ljmp 0x23` = 0xc0000005 on Series S); needs an x86 engine later |

## Build (Linux, all cross)

```sh
tools/build-host.sh   # offline loader test gate -> out/layout/tiramisu.exe
tools/pack.sh         # makemsix pack + osslsigncode sign + round-trip verify
```

Needs: LLVM 19 (`clang-cl`, `lld-link`), xwin splat at
`~/.cache/uwp-crossbuild/xwin`, `makemsix`, `osslsigncode`, and the
uwp-crossbuild wrapper (defaults to `~/gow-uwp/ref/uwp-crossbuild`; override
with `UWP_CROSSBUILD`).

## Deploy (console)

```sh
DP_HOST=<console-ip> DP_PASS=<portal-pass> tools/deploy.sh install out/Tiramisu.msix out/dev.cer
# then: tools/deploy.sh launch <FamilyName>!App <FullName>   # from ps output
# and:  tools/deploy.sh screenshot tv.png                    # LOOK at the TV
```

Dev Home prerequisites: Device Portal enabled, and **"Treat UWP apps as games
by default" ON** (5 GB not 1 GB — missing this fails everything silently).
The log lands in LocalState `tiramisu.log`; pull it via Device Portal.

## Rules of the road

Legitimate paths only: games you own, no DRM bypass, no Xbox security bypass.
No game files or credentials in the repo. MIT for our code; see
`docs/03-evidence.md` for what is proven vs assumed.
