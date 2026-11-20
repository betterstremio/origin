#!/bin/bash
# packaging/macos/pack.sh [--clean]: Origin for macOS (Apple Silicon, macOS 11 or later) compiled, then in OUT (default
# dist/):
#   <appid>_<version>_macos-arm64.dmg (+ .sha256)   Origin.app to drag to Applications; the app's updater looks for it
#                                                    (SB_PKG_KIND)
# Run on a Mac. Needs tools/macos/build-deps.sh, and tools/build-engine.sh mac for torrents (without it the app plays
# every other stream and hides torrents). Use --clean for a release.
# Not signed with a Developer ID: the app is signed ad hoc (Apple Silicon runs nothing unsigned), so macOS asks once
# before it opens a downloaded copy (README: Download).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../../tools/paths.sh"
[ "$(uname -s)" = Darwin ] || { echo "the macOS package is made on a Mac"; exit 1; }
bash "$ROOT/tools/build.sh" macos "$@"
V="$(tr -d '[:space:]' < "$ROOT/VERSION")"; B="$BUILD_ROOT/macos"; P="$BUILD_ROOT/macos-pkg"; A="$P/dmg/Origin.app"
OUT="${OUT:-$ROOT/dist}"
ENGINE="${ENGINE:-$ROOT/build/mac/libsbengine.dylib}"   # (tools/build-engine.sh mac)
rm -rf "$P"; mkdir -p "$A/Contents/MacOS" "$A/Contents/Resources" "$A/Contents/Frameworks" "$OUT"

# the binary, and the libraries it links (SDL2) or loads (libmpv and its own)
cp "$B/Origin" "$A/Contents/MacOS/Origin"
strip -x "$A/Contents/MacOS/Origin"
cp -R "$MB/Frameworks/SDL2.framework" "$A/Contents/Frameworks/"
rm -rf "$A/Contents/Frameworks/SDL2.framework/Versions/A/Headers" "$A/Contents/Frameworks/SDL2.framework/Headers"
cp "$MB"/Frameworks/*.dylib "$A/Contents/Frameworks/"
if [ -f "$ENGINE" ]; then   # (p2p.c looks beside the assets: a link there to the library with the others)
  cp "$ENGINE" "$A/Contents/Frameworks/libsbengine.dylib"
  ln -s ../Frameworks/libsbengine.dylib "$A/Contents/Resources/libsbengine.dylib"
else echo "WARNING: no P2P engine at $ENGINE (tools/build-engine.sh mac): this package hides torrents"; fi

# what the app reads: assets, its version (update.c: appinfo.json beside the assets), the licences
cp -R "$ROOT/assets" "$A/Contents/Resources/assets"
printf '{\n  "id": "%s",\n  "version": "%s",\n  "title": "Origin"\n}\n' "$APPID" "$V" > "$A/Contents/Resources/appinfo.json"
L="$A/Contents/Resources/licenses"; cp -R "$ROOT/third_party/licenses" "$L"
rm -rf "$L/dovi" "$L/mingw-w64-runtime-copyright.txt" "$L/mpv-SOURCE.txt"; cp "$ROOT/LICENSE" "$L/Origin-GPL-3.0.txt"

# the icon, from the 256-pixel PNG every package uses
I="$P/Origin.iconset"; mkdir -p "$I"
for s in 16 32 64 128 256; do sips -z $s $s "$ROOT/assets/icon.png" --out "$I/icon_${s}x${s}.png" >/dev/null; done
for s in 16 32 128; do cp "$I/icon_$((s * 2))x$((s * 2)).png" "$I/icon_${s}x${s}@2x.png"; done
rm -f "$I/icon_64x64.png"
iconutil -c icns "$I" -o "$A/Contents/Resources/Origin.icns"

sed -e "s/@VERSION@/$V/g" -e "s/@APPID@/$APPID/g" "$ROOT/packaging/macos/Info.plist" > "$A/Contents/Info.plist"
printf 'APPL????' > "$A/Contents/PkgInfo"

# signed ad hoc, inside out (each library, the framework, then the app)
for f in "$A"/Contents/Frameworks/*.dylib; do codesign --force --sign - --timestamp=none "$f"; done
codesign --force --sign - --timestamp=none "$A/Contents/Frameworks/SDL2.framework"
codesign --force --sign - --timestamp=none "$A"   # (no hardened runtime: it would refuse libraries signed ad hoc)
codesign --verify --strict --deep "$A"

# nothing outside the app or the system may be named in a library's links (a Homebrew path would work only here)
for f in "$A/Contents/MacOS/Origin" "$A"/Contents/Frameworks/*.dylib; do
  if otool -L "$f" | tail -n +2 | awk '{print $1}' | grep -vE '^(/usr/lib/|/System/Library/|@rpath/|@loader_path/|@executable_path/)'; then
    echo "STOPPED: $(basename "$f") links a library outside the app"; exit 1
  fi
done

ln -s /Applications "$P/dmg/Applications"
DMG="$OUT/${APPID}_${V}_macos-arm64.dmg"; rm -f "$DMG"
hdiutil create -quiet -volname "Origin $V" -srcfolder "$P/dmg" -fs HFS+ -format ULFO -ov "$DMG"
(cd "$OUT" && shasum -a 256 "$(basename "$DMG")" > "$(basename "$DMG").sha256")
ls -l "$DMG" | awk '{printf "  %-62s %6.1f MB\n", $NF, $5/1048576}'
