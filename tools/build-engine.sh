#!/bin/bash
# Builds the torrent engine library the app loads when a torrent is first played:
#   tools/build-engine.sh webos  -> build/webos/libsbengine.so     (ARMv7 soft-float, glibc, libc++ inside)
#   tools/build-engine.sh mac    -> build/mac/libsbengine.dylib    (the Mac preview)
#   tools/build-engine.sh windows -> $WB/engine/libsbengine.dll    (x64, mingw-w64)
# Engine: github.com/NuvioMedia/nuvio-engine (GPL-3.0) at a pinned commit, libtorrent 2.0.12, Boost
# 1.86, plus tools/engine/engine.patch (streaming window that frees far pieces from flash, live rate
# limits, verified pieces served from disk in 256 KB reads, no UPnP/NAT-PMP, a window that scales down to
# 24 MB and can change while playing, libtorrent settings by name, payload deleted with its torrent). No OpenSSL: peer encryption works,
# HTTPS trackers do not. Sources and build folders live in $ORIGIN_BUILD/engine.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ORIGIN_BUILD="${ORIGIN_BUILD:-$HOME/origin-build}"   # the private build folder outside the repo: toolchain, SDK, sources, keys
TARGET="${1:-webos}"
W="${ENGINE_WORK:-$ORIGIN_BUILD/engine}"; mkdir -p "$W"
ZIG="${ZIG:-$ORIGIN_BUILD/zig-aarch64-macos-0.17.0/zig}"
COMMIT=99a1860
SRC="$W/src"
if [ ! -d "$SRC/.git" ]; then git clone -q https://github.com/NuvioMedia/nuvio-engine "$SRC"; fi
git -C "$SRC" checkout -q "$COMMIT"; git -C "$SRC" reset -q --hard "$COMMIT"; git -C "$SRC" clean -qfd -- src include tests cmake
git -C "$SRC" apply --whitespace=nowarn "$ROOT/tools/engine/engine.patch"
# Windows without OpenSSL, as on webOS and Android: peer encryption works, HTTPS trackers do not (the engine's Windows
# branch would require OpenSSL for its certificate check, which only an OpenSSL build of libtorrent calls)
if [ "${1:-}" = windows ]; then git -C "$SRC" apply --whitespace=nowarn "$ROOT/tools/engine/windows.patch"; sed -i 's/^    elseif(WIN32)$/    elseif(WIN32 AND NUVIO_ENGINE_WINDOWS_OPENSSL)/' "$SRC/CMakeLists.txt"; grep -q "WIN32 AND NUVIO_ENGINE_WINDOWS_OPENSSL" "$SRC/CMakeLists.txt" || { echo "engine CMakeLists changed: the Windows OpenSSL branch was not found"; exit 1; }; fi
DEPS="${ENGINE_DEPS:-$W/deps}"; mkdir -p "$DEPS"
if [ ! -f "$DEPS/nuvio_boost-src/boost/version.hpp" ]; then
  curl -fsSL -o "$DEPS/boost.tar.bz2" https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.bz2
  echo "1bed88e40401b2cb7a1f76d4bab499e352fa4d0c5f31c0dbae64e24d34d7513b  $DEPS/boost.tar.bz2" | shasum -a 256 -c -
  rm -rf "$DEPS/nuvio_boost-src"; mkdir -p "$DEPS/nuvio_boost-src"; tar -xjf "$DEPS/boost.tar.bz2" -C "$DEPS/nuvio_boost-src" --strip-components 1; rm -f "$DEPS/boost.tar.bz2"
fi
if [ ! -d "$DEPS/nuvio_libtorrent-src/.git" ]; then
  git clone -q https://github.com/arvidn/libtorrent.git "$DEPS/nuvio_libtorrent-src"
  git -C "$DEPS/nuvio_libtorrent-src" checkout -q 740a0b9aeabe00e762cc0efe4a0f27593db2550b
  git -C "$DEPS/nuvio_libtorrent-src" submodule update -q --init deps/try_signal
