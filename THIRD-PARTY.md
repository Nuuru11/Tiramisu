# Third-party components

Vendored, with licenses preserved in their directories:

| Component | Where | License | Why |
|---|---|---|---|
| miniz | `src/host/vendor/` | MIT | PNG inflate, zip (phone transfer) |
| stb_image | `src/host/vendor/stb_image.h` | public domain / MIT | image-format fallback loader |
| mojoshader | `third_party/mojoshader` | zlib | GLSL → HLSL translation |
| d3d8to9 | `third_party/d3d8to9` | MIT | D3D8 semantics reference for the d3d8 bridge |
| gl4es | `third_party/gl4es` | MIT | OpenGL surface semantics reference |

Everything else in this repository is original code, MIT licensed — see
`LICENSE`. D3D9/D3D8/DDraw bridges follow the published semantics of
D3D9On12 and cnc-ddraw (both MIT) without shipping their code.

No game files, game assets, SDK binaries, or credentials are committed.
