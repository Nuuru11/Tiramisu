# SDK header case shims

The xwin SDK splat lowercases some header filenames
(`sdkddkver.h`, `mswsock.h`) while Mono includes them with their
canonical Windows casing (`<SDKDDKVer.h>`, `<MSWSock.h>`). Windows
filesystems are case-insensitive, so this never matters there; on the
case-sensitive Linux build host the includes fail. Each file here is a
one-line forwarding shim to the real (lowercased) SDK header. They
change no behaviour — they only fix filename case.

`build-mono-uwp.sh` puts this directory first on the include path so
the shims win. If a future SDK or Mono version adds another
case-mismatched include, add a shim here (and check it is really a
case problem, not a missing header).
