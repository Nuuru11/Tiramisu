#!/bin/bash
# tiramisu/tools/build-host.sh — cross-build the Tiramisu UWP host on Linux.
#   clang-cl (MSVC ABI, LLVM 19) -> lld-link -> out/layout/tiramisu.exe
# The host is a UWP image (--uwp): AppContainer + WINAPI_FAMILY_APP.
# Guest exes are separate normal desktop PEs — never compiled with --uwp.
set -euo pipefail
export PATH="$HOME/.local/bin:$PATH"
here="$(cd "$(dirname "$0")/.." && pwd)"; cd "$here"
out="${1:-out}"; mkdir -p "$out/layout" "$out/obj"

CB="${UWP_CROSSBUILD:-$here/../gow-uwp/ref/uwp-crossbuild}"
[[ -x "$CB/scripts/build.sh" ]] || CB="$HOME/gow-uwp/ref/uwp-crossbuild"
[[ -x "$CB/scripts/build.sh" ]] || { echo "ERROR: uwp-crossbuild wrapper not found (set UWP_CROSSBUILD)" >&2; exit 2; }
[[ -d "$HOME/.cache/uwp-crossbuild/xwin/sdk" ]] || { echo "ERROR: no SDK — run: xwin --accept-license --arch x86_64 splat --output ~/.cache/uwp-crossbuild/xwin" >&2; exit 2; }
command -v makemsix >/dev/null || { echo "ERROR: makemsix not in PATH" >&2; exit 2; }

# --- 1. build the offline test first: loader core must be green on host ------
"$here/tools/build-test.sh" > "$out/obj/test.log" 2>&1 \
  || { echo "ERROR: offline loader test FAILED — see $out/obj/test.log"; tail -5 "$out/obj/test.log"; exit 3; }
echo "offline loader test: PASS"

# --- 2a. build Mono for UWP (Path B step 5): stock 6.8.0.105, SGen,
#         interpreter-capable, statically linked. Cached; a no-op when
#         up to date (see third_party/mono-uwp/build-mono-uwp.sh). The
#         script prints ONLY the library path on stdout (progress goes
#         to stderr), so the path is captured here. The link below also
#         adds bcrypt.lib: Mono's Windows crypto (mono-security-windows)
#         uses BCryptGenRandom under UWP (BCrypt instead of CryptoAPI).
MONO_LIB="$("$here/third_party/mono-uwp/build-mono-uwp.sh")" \
  || { echo "ERROR: Mono for UWP build failed" >&2; exit 4; }
[[ -f "$MONO_LIB" ]] || { echo "ERROR: Mono UWP lib missing: $MONO_LIB" >&2; exit 4; }
echo "Mono for UWP: $MONO_LIB"

