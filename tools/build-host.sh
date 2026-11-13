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

# --- 2. compile + link the host (UWP profile) --------------------------------
UWP_OBJ_DIR="$out/obj" "$CB/scripts/build.sh" --uwp \
  --out "$out/layout/tiramisu.exe" --jobs "$(nproc)" \
  src/host/main.cpp src/host/host_log.cpp src/host/swapchain.cpp \
  src/host/ui.cpp src/host/webxfer.cpp src/host/audio.cpp \
  src/trt/pe_loader.cpp src/trt/module_table.cpp src/trt/vfs.cpp \
  src/trt/apiset.cpp src/trt/thunk.cpp src/trt/images.cpp \
  src/trt/pe_exports.cpp src/trt/bridge_d3d.cpp src/trt/bridge_reg.cpp \
  src/trt/bridge_mem.cpp src/trt/bridge_thread.cpp src/trt/bridge_sys.cpp \
  src/trt/bridge_dinput.cpp src/trt/bridge_audio.cpp \
  src/trt/bridge_steam.cpp src/trt/bridge_winmm.cpp src/trt/bridge_d3d9.cpp \
  src/trt/bridge_dsound.cpp \
  src/trt/bridge_kernel32.cpp src/trt/bridge_user32.cpp src/trt/input.cpp \
  src/host/vendor/miniz.c src/host/vendor/miniz_tdef.c \
  src/host/vendor/miniz_tinfl.c src/host/vendor/miniz_zip.c \
  src/host/vendor/qrcodegen.c \
  --link-arg d3d11.lib --link-arg dxgi.lib --link-arg xinput.lib \
  --link-arg d3dcompiler.lib --link-arg xaudio2_8.lib \
  -- /MT /O2 /EHsc /DNDEBUG /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN \
     /D_CRT_SECURE_NO_WARNINGS /DWINRT_NOCRASH_ON_UNHANDLED

# --- 3. sanity: UWP image MUST be an AppContainer image ----------------------
objdump -p "$out/layout/tiramisu.exe" | grep -E 'DllCharacteristics|Subsystem' || true
