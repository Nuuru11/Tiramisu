#!/bin/bash
# tiramisu/tools/corpus-run.sh
# STAGE 6/7 run-depth gate: build tests/test_corpus_run.cpp the same way
# corpus-smoke.sh builds test_corpus.cpp, then EXECUTE real code on the
# corpus — one attempt each, no retry loops:
#
#   PHASE 1 (STAGE 6): DllMain(PROCESS_ATTACH) on every corpus DLL:
#     dstoragecore.dll, dstorage.dll, baselib.dll, UnityPlayer.dll,
#     GameAssembly.dll
#   PHASE 2 (STAGE 7): full EXE boot on the real game EXEs:
#     "Go Next demo.exe" (CRT startup -> WinMain -> UnityPlayer!UnityMain2
#     -> window -> D3D11 -> pump) and UnityCrashHandler64.exe.
#
# Verdict per PE (both legitimate):
#   RUN OK / BOOT <verdict> <name>: <depth ladder / ExitProcess rc / WM_QUIT>
#   RUN/BOOT FAILED <name>: <fault> — last bridge: <dll!symbol>  (named item)
# A fault is a discovery artifact, not a gate failure: this script exits 0
# when every PE got a verdict, nonzero only if the harness itself could not
# produce one.
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

# --- build test_corpus_run (compile C translation units with C compiler first) ---
cobj="$out/run_c_objs"
mkdir -p "$cobj"
"$CLANG" -O2 -Wall -fno-pie -c src/host/vendor/miniz_tinfl.c -o "$cobj/miniz_tinfl.o" || exit 1
"$CLANG" -std=gnu11 -O2 -fno-pie -c src/trt/mojo_hlsl.c -o "$cobj/mojo_hlsl.o" || exit 1

clang++ -std=c++17 -O2 -Wall -no-pie \
  tests/test_corpus_run.cpp \
  src/trt/pe_loader.cpp src/trt/module_table.cpp src/trt/apiset.cpp \
  src/trt/thunk.cpp src/trt/vfs.cpp src/trt/iso.cpp src/trt/images.cpp src/trt/pe_exports.cpp src/trt/bridge_d3d.cpp src/trt/bridge_d3d12.cpp src/trt/bridge_reg.cpp \
  src/trt/bridge_mem.cpp src/trt/bridge_thread.cpp src/trt/bridge_sys.cpp \
  src/trt/bridge_dinput.cpp src/trt/bridge_audio.cpp \
  src/trt/bridge_steam.cpp src/trt/bridge_winmm.cpp src/trt/bridge_d3d9.cpp src/trt/bridge_dsound.cpp src/trt/bridge_ddraw.cpp src/trt/bridge_d3dx9.cpp src/trt/bridge_mmedia.cpp src/trt/bridge_img.cpp src/trt/bridge_d3d8.cpp src/trt/bridge_openal.cpp src/trt/bridge_ole.cpp src/trt/bridge_shell.cpp src/trt/bridge_shlwapi.cpp src/trt/bridge_comctl.cpp src/trt/bridge_gdi.cpp src/trt/bridge_wininet.cpp src/trt/bridge_psapi.cpp src/trt/bridge_iphlpapi.cpp src/trt/bridge_imm32.cpp src/trt/bridge_crypt32.cpp src/trt/bridge_dbghelp.cpp src/trt/bridge_d3dx.cpp src/trt/input.cpp \
  src/trt/bridge_kernel32.cpp src/trt/bridge_kernel32_gap.cpp src/trt/bridge_user32.cpp src/trt/bridge_user32_gap.cpp \
  src/trt/bridge_ntdll.cpp src/trt/bridge_opengl32.cpp src/trt/glsl_hlsl.cpp \
  src/trt/bridge_vulkan.cpp src/trt/spirv_hlsl.cpp src/trt/bridge_hid.cpp src/trt/bridge_xinput.cpp \
  src/trt/bridge_wasapi.cpp src/trt/bridge_winrt.cpp src/trt/bridge_ws2.cpp src/trt/bridge_msvcrt.cpp src/trt/bridge_console_pack.cpp \
  src/trt/bridge_sys_extras.cpp src/trt/bridge_mf.cpp src/trt/bridge_gameinput.cpp \
  "$cobj/miniz_tinfl.o" "$cobj/mojo_hlsl.o" \
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

# --- STAGE 7: EXE-entry boot depth (real game EXEs, offline) ---
EXES=(
  "corpus-local/ortmm/OrTMM.exe"
  "corpus-local/raccoin/RACCOIN.exe"
)
exe_report="$out/corpus-exe-report.md"
{
  printf '# STAGE 7 EXE-entry boot depth — %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf '\nDepth ladder per EXE: CRT-STARTUP OK / WINMAIN ENTERED / UNITYMAIN2 CALLED / WINDOW CREATED / D3D11 DEVICE ACQUIRED / PUMP ENTERED / PUMP QUIT. Furthest marker + named blocker is the deliverable.\n\n'
} > "$exe_report"
for exe in "${EXES[@]}"; do
  [[ -f "$exe" ]] || { echo "corpus-exe: missing $exe (skipped)"; continue; }
  "$out/test_corpus_run" --exe "$exe" 2>&1 | tee -a "$exe_report"
  # BOOT FAILED (exit 1) is a NAMED outcome, not a gate failure: the report
  # names the blocker. Only a crash WITHOUT the named report fails the gate.
  p=${PIPESTATUS[0]}
  if [[ $p -ne 0 && $p -ne 1 ]]; then rc=1; fi
done

exit $rc
