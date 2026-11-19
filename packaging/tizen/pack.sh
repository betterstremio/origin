#!/bin/bash
# Build the Samsung TV preview for Apps2Samsung. OUT defaults to dist/.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../../tools/paths.sh"

P="$ROOT/packaging/tizen"
B="$BUILD_ROOT/tizen"
DOTNET="${DOTNET:-$(command -v dotnet || echo "$HOME/.dotnet/dotnet")}"
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1   # (the SDK needs no ICU this way)
V="$(cat "$ROOT/VERSION")"
[[ "$V" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "tizen: invalid VERSION: $V" >&2; exit 1; }
command -v "$DOTNET" >/dev/null || { echo "tizen: .NET SDK not found (set DOTNET)" >&2; exit 1; }

ASSET_ID=io.github.betterstremio.origin
APPID="$ASSET_ID" bash "$ROOT/tools/build.sh" tizen "$@"
bash "$ROOT/tools/build-engine.sh" tizen
ENGINE="$B/libsbengine.so"
[ -s "$ENGINE" ] || { echo "tizen: libsbengine.so is required at $ENGINE" >&2; exit 1; }

O="${OUT:-$ROOT/dist}"
mkdir -p "$O"
S="$(mktemp -d "$B/package.XXXXXXXX")"
A="$(mktemp -d "$O/.tizen.XXXXXXXX")"
trap 'rm -rf "$S" "$A"' EXIT
mkdir -p "$S"/{hostbin,lib,res/art,shared/res}
sed "s/@VERSION@/$V/g" "$P/tizen-manifest.xml" > "$S/tizen-manifest.xml"
PKG="$(sed -n 's/.* package="\([^"]*\)" .*/\1/p' "$S/tizen-manifest.xml")"
ID="$(sed -n 's/.* appid="\([^"]*\)" .*/\1/p' "$S/tizen-manifest.xml")"
FILE="${ASSET_ID}_${V}_tizen-beta.tpk"

cp "$B/libstremioorigin.so" "$S/lib/"
cp "$B/libstremioorigin.so" "$ENGINE" "$P/silence.wav" "$S/res/"
cp -R "$ROOT/assets/." "$S/res/art/"
cp "$ROOT/assets/icon.png" "$S/shared/res/Origin.png"
mkdir -p "$S/res/licenses"
cp "$ROOT/LICENSE" "$S/res/licenses/"
cp -R "$ROOT/third_party/licenses/." "$S/res/licenses/"
printf '{"id":"%s","version":"%s"}\n' "$ID" "$V" > "$S/res/appinfo.json"

cp "$P/"*.cs "$P/StremioOrigin.csproj" "$S/"
cp -R "$P/start" "$S/"
mkdir -p "$S/hosts"
cp "$P/hosts/HostNui.cs" "$S/hosts/"
for host in A B C D; do
  dir="$(printf '%s' "$host" | tr '[:upper:]' '[:lower:]')"
  mkdir -p "$S/hosts/$dir"
  cp "$P/hosts/$dir/Host$host.csproj" "$S/hosts/$dir/"
  "$DOTNET" build "$S/hosts/$dir/Host$host.csproj" -c Release -nologo -v quiet -o "$S/managed/Host$host"
  cp "$S/managed/Host$host/StremioOrigin.Host$host.dll" "$S/hostbin/"
done
"$DOTNET" build "$S/StremioOrigin.csproj" -c Release -nologo -v quiet -o "$S/managed/picker/"
RAW="$S/managed/picker/$PKG-$V.tpk"
[ -s "$RAW" ] || { echo "tizen: .NET SDK did not produce $RAW" >&2; exit 1; }
unzip -p "$RAW" res/libsbengine.so | cmp -s "$ENGINE" - || {
  echo "tizen: the TPK does not contain the built libsbengine.so" >&2
  exit 1
}
mv "$RAW" "$A/$FILE"
(cd "$A" && shasum -a 256 "$FILE" > "$FILE.sha256")
mv "$A/$FILE" "$A/$FILE.sha256" "$O/"
printf 'Built %s\n' "$O/$FILE"
