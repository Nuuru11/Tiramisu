#!/bin/bash
# third_party/fna-stubs/build-stubs.sh — build the spike-parity Steam/FMOD
# stub libraries Celeste's P/Invokes bind to offline.
#
#   out/libfmod-stub.so           — all 489 fmod + fmodstudio entries
#   out/libcsteamworks-stub.so    — all 872 CSteamworks + sdkencryptedappticket entries
#
# The entry lists come from `monodis --implmap` dumps of Celeste.exe and
# Steamworks.NET.dll (celeste.implmap / steamworks.implmap in this dir —
# symbol-name metadata, not game content). gen-stubs.py generates a default
# stub per entry; the hand-tuned functions in *_overrides.c (listed in
# overrides-*.txt) replace the defaults where boot progress depends on the
# value. The build verifies the final .so exports cover every implmap entry.
#
# THESE ARE STUBS, not implementations: no audio, no Steam, no network.
# They exist so Celeste boots to the title screen offline under Mono; the
# next Path B step replaces them with real dllmaps to Tiramisu's 64-bit
# FMOD and Steam bridges. See README.md.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
OUT="$here/out"
GEN="$here/gen"
mkdir -p "$OUT" "$GEN"

FORCE=0
[[ "${1:-}" == "--force" ]] && FORCE=1

LIB_FMOD="$OUT/libfmod-stub.so"
LIB_STEAM="$OUT/libcsteamworks-stub.so"

needs_build=0
[[ "$FORCE" == 1 ]] && needs_build=1
[[ -f "$LIB_FMOD" && -f "$LIB_STEAM" ]] || needs_build=1
if [[ "$needs_build" == 0 ]]; then
  newer=$(find "$here" -maxdepth 1 \( -name '*.c' -o -name '*.py' -o -name '*.txt' -o -name '*.implmap' \) \
    -newer "$LIB_FMOD" -print -quit 2>/dev/null || true)
  [[ -n "$newer" ]] && needs_build=1
fi
if [[ "$needs_build" == 0 ]]; then
  echo "fna-stubs: outputs up to date, skipping (use --force to rebuild)"
  exit 0
fi

command -v python3 >/dev/null || { echo "ERROR: python3 not found" >&2; exit 2; }
command -v cc >/dev/null || { echo "ERROR: cc not found" >&2; exit 2; }

python3 "$here/gen-stubs.py"

cc -O2 -Wall -fPIC -shared -o "$LIB_FMOD" \
  "$GEN/gen_fmod.c" "$here/fmod_overrides.c"
cc -O2 -Wall -fPIC -shared -o "$LIB_STEAM" \
  "$GEN/gen_steam.c" "$here/steam_overrides.c"

# Verify: every implmap native name must be an exported symbol.
# (Absolute paths: this script runs from the repo root under the gate.)
python3 - "$LIB_FMOD" "$LIB_STEAM" "$here/celeste.implmap" \
  "$here/steamworks.implmap" <<'PY'
import re, subprocess, sys
def names(implmap):
    out = set()
    for line in open(implmap):
        m = re.search(r"\((\S+) (\S+)\)\s*$", line.strip())
        if m: out.add(m.group(1))
    return out
def exports(lib):
    out = subprocess.run(["nm", "-D", "--defined-only", lib],
                         capture_output=True, text=True).stdout
    return {ln.split()[-1] for ln in out.splitlines() if " T " in ln or " D " in ln}
fmod = names(sys.argv[3])
steam = names(sys.argv[4])
ef = exports(sys.argv[1]); es = exports(sys.argv[2])
miss_f = sorted(fmod - ef); miss_s = sorted(steam - es)
print(f"fmod: {len(fmod)} implmap names, {len(ef)} exports, missing {len(miss_f)}")
print(f"steam: {len(steam)} implmap names, {len(es)} exports, missing {len(miss_s)}")
if miss_f: print("MISSING FMOD:", miss_f[:10])
if miss_s: print("MISSING STEAM:", miss_s[:10])
sys.exit(1 if (miss_f or miss_s) else 0)
PY

ls -l "$LIB_FMOD" "$LIB_STEAM"
echo "fna-stubs: BUILD OK"