# --- 2. compile + link the host (UWP profile) --------------------------------
# FNA3D D3D11 (Path B step 3): the D3D11 driver + FNA3D core are compiled
# into the host so a managed game renders on the host's CoreWindow
# swapchain. MojoShader is NOT compiled separately here: it already
# lives in the host via src/trt/mojo_hlsl.c (unity build, extended with
# the effects + d3d11 backend for FNA3D) -- compiling it twice would
# duplicate every MOJOSHADER_* symbol. The SDL shim
# (third_party/fna3d-src/sdl-shim) stands in for SDL2, which does not
# exist on the console; see its README. The Linux gate keeps using
# FNA3D's OpenGL driver (llvmpipe) via third_party/fna3d/build-fna3d.sh.
UWP_OBJ_DIR="$out/obj" "$CB/scripts/build.sh" --uwp \
  --out "$out/layout/tiramisu.exe" --jobs "$(nproc)" \
  -I third_party/fna3d-src/include \
  -I third_party/fna3d-src/MojoShader \
  -I third_party/fna3d-src/sdl-shim \
  -I third_party/mono/usr/include/mono-2.0 \
  src/host/main.cpp src/host/host_log.cpp src/host/swapchain.cpp \
  src/host/ui.cpp src/host/webxfer.cpp src/host/audio.cpp src/host/settings.cpp \
  src/trt/pe_loader.cpp src/trt/module_table.cpp src/trt/vfs.cpp src/trt/iso.cpp \
  src/trt/bridge_res.cpp \
  src/trt/apiset.cpp src/trt/thunk.cpp src/trt/images.cpp \
  src/trt/pe_exports.cpp src/trt/bridge_d3d.cpp src/trt/bridge_d3d12.cpp src/trt/bridge_reg.cpp \
  src/trt/bridge_mem.cpp src/trt/bridge_thread.cpp src/trt/bridge_sys.cpp \
  src/trt/bridge_dinput.cpp src/trt/bridge_audio.cpp \
  src/trt/bridge_steam.cpp src/trt/bridge_winmm.cpp src/trt/bridge_d3d9.cpp \
  src/trt/bridge_dsound.cpp src/trt/bridge_ddraw.cpp src/trt/bridge_d3dx9.cpp \
  src/trt/bridge_mmedia.cpp src/trt/bridge_img.cpp src/trt/bridge_d3dx.cpp src/trt/bridge_d3d8.cpp src/trt/bridge_openal.cpp src/trt/bridge_ole.cpp src/trt/bridge_shell.cpp src/trt/bridge_shlwapi.cpp src/trt/bridge_comctl.cpp src/trt/bridge_gdi.cpp \
  src/trt/bridge_wininet.cpp src/trt/bridge_psapi.cpp src/trt/bridge_iphlpapi.cpp src/trt/bridge_imm32.cpp src/trt/bridge_crypt32.cpp src/trt/bridge_dbghelp.cpp \
  src/trt/bridge_ntdll.cpp src/trt/bridge_opengl32.cpp src/trt/glsl_hlsl.cpp \
  src/trt/bridge_vulkan.cpp src/trt/spirv_hlsl.cpp src/trt/bridge_hid.cpp \
  src/trt/bridge_xinput.cpp src/trt/bridge_wasapi.cpp src/trt/bridge_winrt.cpp src/trt/bridge_ws2.cpp src/trt/bridge_msvcrt.cpp src/trt/bridge_console_pack.cpp \
  src/trt/bridge_sys_extras.cpp src/trt/bridge_mf.cpp src/trt/bridge_gameinput.cpp \
  src/trt/bridge_kernel32.cpp src/trt/bridge_kernel32_gap.cpp src/trt/bridge_user32.cpp src/trt/bridge_user32_gap.cpp src/trt/input.cpp \
  src/trt/bridge_nvapi.cpp src/trt/bridge_gdiplus.cpp src/trt/bridge_fmod.cpp \
  src/trt/bridge_fmod_studio.cpp \
  src/trt/bridge_membudget.cpp src/trt/mono_host.cpp src/trt/fna_platform.cpp \
  src/trt/fna3d_host.cpp \
  third_party/fna3d-src/src/FNA3D.c \
  third_party/fna3d-src/src/FNA3D_Driver_D3D11.c \
  third_party/fna3d-src/src/FNA3D_PipelineCache.c \
  third_party/fna3d-src/sdl-shim/sdl_shim.c \
  src/host/vendor/miniz.c src/host/vendor/miniz_tdef.c \
  src/host/vendor/miniz_tinfl.c src/host/vendor/miniz_zip.c \
  src/host/vendor/qrcodegen.c src/trt/mojo_hlsl.c src/trt/vendor_stb_vorbis.c \
  --link-arg d3d11.lib --link-arg d3d12.lib --link-arg dxgi.lib --link-arg xinput.lib \
  --link-arg d3dcompiler.lib --link-arg xaudio2_8.lib --link-arg ws2_32.lib \
  --link-arg bcrypt.lib --link-arg "$MONO_LIB" \
  -- /MT /O2 /EHsc /DNDEBUG /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN \
     /D_CRT_SECURE_NO_WARNINGS /DWINRT_NOCRASH_ON_UNHANDLED \
     /DFNA3D_DRIVER_D3D11 /DWINAPI_FAMILY_WINRT=1 \
     /DMOJOSHADER_NO_VERSION_INCLUDE /DMOJOSHADER_EFFECT_SUPPORT

# --- 2b. explicit check: the FNA3D D3D11 driver object built under the
#         UWP toolchain (compile-level gate for Path B step 3). The link
#         above already proves the objects exist and resolve (fna3d_host
#         references FNA3D_D3D11_SetHostDevice; the driver references the
#         MojoShader d3d11 backend from mojo_hlsl.o); this names the
#         objects so a missing driver file fails loudly instead of
#         silently dropping the D3D11 path.
for fna_obj in "$out/obj/"*FNA3D_Driver_D3D11*.obj \
               "$out/obj/"*FNA3D*.obj \
               "$out/obj/"*fna3d_host*.obj; do
  [[ -f "$fna_obj" ]] || { echo "ERROR: FNA3D UWP object missing: $fna_obj" >&2; exit 4; }
