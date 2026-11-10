#!/bin/bash
# The libraries the Windows build links against, cross-compiled in Linux or WSL with mingw-w64 (posix threads):
#   SDL2, zlib, mbedTLS, curl (all static, so the exe needs no runtime DLLs of its own)
#   ANGLE (libEGL.dll, libGLESv2.dll): GLES2 on Direct3D 11, taken from a pinned Electron release (the version VS Code 1.145
#   ships; Electron 44 builds ANGLE into its exe and has no separate DLLs)
# Output: $WB/deps/{include,lib,bin}. Sources are pinned and checked.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../paths.sh"
X=x86_64-w64-mingw32; CC="$X-gcc-posix"; CXX="$X-g++-posix"
SRC="$WB/src"; O="$WB/deps"; mkdir -p "$SRC" "$O/include" "$O/lib" "$O/bin"
fetch() { # url sha256 file
  [ -f "$SRC/$3" ] || curl -fsSL -o "$SRC/$3" "$1"
  echo "$2  $SRC/$3" | sha256sum -c - >/dev/null || { echo "checksum mismatch: $3"; sha256sum "$SRC/$3"; exit 1; }
}
SDL_V=2.30.9; ZLIB_V=1.3.1; MBED_V=3.6.2; CURL_V=8.11.0; ELECTRON_V=43.8.3
fetch https://github.com/libsdl-org/SDL/releases/download/release-$SDL_V/SDL2-$SDL_V.tar.gz 24b574f71c87a763f50704bbb630cbe38298d544a1f890f099a4696b1d6beba4 sdl.tgz
fetch https://zlib.net/fossils/zlib-$ZLIB_V.tar.gz 9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23 zlib.tgz
fetch https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBED_V/mbedtls-$MBED_V.tar.bz2 8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca mbedtls.tbz
fetch https://curl.se/download/curl-$CURL_V.tar.xz db59cf0d671ca6e7f5c2c5ec177084a33a79e04c97e71cf183a5cdea235054eb curl.txz
fetch https://github.com/electron/electron/releases/download/v$ELECTRON_V/electron-v$ELECTRON_V-win32-x64.zip 98efbd2d1c7bd44150c0aa800cd9ac5fefcefd2d44fd94958a95d6744d86c6de electron.zip
for p in sdl.tgz:SDL2-$SDL_V zlib.tgz:zlib-$ZLIB_V mbedtls.tbz:mbedtls-$MBED_V curl.txz:curl-$CURL_V; do
  IFS=: read -r f d <<<"$p"; [ -d "$SRC/$d" ] || tar -xf "$SRC/$f" -C "$SRC"; done

TC="$WB/mingw.cmake"
cat > "$TC" <<T
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER $CC)
set(CMAKE_CXX_COMPILER $CXX)
set(CMAKE_RC_COMPILER $X-windres)
set(CMAKE_FIND_ROOT_PATH /usr/$X $O)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
T
C=(-G Ninja -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$O" -DCMAKE_C_FLAGS="-O2")
if [ ! -f "$O/lib/libSDL2.a" ]; then echo "== SDL2"
  cmake -S "$SRC/SDL2-$SDL_V" -B "$WB/b/sdl" "${C[@]}" -DSDL_STATIC=ON -DSDL_SHARED=OFF -DSDL_TEST=OFF -DSDL_RENDER_D3D=OFF -DSDL_VULKAN=OFF >/dev/null
  ninja -C "$WB/b/sdl" install >/dev/null; fi
if [ ! -f "$O/lib/libz.a" ]; then echo "== zlib"
  cmake -S "$SRC/zlib-$ZLIB_V" -B "$WB/b/zlib" "${C[@]}" >/dev/null
  ninja -C "$WB/b/zlib" install >/dev/null; cp "$O/lib/libzlibstatic.a" "$O/lib/libz.a"; rm -f "$O"/lib/libzlib.dll.a "$O"/bin/libzlib*.dll; fi
if [ ! -f "$O/lib/libmbedtls.a" ]; then echo "== mbedTLS"
  cmake -S "$SRC/mbedtls-$MBED_V" -B "$WB/b/mbed" "${C[@]}" -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DMBEDTLS_FATAL_WARNINGS=OFF >/dev/null
  ninja -C "$WB/b/mbed" install >/dev/null; fi
if [ ! -f "$O/lib/libcurl.a" ]; then echo "== curl"
  cmake -S "$SRC/curl-$CURL_V" -B "$WB/b/curl" "${C[@]}" -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF \
    -DCURL_USE_MBEDTLS=ON -DCURL_USE_SCHANNEL=OFF -DCURL_USE_OPENSSL=OFF -DMBEDTLS_INCLUDE_DIR="$O/include" -DMBEDTLS_LIBRARY="$O/lib/libmbedtls.a" -DMBEDX509_LIBRARY="$O/lib/libmbedx509.a" -DMBEDCRYPTO_LIBRARY="$O/lib/libmbedcrypto.a" \
    -DZLIB_INCLUDE_DIR="$O/include" -DZLIB_LIBRARY="$O/lib/libz.a" \
    -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DUSE_NGHTTP2=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_LIBIDN2=OFF -DUSE_WIN32_IDN=OFF -DCURL_DISABLE_LDAP=ON \
    -DENABLE_THREADED_RESOLVER=ON -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=none -DCURL_DISABLE_SRP=ON >/dev/null
  ninja -C "$WB/b/curl" install >/dev/null; fi
if [ ! -f "$O/lib/libGLESv2.dll.a" ]; then echo "== ANGLE (Electron $ELECTRON_V)"
  rm -rf "$WB/b/angle"; mkdir -p "$WB/b/angle"
  (cd "$WB/b/angle" && unzip -q -o "$SRC/electron.zip" libEGL.dll libGLESv2.dll LICENSES.chromium.html)
  cp "$WB/b/angle/libEGL.dll" "$WB/b/angle/libGLESv2.dll" "$O/bin/"
  gzip -9 -c "$WB/b/angle/LICENSES.chromium.html" > "$O/bin/LICENSES.chromium.html.gz"
  { echo "LIBRARY libGLESv2.dll"; echo EXPORTS; $X-objdump -p "$O/bin/libGLESv2.dll" | awk '$NF ~ /^gl[A-Za-z0-9_]+$/ {print $NF}' | sort -u; } > "$WB/b/angle/GLESv2.def"
  $X-dlltool -d "$WB/b/angle/GLESv2.def" -l "$O/lib/libGLESv2.dll.a"
  # the Khronos headers (GLES2, EGL, KHR) from SDL's source tree
  for d in GLES2 EGL KHR; do mkdir -p "$O/include/$d"; cp "$SRC/SDL2-$SDL_V/src/video/khronos/$d/"*.h "$O/include/$d/"; done
fi
ls "$O/lib" "$O/bin" | tr '\n' ' '; echo
echo DEPS_OK
