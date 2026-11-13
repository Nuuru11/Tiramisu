#!/bin/bash
# tiramisu/tools/corpus-run.sh
# STAGE 6 run-depth gate: build tests/test_corpus_run.cpp the same way
# corpus-smoke.sh builds test_corpus.cpp, then EXECUTE real DllMain code on
# the corpus DLLs — smallest to largest — one attempt each, no retry loops:
#
#   dstoragecore.dll, dstorage.dll, baselib.dll, UnityPlayer.dll,
#   GameAssembly.dll
#
# Verdict per DLL (both legitimate):
#   RUN OK <name>: DllMain(PROCESS_ATTACH) returned.
#   RUN FAILED <name>: <fault> — last bridge: <dll!symbol>   (named work item)
# A fault is a discovery artifact, not a gate failure: this script exits 0
# when every DLL got a verdict, nonzero only if the harness itself could not
# produce one (map failure is a named RUN FAILED too).
set -uo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
cd "$here"
out=tests/out
mkdir -p "$out"

# --- same toolchain pick as build-test.sh ---
LLVM_BIN="/usr/lib/llvm-19/bin"
CLANG="$LLVM_BIN/clang"
[[ -x "$CLANG" ]] || CLANG="$HOME/.local/bin/clang-19"
[[ -x "$CLANG" ]] || { echo "ERROR: clang 19 not found" >&2; exit 1; }

# --- build test_corpus_run (same source list as corpus-smoke.sh) ---
clang++ -std=c++17 -O2 -Wall -no-pie \
  tests/test_corpus_run.cpp \
  src/trt/pe_loader.cpp src/trt/module_table.cpp src/trt/apiset.cpp \
  src/trt/thunk.cpp src/trt/vfs.cpp src/trt/images.cpp src/trt/pe_exports.cpp src/trt/bridge_d3d.cpp src/trt/bridge_reg.cpp \
  src/trt/bridge_mem.cpp src/trt/bridge_thread.cpp src/trt/bridge_sys.cpp \
  src/trt/bridge_dinput.cpp src/trt/bridge_audio.cpp \
  src/trt/bridge_steam.cpp src/trt/bridge_winmm.cpp src/trt/bridge_d3d9.cpp src/trt/bridge_dsound.cpp src/trt/bridge_ddraw.cpp src/trt/bridge_d3dx9.cpp src/trt/bridge_mmedia.cpp src/trt/bridge_img.cpp src/trt/bridge_d3d8.cpp src/trt/bridge_openal.cpp src/trt/bridge_ole.cpp src/trt/bridge_shell.cpp src/trt/bridge_shlwapi.cpp src/trt/bridge_comctl.cpp src/trt/bridge_gdi.cpp src/trt/bridge_wininet.cpp src/trt/bridge_psapi.cpp src/trt/bridge_iphlpapi.cpp src/trt/bridge_imm32.cpp src/trt/bridge_crypt32.cpp src/trt/bridge_dbghelp.cpp src/trt/bridge_d3dx.cpp src/host/vendor/miniz_tinfl.c src/trt/input.cpp \
  src/trt/bridge_kernel32.cpp src/trt/bridge_user32.cpp \
  -o "$out/test_corpus_run" || exit 1

# --- smallest-to-largest corpus DLLs (DLLs only: DllMain is the target) ---
CORPUS_DIR="corpus-local"
mapfile -t DLLS < <(find "$CORPUS_DIR" -maxdepth 2 -type f -iname '*.dll' 2>/dev/null | \
  xargs -r ls -la 2>/dev/null | sort -k5 -n | awk '{print $NF}')

if [[ ${#DLLS[@]} -eq 0 ]]; then
  echo "corpus-run: no DLL files found under $CORPUS_DIR"
  exit 0
fi

printf 'corpus-run: %d DLL(s), smallest to largest: %s\n\n' "${#DLLS[@]}" "${DLLS[*]}"

# --- run; tee to tests/out/corpus-run-report.md ---
rc=0
"$out/test_corpus_run" "${DLLS[@]}" 2>&1 | tee "$out/corpus-run-report.md"
[[ ${PIPESTATUS[0]} -eq 0 ]] || rc=1

exit $rc
