#!/bin/bash
# tiramisu/tools/corpus-smoke.sh
# Hardware-free verification of the real-game corpus:
# builds tests/test_corpus.cpp with the same clang++ flag style build-test.sh
# uses for test_real_load, then runs it on every PE found under
# ~/tiramisu/corpus-local/ (depth 2), tee-ing everything to
# tests/out/corpus-report.md.
#
# Exit 0 = no LOAD FAILED. Corrupted/MZ-less/unsupported-arch inputs report
# SKIP or LOAD FAILED individually — a SKIP never fails the gate.
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

# --- build test_corpus (same source list as the test_real_load case) ---
clang++ -std=c++17 -O2 -Wall -no-pie \
  tests/test_corpus.cpp \
  src/trt/pe_loader.cpp src/trt/module_table.cpp src/trt/apiset.cpp \
  src/trt/thunk.cpp src/trt/vfs.cpp src/trt/images.cpp src/trt/pe_exports.cpp src/trt/bridge_d3d.cpp src/trt/bridge_reg.cpp \
  src/trt/bridge_mem.cpp src/trt/bridge_thread.cpp src/trt/bridge_sys.cpp \
  src/trt/bridge_dinput.cpp src/trt/bridge_audio.cpp \
  src/trt/bridge_steam.cpp src/trt/bridge_winmm.cpp src/trt/bridge_d3d9.cpp src/trt/bridge_dsound.cpp src/trt/bridge_ddraw.cpp src/trt/bridge_d3dx9.cpp src/trt/bridge_mmedia.cpp src/trt/bridge_img.cpp src/trt/bridge_d3d8.cpp src/trt/bridge_openal.cpp src/trt/bridge_ole.cpp src/trt/bridge_shell.cpp src/trt/bridge_shlwapi.cpp src/trt/bridge_comctl.cpp src/trt/bridge_gdi.cpp src/trt/bridge_d3dx.cpp src/host/vendor/miniz_tinfl.c src/trt/input.cpp \
  src/trt/bridge_kernel32.cpp src/trt/bridge_user32.cpp \
  -o "$out/test_corpus" || exit 1

# --- collect every PE under corpus-local/ (up to depth 2) ---
CORPUS_DIR="corpus-local"
mapfile -t PES < <(find "$CORPUS_DIR" -maxdepth 2 -type f 2>/dev/null | \
  grep -iE '\.(exe|dll)$' | sort)

if [[ ${#PES[@]} -eq 0 ]]; then
  echo "corpus-smoke: no PE files found under $CORPUS_DIR"
  exit 0
fi

printf 'corpus-smoke: %d PE file(s): %s\n\n' "${#PES[@]}" "${PES[*]}"

# --- run; tee to tests/out/corpus-report.md; exit nonzero on LOAD FAILED ---
rc=0
"$out/test_corpus" "${PES[@]}" 2>&1 | tee "$out/corpus-report.md"
# PIPESTATUS: the tee pipeline must not swallow the test's exit code
[[ ${PIPESTATUS[0]} -eq 0 ]] || rc=1

exit $rc
