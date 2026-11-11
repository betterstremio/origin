#!/bin/bash
# packaging/windows/pack.sh [--clean]: Origin for Windows compiled, then in OUT (default dist/):
#   <appid>_<version>_windows_setup.exe (+ .sha256)   the installer the app's updater looks for (SB_PKG_KIND)
#   <appid>_<version>_windows_portable.zip            the same files to unpack anywhere (settings still in %LOCALAPPDATA%)
# Needs tools/windows/build-deps.sh, tools/build-engine.sh windows and makensis (apt install nsis). Use --clean for a release.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../../tools/paths.sh"
bash "$ROOT/tools/build.sh" windows "$@"
V="$(tr -d '[:space:]' < "$ROOT/VERSION")"; O="$BUILD_ROOT/windows/out"; P="$BUILD_ROOT/windows/pkg"; S="$P/Origin"; OUT="${OUT:-$ROOT/dist}"
[ -f "$WB/engine/libsbengine.dll" ] || { echo "no P2P engine: tools/build-engine.sh windows"; exit 1; }
rm -rf "$P"; mkdir -p "$S" "$OUT"
cp "$O/Origin.exe" "$O/libEGL.dll" "$O/libGLESv2.dll" "$O/libmpv-2.dll" "$WB/engine/libsbengine.dll" "$S/"
mkdir -p "$S/fallback"; cp "$O/fallback/vulkan-1.dll" "$S/fallback/"   # (used only on a PC without a Vulkan loader of its own)
cp -r "$ROOT/assets" "$S/assets"
cp -r "$ROOT/third_party/licenses" "$S/licenses"; rm -rf "$S/licenses/dovi"; cp "$WB/deps/bin/LICENSES.chromium.html.gz" "$S/licenses/"
cp "$ROOT/LICENSE" "$S/licenses/Origin-GPL-3.0.txt"
printf '{\n  "id": "%s",\n  "version": "%s",\n  "title": "Origin"\n}\n' "$APPID" "$V" > "$S/appinfo.json"
SETUP="$OUT/${APPID}_${V}_windows_setup.exe"; ZIP="$OUT/${APPID}_${V}_windows_portable.zip"
makensis -V2 -DVERSION="$V" -DSRC="$S" -DICON="$BUILD_ROOT/windows/build/origin.ico" -DOUT="$SETUP" "$ROOT/packaging/windows/installer.nsi"
(cd "$OUT" && sha256sum "$(basename "$SETUP")" > "$(basename "$SETUP").sha256")
rm -f "$ZIP"; python3 -I -c 'import sys, os, zipfile
src, out = sys.argv[1], sys.argv[2]; base = os.path.dirname(src)
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for d, _, fs in os.walk(src):
        for f in sorted(fs): p = os.path.join(d, f); z.write(p, os.path.relpath(p, base))' "$S" "$ZIP"
ls -l "$SETUP" "$ZIP" | awk '{printf "  %-62s %6.1f MB\n", $NF, $5/1048576}'
