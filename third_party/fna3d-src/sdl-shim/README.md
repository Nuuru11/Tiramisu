# SDL shim for the UWP build (Tiramisu-only)

Tiramisu links **no SDL** on the console: there is no SDL window, video,
or audio in the UWP host. But FNA3D's D3D11 driver and `FNA3D.c` are
written against SDL2 for memory, mutexes, dynamic loading, hints,
logging, and window bookkeeping. This directory provides exactly the
SDL2 surface those translation units use, implemented on Win32/UWP
primitives in the AppContainer partition.

Files:

- `SDL.h` — the shim header (version 2.26.0, the minimum FNA3D accepts).
- `SDL_syswm.h` — `SDL_SysWMinfo` (Windows part only) + `SDL_GetWindowWMInfo`.
- `sdl_shim.c` — the implementations.

## What is real here

- Memory/string (`SDL_malloc`, `memcpy`, `strlen`, `snprintf`, ...): the
  C runtime, via macros.
- Mutexes: `CRITICAL_SECTION`.
- Dynamic loading: `GetModuleHandleA` / `GetProcAddress` on modules the
  host already links (`d3d11.dll`, `dxgi.dll`). `SDL_UnloadObject` is a
  deliberate no-op: nothing is loaded by us, and freeing a
  `GetModuleHandle` handle would corrupt the host's own import.
- Hints: a small real key/value store (16 slots).
- Logging: `OutputDebugStringA`.
- Iconv: real UTF-8 → `WCHAR_T` via `MultiByteToWideChar` — the only
  conversion FNA3D uses (`D3D11_SetStringMarker`). Any other conversion
  fails honestly (`SDL_iconv_open` returns `(SDL_iconv_t) -1`).
- `SDL_strcasecmp`: `_stricmp`.

## What is honestly absent

There are no SDL windows on the console, so:

- `SDL_GetWindowData` / `SDL_SetWindowData`: no storage, always `NULL`.
- `SDL_GetWindowSizeInPixels`: reports 0×0.
- `SDL_GetWindowWMInfo`: returns `SDL_FALSE`.

The host-adopted D3D11 path never calls these (it gets the drawable size
from the host swapchain instead). They exist so the stock path still
compiles, and fail visibly if ever reached.

## Build wiring

`tools/build-host.sh` puts this directory on the include path (`-I
third_party/fna3d-src/sdl-shim`) and compiles `sdl_shim.c` into the host.
The shim is **never** on the include path of the Linux build, which uses
real SDL2 via CMake (`third_party/fna3d/build-fna3d.sh`).

## Scope warning

Do not grow this shim into a fake SDL. If a future FNA3D need cannot be
implemented honestly on UWP primitives, the right answer is to change
the FNA3D call site (or the host), not to add a lying stub here.
