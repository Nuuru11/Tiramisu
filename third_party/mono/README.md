# Mono 6.8.0.105 — embedding headers + SGen runtime library (user-space)

Mono embedding needs the runtime's headers and a *shared* `libmono-2.0.so`.
Mineplex's system Mono is the Debian build where `/usr/bin/mono` is a
statically-linked `mono-sgen` binary: no shared libmono and no headers are
installed system-wide, and installing system packages is out of scope.

Instead, the three Ubuntu noble packages were downloaded with
`apt-get download` (no install) and extracted here with `dpkg-deb -x`:

| Package | Version | Provides |
|---|---|---|
| `libmono-2.0-1` | 6.8.0.105+dfsg-3.6ubuntu2 | `libmono-2.0.so.1` symlinks |
| `libmonosgen-2.0-1` | 6.8.0.105+dfsg-3.6ubuntu2 | `libmonosgen-2.0.so.1.0.0` (the SGen runtime) |
| `libmono-2.0-dev` | 6.8.0.105+dfsg-3.6ubuntu2 | `usr/include/mono-2.0/` headers, `mono-2.pc` |

Layout mirrors the packages: `usr/lib/libmono-2.0.so*`,
`usr/include/mono-2.0/mono/{jit,metadata}/...`.

The embedded runtime still uses the *system* framework assemblies
(`/usr/lib/mono/4.5/mscorlib.dll` etc., installed by the system Mono
packages) via `mono_set_dirs("/usr/lib", "/etc")` — only the embedding
headers and the shared runtime library come from here.

The harness (`src/trt/mono_host.cpp`) dlopens this library by explicit path
(first choice), falling back to the system loader name `libmono-2.0.so.1`
and then `libmonoboehm-2.0.so.1` (Boehm GC runtime, present system-wide).

Mono is MIT-licensed (see `usr/share/doc/libmono-2.0-1/copyright`).
