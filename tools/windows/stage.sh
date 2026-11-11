#!/bin/bash
# A runnable copy of the Windows build, laid out as the installer will lay it out:
#   Origin.exe, libEGL.dll, libGLESv2.dll, libmpv-2.dll, appinfo.json (the version the updater reads), assets/
# tools/windows/stage.sh [DEST]   (WSL; default DEST: so/windows-run beside this checkout). Build first: tools/build.sh windows
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../paths.sh"
O="$BUILD_ROOT/windows/out"; DEST="${1:-$ROOT/../windows-run}"
[ -f "$O/Origin.exe" ] || { echo "no $O/Origin.exe: run tools/build.sh windows"; exit 1; }
mkdir -p "$DEST"
cp "$O/Origin.exe" "$DEST/"; cp -u "$O/libEGL.dll" "$O/libGLESv2.dll" "$O/libmpv-2.dll" "$DEST/"; [ -f "$O/libsbengine.dll" ] && cp -u "$O/libsbengine.dll" "$DEST/"; mkdir -p "$DEST/fallback"; cp -u "$O/fallback/vulkan-1.dll" "$DEST/fallback/"   # (-u: libmpv is big)
rm -rf "$DEST/assets"; cp -r "$ROOT/assets" "$DEST/assets"
printf '{\n  "id": "%s",\n  "version": "%s",\n  "title": "Origin"\n}\n' "$APPID" "$(tr -d '[:space:]' < "$ROOT/VERSION")" > "$DEST/appinfo.json"
du -sh "$DEST" | awk '{print "staged " $1}'
