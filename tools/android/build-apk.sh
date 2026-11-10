#!/bin/bash
# The Android APK (Linux or WSL), after tools/android/build-deps.sh and tools/build-engine.sh android-<abi>:
#   tools/android/build-apk.sh            release: <appid>_<ver>_android-arm64.apk and _android-armv7.apk, one per CPU type,
#                                         and _android.apk with both, the only file that installed 2.1.2 and older apps recognise
#   tools/android/build-apk.sh --dev      dev build (SB_* test settings from the launch intent), + x86 for the emulator
#   --clean                               compile everything again (always for a release)
# Every build is signed with the release key ($ORIGIN_BUILD/keys/android-release.jks, made on the first run): an
# update only installs over an APK signed with the same key, so that file must never be lost.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../paths.sh"
OB="$ORIGIN_BUILD"; SDK="${ANDROID_HOME:-$HOME/android-sdk}"; BA="$BUILD_ROOT/android"
DEV=0; CLEAN=""; for a in "$@"; do case "$a" in --dev) DEV=1;; --clean) CLEAN=--clean;; esac; done
if [ $DEV = 1 ]; then ABIS="${ABIS:-arm64-v8a armeabi-v7a x86}"; else ABIS="${ABIS:-arm64-v8a armeabi-v7a}"; fi
VER="${SB_VERSION:-$(cat "$ROOT/VERSION")}"   # SB_VERSION=x.y.z: another version (the updater test builds a newer one)
APPID=tv.stremioorigin.app; mkdir -p "$BA"; APPINFO="$BA/appinfo.json"
sed "s/\"version\": \"0.0.0\"/\"version\": \"$VER\"/" "$ROOT/packaging/webos/appinfo.json" > "$APPINFO"

bash "$ROOT/tools/build.sh" android $ABIS $CLEAN

# ---- the Gradle project, staged outside the repo ----
G="$BA/gradle"; mkdir -p "$G"
rm -rf "$G/app/src" "$G/app/build/sdl-java"; mkdir -p "$G/app"
P="$ROOT/packaging/android"; cp -r "$P/settings.gradle" "$P/build.gradle" "$P/gradle.properties" "$G/"
cp "$P/app/build.gradle" "$P/app/proguard-rules.pro" "$G/app/"; cp -r "$P/app/src" "$G/app/"
mkdir -p "$G/app/build"; cp -r "$AB/deps/sdl-java" "$G/app/build/sdl-java"
for ABI in $ABIS; do J="$G/app/src/main/jniLibs/$ABI"; mkdir -p "$J"; cp "$BA/out/$ABI"/{libmain.so,libSDL2.so,libcurl.so} "$J/"
  [ -f "$BA/out/$ABI/libsbengine.so" ] && cp "$BA/out/$ABI/libsbengine.so" "$J/"; done
A="$G/app/src/main/assets/app"; mkdir -p "$A"; cp -r "$ROOT/assets" "$A/assets"; cp "$APPINFO" "$A/appinfo.json"
# the launcher icon and the TV banner, drawn from the webOS icons
python3 - "$ROOT" "$G/app/src/main/res" <<'PY'
import sys, os
from PIL import Image
root, res = sys.argv[1], sys.argv[2]
icon = Image.open(os.path.join(root, 'packaging/webos/largeIcon.png')).convert('RGBA')
for d, s in (('mdpi', 48), ('hdpi', 72), ('xhdpi', 96), ('xxhdpi', 144), ('xxxhdpi', 192)):
    os.makedirs(f'{res}/mipmap-{d}', exist_ok=True); icon.resize((s, s), Image.LANCZOS).save(f'{res}/mipmap-{d}/ic_launcher.png')
os.makedirs(f'{res}/drawable-xhdpi', exist_ok=True)
b = Image.new('RGBA', (320, 180), (12, 11, 17, 255))
logo = Image.open(os.path.join(root, 'assets/logo.png')).convert('RGBA'); k = min(260 / logo.width, 120 / logo.height); logo = logo.resize((max(1, int(logo.width * k)), max(1, int(logo.height * k))), Image.LANCZOS)
b.alpha_composite(logo, ((320 - logo.width) // 2, (180 - logo.height) // 2)); b.convert('RGB').save(f'{res}/drawable-xhdpi/banner.png')
PY

# ---- the signing key (once) ----
mkdir -p "$OB/keys"; KS="$OB/keys/android-release.jks"; KP="$OB/keys/android-release.pass"
if [ ! -f "$KS" ]; then
  head -c 24 /dev/urandom | base64 | tr -d '/+=' > "$KP"; chmod 600 "$KP"
  keytool -genkeypair -keystore "$KS" -storetype PKCS12 -alias stremioorigin -keyalg RSA -keysize 4096 -validity 36500 \
    -storepass "$(cat "$KP")" -keypass "$(cat "$KP")" -dname "CN=Stremio Origin" >/dev/null 2>&1
  echo "== new release key: $KS (back it up: updates need it)"
fi

# ---- Gradle ----
GV=8.9; GH="$AB/gradle-$GV"
if [ ! -x "$GH/bin/gradle" ]; then curl -fsSL -o "$AB/gradle.zip" "https://services.gradle.org/distributions/gradle-$GV-bin.zip"
  echo "d725d707bfabd4dfdc958c624003b3c80accc03f7037b5122c4b1d0ef15cecab  $AB/gradle.zip" | sha256sum -c - >/dev/null; (cd "$AB" && unzip -qo gradle.zip); fi
echo "sdk.dir=$SDK" > "$G/local.properties"
cd "$G"; rm -rf app/build/outputs/apk
SB_ABIS="$ABIS" SB_APPINFO="$APPINFO" SB_KEYSTORE="$KS" SB_KEYSTORE_PASS="$(cat "$KP")" \
  "$GH/bin/gradle" --no-daemon -q assembleRelease $([ $DEV = 1 ] && echo -Pdev) 2>&1 | grep -v "^$" | grep -vi "warning: \[options\]\|deprecat" || true
OUTS="$G/app/build/outputs/apk/release"; mkdir -p "$BA/apk"
one() {   # the Gradle output, the suffix in its release name
  [ -f "$OUTS/$1" ] || { echo "APK FAILED: no $1"; exit 1; }
  local n="${APPID}_${VER}_$2.apk"; cp "$OUTS/$1" "$BA/apk/$n"
  "$SDK/build-tools/35.0.0/apksigner" verify "$BA/apk/$n" && echo "signed ok"
  ls -l "$BA/apk/$n" | awk '{printf "%s  %.1f MB\n", $NF, $5/1048576}'; sha256sum "$BA/apk/$n" | cut -c1-64
}
if [ $DEV = 1 ]; then one app-release.apk android_dev
else
  for ABI in $ABIS; do case "$ABI" in arm64-v8a) k=arm64;; armeabi-v7a) k=armv7;; *) k=$ABI;; esac; one "app-$ABI-release.apk" "android-$k"; done
  one app-universal-release.apk android
fi
echo APK_OK
