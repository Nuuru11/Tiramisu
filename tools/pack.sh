#!/bin/bash
# tiramisu/tools/pack.sh — package + sign + verify (proven recipe).
# Self-signed cert subject MUST equal manifest Publisher (CN=TiramisuDev).
# Verify = osslsigncode with trust anchor + makemsix round-trip unpack +
# entry-point grep. Any doubt fails the build here, never on the TV.
set -euo pipefail
export PATH="$HOME/.local/bin:$PATH"
here="$(cd "$(dirname "$0")/.." && pwd)"; cd "$here"
out="${1:-out}"
[[ -x "$out/layout/tiramisu.exe" ]] || { echo "ERROR: run build-host.sh first" >&2; exit 2; }

# ---- stage package root: manifest + exe + guest + assets ---------------------
cp package/AppxManifest.xml "$out/layout/AppxManifest.xml"
mkdir -p "$out/layout/Assets"
cp tests/out/guest_hello.exe "$out/layout/guest_hello.exe"
# regenerate the real branding assets every pack (sources: package/branding)
python3 tools/gen-assets.py "$out/layout/Assets"
# CONSOLE-PROVEN envelope ONLY: the Xbox AppX deployment is picky about asset
# naming variants — the extra tiles (Square310x310, Wide310x150, per-scale
# Square44x44 subdirs) went in once and the console refused activation with
# 0x80070002. Strip everything the four-asset manifest envelope doesn't name.
find "$out/layout/Assets" -type f ! -name 'StoreLogo.png' \
  ! -name 'Square150x150Logo.png' ! -name 'Square44x44Logo.png' \
  ! -name 'SplashScreen.png' -delete
find "$out/layout/Assets" -type d -empty -delete

# ---- test cert (never committed; subject == Publisher) ----------------------
if [[ ! -f "$out/dev.pfx" ]]; then
  openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$out/dev.key" -out "$out/dev.pem" -days 825 \
    -subj "/CN=TiramisuDev" \
    -addext "basicConstraints=critical,CA:FALSE" \
    -addext "keyUsage=critical,digitalSignature" \
    -addext "extendedKeyUsage=codeSigning,1.3.6.1.5.5.7.3.3" 2>/dev/null
  openssl pkcs12 -export -out "$out/dev.pfx" -inkey "$out/dev.key" \
    -in "$out/dev.pem" -passout pass:tiramisu
  openssl x509 -in "$out/dev.pem" -outform DER -out "$out/dev.cer"
fi

# ---- pack + sign -------------------------------------------------------------
rm -f "$out/Tiramisu-unsigned.msix" "$out/Tiramisu.msix"
makemsix pack -d "$out/layout" -p "$out/Tiramisu-unsigned.msix"
osslsigncode sign -pkcs12 "$out/dev.pfx" -pass tiramisu -h sha256 \
  -in "$out/Tiramisu-unsigned.msix" -out "$out/Tiramisu.msix"

# ---- verify (fail the build on any doubt) ------------------------------------
openssl x509 -in "$out/dev.cer" -inform DER -out "$out/dev-ca.pem" -outform PEM
osslsigncode verify -in "$out/Tiramisu.msix" -CAfile "$out/dev-ca.pem" \
  | grep -q 'Signature verification: ok' \
  || { echo "FATAL: signature verification FAILED" >&2; exit 4; }
rm -rf /tmp/msix-rt.$$ && makemsix unpack -p "$out/Tiramisu.msix" -d "/tmp/msix-rt.$$" -ac >/dev/null 2>&1 \
  || { echo "FATAL: round-trip unpack FAILED" >&2; exit 4; }
grep -q 'Executable="tiramisu.exe"' "/tmp/msix-rt.$$/AppxManifest.xml" \
  || { echo "FATAL: packed manifest lost the entry point" >&2; exit 4; }
[[ -f "/tmp/msix-rt.$$/guest_hello.exe" ]] \
  || { echo "FATAL: guest exe missing from package" >&2; exit 4; }
rm -rf "/tmp/msix-rt.$$"

echo "---- done ----"
ls -l "$out/Tiramisu.msix" "$out/dev.cer"
echo
echo "Deploy:  DP_HOST=<console-ip> DP_PASS=<portal-pass> $here/tools/deploy.sh install $out/Tiramisu.msix $out/dev.cer"
