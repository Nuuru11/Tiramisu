# FNA3D (Path B graphics backend)

FNA3D — the 3D renderer FNA uses — built from source as `libFNA3D.so.0`
(SDL2 backend, MojoShader bundled in; OpenGL driver used offline).

- Source clone: `../fna3d-src/` (FNA3D master `d01538e`, MojoShader
  submodule included). Never modified by the build. Note: FNA's own
  submodule pin for FNA3D is `2806acf`; this build uses same-date master
  HEAD `d01538e`, as the Path B spike did. If FNA/FNA3D drift apart, pin
  to FNA's submodule commit.
- Build: `build-fna3d.sh` (CMake, Release, `-DBUILD_SDL3=OFF` — mineplex
  has SDL2 2.30.0, no SDL3). Idempotent; `--force` rebuilds.
- Output (kept): `out/libFNA3D.so.0` (+ `libFNA3D.so` symlink).
- License: zlib — `LICENSE-zlib` (copied from upstream). MojoShader is
  also zlib (its license lives in the submodule).

## Next step (not this one)

On the console, FNA3D's D3D11 driver must be adapted to the host device /
CoreWindow swapchain (no HWND on UWP). The offline harness uses the
OpenGL driver under Xvfb/llvmpipe, exactly as the Path B spike did.