fi
case "$TARGET" in
  webos) ZT="-target arm-linux-gnueabi.${GLIBC:-2.17} -mcpu=cortex_a9-neon -mfloat-abi=softfp"; OUT="$ROOT/build/webos/libsbengine.so"; SYS=Linux; PROC=arm ;;   # soft-float ABI like the app; this compiler drops NEON from the assembler there, so libtorrent takes its portable paths
  mac)   ZT=""; OUT="$ROOT/build/mac/libsbengine.dylib"; SYS=Darwin; PROC=aarch64 ;;
  android-*) ABI="${TARGET#android-}"; OUT="${ENGINE_OUT:-$ROOT/build/android/$ABI/libsbengine.so}"; NDK="${NDK:-$HOME/android-sdk/ndk/27.2.12479018}"; API="${API:-24}" ;;   # arm64-v8a, armeabi-v7a, x86 (the emulator)
  windows) OUT="${ENGINE_OUT:-${WB:-$HOME/wbuild}/engine/libsbengine.dll}"; SYS=Windows; PROC=x86_64 ;;   # mingw-w64 (posix threads), as the app
  *) echo "unknown target $TARGET"; exit 1 ;;
esac
TC="$W/tc-$TARGET"; mkdir -p "$TC"
if [[ "$TARGET" == android-* ]]; then   # the NDK's own toolchain file; its clang links the library
  case "$ABI" in arm64-v8a) TRIPLE=aarch64-linux-android;; armeabi-v7a) TRIPLE=armv7a-linux-androideabi;; x86) TRIPLE=i686-linux-android;; x86_64) TRIPLE=x86_64-linux-android;; *) echo "unknown ABI $ABI"; exit 1;; esac
  NDKBIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
  printf '#!/bin/sh
exec "%s" --target=%s%s "$@"
' "$NDKBIN/clang++" "$TRIPLE" "$API" > "$TC/c++"; chmod 755 "$TC/c++"
  printf 'include("%s")
' "$NDK/build/cmake/android.toolchain.cmake" > "$TC/toolchain.cmake"
  TCX=(-DANDROID_ABI="$ABI" -DANDROID_PLATFORM="android-$API" -DANDROID_STL=c++_static)
elif [ "$TARGET" = windows ]; then
TCX=(); X=x86_64-w64-mingw32
for t in cc:gcc-posix c++:g++-posix ar:ar ranlib:ranlib; do printf '#!/bin/sh\nexec %s-%s "$@"\n' "$X" "${t#*:}" > "$TC/${t%%:*}"; chmod 755 "$TC/${t%%:*}"; done
cat > "$TC/toolchain.cmake" <<T
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER $TC/cc)
set(CMAKE_CXX_COMPILER $TC/c++)
set(CMAKE_RC_COMPILER $X-windres)
set(CMAKE_AR $TC/ar)
set(CMAKE_RANLIB $TC/ranlib)
set(CMAKE_FIND_ROOT_PATH /usr/$X)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
T
else
TCX=()
printf '#!/bin/sh\nexec "%s" cc %s "$@"\n' "$ZIG" "$ZT" > "$TC/cc"; printf '#!/bin/sh\nexec "%s" c++ %s "$@"\n' "$ZIG" "$ZT" > "$TC/c++"
printf '#!/bin/sh\nexec "%s" ar "$@"\n' "$ZIG" > "$TC/ar"; printf '#!/bin/sh\nexec "%s" ranlib "$@"\n' "$ZIG" > "$TC/ranlib"
chmod 755 "$TC/cc" "$TC/c++" "$TC/ar" "$TC/ranlib"
cat > "$TC/toolchain.cmake" <<T
set(CMAKE_SYSTEM_NAME $SYS)
set(CMAKE_SYSTEM_PROCESSOR $PROC)
set(CMAKE_C_COMPILER $TC/cc)
set(CMAKE_CXX_COMPILER $TC/c++)
set(CMAKE_AR $TC/ar)
set(CMAKE_RANLIB $TC/ranlib)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
T
fi
B="$W/build-$TARGET"
SSL=(-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON); SSLLIB=()
if [ "$TARGET" = mac ]; then OSSL="$(brew --prefix openssl@3)"; SSL=(-DOPENSSL_ROOT_DIR="$OSSL"); SSLLIB=("$OSSL/lib/libssl.a" "$OSSL/lib/libcrypto.a"); export PKG_CONFIG_PATH="$OSSL/lib/pkgconfig"; fi   # the engine needs it on macOS (system trust store)
FL="-ffunction-sections -fdata-sections -fvisibility=hidden -fvisibility-inlines-hidden -D_LARGEFILE64_SOURCE -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64 -ffile-prefix-map=$HOME=/src -ffile-prefix-map=$ROOT=/app"   # no local paths in the library (source names in asserts)
if [[ "$TARGET" == android-* ]]; then FL="$FL -fexperimental-library"; fi   # the NDK's libc++ 18 keeps std::stop_token behind it
if [ "$TARGET" = windows ]; then FL="$FL -DESTALE=10070"; fi   # (mingw has no ESTALE: Winsock's WSAESTALE; it only marks a vanished drive)
if [ "$TARGET" = mac ]; then mkdir -p "$W/ossl3"; ln -sfn "$OSSL/include/openssl" "$W/ossl3/openssl"; FL="$FL -I$W/ossl3"; fi   # OpenSSL 3 ahead of the compiler's own /opt/homebrew/include (OpenSSL 4 there does not build the pinned libtorrent; the same folder given twice is dropped, hence the link)
[ "$TARGET" = mac ] && [ -n "${ENGINE_MAC_PREBUILT:-}" ] || cmake -S "$SRC" -B "$B" -G Ninja -DNUVIO_ENGINE_ENABLE_LIBTORRENT=ON -DNUVIO_ENGINE_BUILD_TESTS=OFF \
  -DFETCHCONTENT_SOURCE_DIR_NUVIO_BOOST="$DEPS/nuvio_boost-src" -DFETCHCONTENT_SOURCE_DIR_NUVIO_LIBTORRENT="$DEPS/nuvio_libtorrent-src" \
  -DCMAKE_TOOLCHAIN_FILE="$TC/toolchain.cmake" "${TCX[@]}" -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  "${SSL[@]}" -DCMAKE_DISABLE_FIND_PACKAGE_GnuTLS=ON -DCMAKE_DISABLE_FIND_PACKAGE_LibGcrypt=ON \
  -DCMAKE_C_FLAGS="$FL" -DCMAKE_CXX_FLAGS="$FL" >/dev/null
