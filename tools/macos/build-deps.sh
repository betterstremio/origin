#!/bin/bash
# The libraries the macOS build uses, prebuilt for macOS 11 and later (Homebrew's are built for the Mac they are
# installed on, so an app carrying them would not open on an older macOS):
#   SDL2.framework: SDL's own release (universal; the app is linked against it and carries it in Contents/Frameworks)
#   libmpv and its FFmpeg, libass and the rest: media-kit's build (github.com/media-kit/libmpv-darwin-build, the
#   "video-full" set: every decoder, VideoToolbox, no encoders), mpv 0.36.0. Loaded at run time by video_macos.c
#   mpv's headers at the same version, for building only
#   OpenSSL 3.5 (static, for macOS 11), which tools/build-engine.sh mac links into the torrent engine
# Output: $MB/{Frameworks,include/mpv,openssl}. Sources are pinned and checked.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../paths.sh"
SRC="$MB/src"; O="$MB"; mkdir -p "$SRC" "$O/Frameworks" "$O/include/mpv"
fetch() { # url sha256 file
  [ -f "$SRC/$3" ] || curl -fsSL -o "$SRC/$3" "$1"
  echo "$2  $SRC/$3" | shasum -a 256 -c - >/dev/null || { echo "checksum mismatch: $3"; shasum -a 256 "$SRC/$3"; exit 1; }
}
SDL_V=2.32.10; MK_V=v0.7.3; MPV_V=v0.36.0; OSSL_V=3.5.9
fetch https://github.com/libsdl-org/SDL/releases/download/release-$SDL_V/SDL2-$SDL_V.dmg 4a7ac31640d70214e848f994be8a12849c0f97918a7e6c2e27a40036166d1a7f sdl2.dmg
MK_F=libmpv-libs_${MK_V}_macos-arm64-video-full
fetch https://github.com/media-kit/libmpv-darwin-build/releases/download/$MK_V/$MK_F.tar.gz 9bb168ec908b4801f4231f411e3278a0aea644a2b03ce375c880d2813ab7f949 libmpv.tgz
fetch https://raw.githubusercontent.com/mpv-player/mpv/$MPV_V/libmpv/client.h 287181025de368777fa30f6f636c1915e77cc551ffe132993528f08ff4a64ae4 client.h
fetch https://raw.githubusercontent.com/mpv-player/mpv/$MPV_V/libmpv/render.h 0a93aa38bd3994f96860c1d68adafeba87c97b174456d13ec0b18a855180bd19 render.h
fetch https://raw.githubusercontent.com/mpv-player/mpv/$MPV_V/libmpv/render_gl.h 7bb28b46420b7c85247ac316cec5e2146469418f03d4c78d7b268beeeb9882f2 render_gl.h
fetch https://github.com/openssl/openssl/releases/download/openssl-$OSSL_V/openssl-$OSSL_V.tar.gz 603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a openssl.tgz

M="$SRC/mnt"; mkdir -p "$M"
hdiutil attach -nobrowse -readonly -mountpoint "$M" "$SRC/sdl2.dmg" >/dev/null
rm -rf "$O/Frameworks/SDL2.framework"; cp -R "$M/SDL2.framework" "$O/Frameworks/"
hdiutil detach "$M" >/dev/null
rm -rf "$SRC/libmpv"; mkdir -p "$SRC/libmpv"; tar -xzf "$SRC/libmpv.tgz" -C "$SRC/libmpv" --strip-components 1
rm -f "$O"/Frameworks/*.dylib; cp "$SRC"/libmpv/*.dylib "$O/Frameworks/"
cp "$SRC/client.h" "$SRC/render.h" "$SRC/render_gl.h" "$O/include/mpv/"
if [ ! -f "$O/openssl/lib/libssl.a" ]; then   # (a few minutes; kept once built)
  rm -rf "$SRC/openssl"; mkdir -p "$SRC/openssl"; tar -xzf "$SRC/openssl.tgz" -C "$SRC/openssl" --strip-components 1
  (cd "$SRC/openssl" && ./Configure darwin64-arm64-cc no-shared no-tests no-docs -mmacosx-version-min=11.0 \
     --prefix="$O/openssl" --libdir=lib && make -j"$JOBS" && make install_sw) > "$SRC/openssl.log" 2>&1 ||
    { tail -30 "$SRC/openssl.log"; echo "OpenSSL did not build (all of it: $SRC/openssl.log)"; exit 1; }
fi
echo "deps in $O: SDL $SDL_V, libmpv $MPV_V (media-kit $MK_V), OpenSSL $OSSL_V, $(ls "$O/Frameworks" | wc -l | tr -d ' ') items, $(du -sh "$O/Frameworks" | cut -f1)"