done
echo "FNA3D D3D11 driver objects present in $out/obj"

# --- 2c. explicit check: Mono is in the UWP link (Path B step 5). The
#         link above already proves the symbols resolve (mono_host.o
#         references mono_jit_init/mono_jit_exec; lld-link would fail
#         on an unresolved symbol); this names them so a missing Mono
#         lib fails loudly instead of silently dropping the embedding.
#         The exe does not export the Mono symbols (statically linked,
#         internal), so the check is on the linked objects + the lib.
#         FNA3D IS exported from the exe (__declspec(dllexport)) — the
#         __Internal dllmap target — so its exports are checked in the
#         exe's export table.
#         NB: each tool's output is captured to a file before grepping:
#         piping to `grep -q` makes grep exit on the first match,
#         SIGPIPE-ing the producer, which under `set -o pipefail` fails
#         the check even when the symbol is present.
llvm-nm --defined-only "$MONO_LIB" > "$out/obj/mono-symbols.txt" 2>/dev/null \
  || { echo "ERROR: llvm-nm failed on $MONO_LIB" >&2; exit 5; }
for sym in mono_jit_init mono_jit_exec mono_config_parse mono_set_dirs \
           mono_domain_assembly_open mono_add_internal_call \
           mono_install_assembly_load_hook mono_jit_cleanup; do
  grep -q " T $sym$" "$out/obj/mono-symbols.txt" \
    || { echo "ERROR: Mono symbol missing from $MONO_LIB: $sym" >&2; exit 5; }
done
mono_host_obj="$(ls "$out/obj/"*mono_host*.obj 2>/dev/null | head -1)"
[[ -f "$mono_host_obj" ]] \
  || { echo "ERROR: mono_host UWP object missing in $out/obj" >&2; exit 5; }
llvm-nm --undefined-only "$mono_host_obj" > "$out/obj/mono_host-undef.txt" 2>/dev/null \
  || { echo "ERROR: llvm-nm failed on $mono_host_obj" >&2; exit 5; }
for sym in mono_jit_init mono_jit_exec; do
  grep -q " U $sym$" "$out/obj/mono_host-undef.txt" \
    || { echo "ERROR: mono_host.o does not reference $sym (UWP Mono embedding not wired?)" >&2; exit 5; }
done
echo "Mono symbols present in UWP link: mono_jit_init, mono_jit_exec, +6 more (lib: $MONO_LIB)"
objdump -p "$out/layout/tiramisu.exe" > "$out/obj/exe-headers.txt" \
  || { echo "ERROR: objdump failed on $out/layout/tiramisu.exe" >&2; exit 5; }
grep -q "FNA3D_" "$out/obj/exe-headers.txt" \
  || { echo "ERROR: no FNA3D_* exports in $out/layout/tiramisu.exe" >&2; exit 5; }
echo "FNA3D exports present in tiramisu.exe"

# --- 2d. package the Mono framework assemblies + machine config into
#         the app layout (Path B step 5). The UWP branch of mono_host
#         derives framework_dir/config_dir from the install dir:
#         <install>/mono/4.5/*.dll + <install>/mono/gac/... and
#         <install>/etc/mono/config, mirroring Linux's /usr/lib + /etc
#         split. The assemblies are the same 6.8.0.105 IL the Linux
#         path uses (platform-independent; not rebuilt). tools/pack.sh
#         includes $out/layout in the MSIX.
MONO_FW="${MONO_FRAMEWORK_DIR:-/usr/lib/mono}"
[[ -d "$MONO_FW/4.5" && -f "$MONO_FW/4.5/mscorlib.dll" ]] \
  || { echo "ERROR: Mono framework assemblies not found: $MONO_FW/4.5" >&2; exit 6; }
mkdir -p "$out/layout/mono" "$out/layout/etc/mono"
cp -r "$MONO_FW/4.5" "$out/layout/mono/4.5"
[[ -d "$MONO_FW/gac" ]] && cp -r "$MONO_FW/gac" "$out/layout/mono/gac"
[[ -f /etc/mono/config ]] && cp /etc/mono/config "$out/layout/etc/mono/config"
echo "Mono framework packaged: $(du -sh "$out/layout/mono" | cut -f1) assemblies + config"

# --- 3. sanity: UWP image MUST be an AppContainer image ----------------------
objdump -p "$out/layout/tiramisu.exe" | grep -E 'DllCharacteristics|Subsystem' || true