if [ "$TARGET" = mac ] && [ -n "${ENGINE_MAC_PREBUILT:-}" ]; then B="$ENGINE_MAC_PREBUILT"   # the Mac preview only: archives built earlier (Homebrew's newer OpenSSL no longer compiles the pinned libtorrent)
else ninja -j "${ENGINE_JOBS:-4}" -C "$B" nuvio_engine; fi   # a few jobs: the Mac stays usable
mkdir -p "$(dirname "$OUT")"
LT="$B/_deps/nuvio_libtorrent-build/libtorrent-rasterbar.a"
if [ "$TARGET" = windows ]; then   # a DLL that exports sbe_get alone (the .def), the C++ runtime and winpthreads linked in
  printf 'LIBRARY libsbengine.dll\nEXPORTS\n  sbe_get\n' > "$TC/sbengine.def"
  "$TC/c++" -std=c++20 -Os $FL -shared -I"$SRC/include" -I"$ROOT/src" "$ROOT/tools/engine/sbengine.cpp" "$TC/sbengine.def" \
    "$B/nuvio_engine.a" "$LT" -static -static-libgcc -static-libstdc++ -Wl,--gc-sections -s \
    -lws2_32 -lmswsock -liphlpapi -lbcrypt -lcrypt32 -ladvapi32 -ldbghelp -o "$OUT"
elif [ "$TARGET" = webos ] || [[ "$TARGET" == android-* ]]; then
  echo '{ global: sbe_get; local: *; };' > "$TC/exports.map"
  "$TC/c++" -std=c++20 -Os -fPIC $FL -shared -I"$SRC/include" -I"$ROOT/src" "$ROOT/tools/engine/sbengine.cpp" \
    "$B/libnuvio_engine.a" "$LT" -Wl,--gc-sections -Wl,--version-script="$TC/exports.map" -Wl,--strip-all -Wl,-soname,libsbengine.so $([[ "$TARGET" == android-* ]] && echo "-static-libstdc++ -Wl,-z,max-page-size=16384" || echo -lpthread) -o "$OUT"
else
  "$TC/c++" -std=c++20 -Os $FL -shared -I"$SRC/include" -I"$ROOT/src" "$ROOT/tools/engine/sbengine.cpp" \
    "$B/libnuvio_engine.a" "$LT" "${SSLLIB[@]}" -framework Security -framework CoreFoundation -framework SystemConfiguration \
    -Wl,-dead_strip -o "$OUT"
fi
ls -l "$OUT"
