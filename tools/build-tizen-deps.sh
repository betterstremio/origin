#!/bin/bash
# The libraries linked into libstremioorigin.so for Samsung (ARMv7 softfp, glibc 2.24 and older), so the app needs
# nothing of the TV's own beyond the C library, EGL / GLES and what it opens by name at run time:
#   SDL 2.30.12: timers, threads, events, the dummy video driver (the host gives the GL surface) and sound through the
#                TV's mixer (pulseaudio, its library opened at run time, as the TV's own SDL did)
#   zlib 1.3.1
# Sources from $ORIGIN_BUILD/src (SDL2-2.30.12.tar.gz, zlib-1.3.1.tar.gz). Output: $BUILD_ROOT/tizen/deps/{lib,include}.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/paths.sh"
SDK="$1"
D="$2"; W="$D/work"; mkdir -p "$W" "$D/lib" "$D/include"
STAMP="$D/.stamp"
flags="$ZIG $SDK SDL2-2.30.12 zlib-1.3.1 v1"
[ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$flags" ] && [ -f "$D/lib/libSDL2.a" ] && [ -f "$D/lib/libz.a" ] && exit 0
rm -f "$STAMP"
# No path of the build machine in what is built: file names in asserts, logs and debug data are written as /deps/...,
# /src/..., /toolchain/... (SDL carried its source's full path, the builder's home folder with it, into the library)
PMAP="-ffile-prefix-map=$HOME=/home -ffile-prefix-map=$ORIGIN_BUILD=/toolchain -ffile-prefix-map=$ROOT=/src -ffile-prefix-map=$W=/deps"
FL="-target arm-linux-gnueabi.2.24 -mcpu=cortex_a9 -mfloat-abi=softfp -O2 -fPIC -ffunction-sections -fdata-sections -fvisibility=hidden -U__ARM_NEON -U__ARM_NEON__ $PMAP"  # (no NEON: this zig hits a backend bug on NEON code with the TV's float ABI; scalar code as the TV's own SDL)
printf '#!/bin/sh\nexec "%s" cc %s "$@"\n' "$ZIG" "$FL" > "$W/cc"; chmod +x "$W/cc"
printf '#!/bin/sh\nexec "%s" ar "$@"\n' "$ZIG" > "$W/ar"; chmod +x "$W/ar"
printf '#!/bin/sh\nexec "%s" ranlib "$@"\n' "$ZIG" > "$W/ranlib"; chmod +x "$W/ranlib"

verify_source() {
  local source="$ORIGIN_BUILD/src/$1"
  [ -f "$source" ] || { echo "tizen: missing source archive $source" >&2; exit 1; }
  printf '%s  %s\n' "$2" "$source" | shasum -a 256 -c -
}

echo "== zlib 1.3.1"
rm -rf "$W/zlib-1.3.1"
verify_source zlib-1.3.1.tar.gz 9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
tar -xzf "$ORIGIN_BUILD/src/zlib-1.3.1.tar.gz" -C "$W"
( cd "$W/zlib-1.3.1" && for f in adler32 crc32 deflate infback inffast inflate inftrees trees zutil compress uncompr gzclose gzlib gzread gzwrite; do
    "$W/cc" -w -DHAVE_UNISTD_H -c $f.c -o $f.o; done && rm -f "$D/lib/libz.a" && "$W/ar" rcs "$D/lib/libz.a" *.o && cp zlib.h zconf.h "$D/include/" )

echo "== SDL 2.30.12"
rm -rf "$W/SDL2-2.30.12" "$W/sdl-build"
verify_source SDL2-2.30.12.tar.gz ac356ea55e8b9dd0b2d1fa27da40ef7e238267ccf9324704850d5d47375b48ea
tar -xzf "$ORIGIN_BUILD/src/SDL2-2.30.12.tar.gz" -C "$W"
cat > "$W/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER "$W/cc")
set(CMAKE_AR "$W/ar")
set(CMAKE_RANLIB "$W/ranlib")
set(CMAKE_FIND_ROOT_PATH "$SDK")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
OFF="X11 WAYLAND KMSDRM OPENGL OPENGLES VULKAN RPI DIRECTFB OFFSCREEN VIVANTE METAL RENDER_D3D \
     ALSA PIPEWIRE JACK SNDIO ESD ARTS NAS OSS FUSIONSOUND LIBSAMPLERATE DISKAUDIO \
     JOYSTICK HAPTIC SENSOR HIDAPI POWER LIBUDEV DBUS IBUS FCITX TEST TESTS SHARED ARMNEON ARMSIMD ASSEMBLY"
OPTS=(); for o in $OFF; do OPTS+=("-DSDL_$o=OFF"); done
PKG_CONFIG_SYSROOT_DIR="$SDK" PKG_CONFIG_LIBDIR="$SDK/usr/lib/pkgconfig:$SDK/usr/share/pkgconfig" \
cmake -S "$W/SDL2-2.30.12" -B "$W/sdl-build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$W/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" \
  -DSDL_STATIC=ON -DSDL_STATIC_PIC=ON -DSDL_VIDEO=ON -DSDL_DUMMYVIDEO=ON -DSDL_AUDIO=ON -DSDL_DUMMYAUDIO=ON \
  -DSDL_PULSEAUDIO=ON -DSDL_PULSEAUDIO_SHARED=ON -DSDL_LOADSO=ON -DSDL_THREADS=ON -DSDL_TIMERS=ON -DSDL_PTHREADS=ON \
  "${OPTS[@]}" > "$W/sdl-cmake.log" 2>&1 || { tail -30 "$W/sdl-cmake.log"; exit 1; }
grep -Eq '^#define SDL_AUDIO_DRIVER_PULSEAUDIO_DYNAMIC ' "$W/sdl-build/include/SDL_config.h" 2>/dev/null || grep -rEq '^#define SDL_AUDIO_DRIVER_PULSEAUDIO_DYNAMIC ' "$W/sdl-build/include-config-release" 2>/dev/null \
  || { echo "SDL: no pulseaudio (the TV's mixer) in this build: the interface would be silent" >&2; grep -i pulse "$W/sdl-cmake.log" | tail -5; exit 1; }
ninja -C "$W/sdl-build" > "$W/sdl-ninja.log" 2>&1 || { tail -30 "$W/sdl-ninja.log"; exit 1; }
cp "$W/sdl-build/libSDL2.a" "$D/lib/libSDL2.a"
rm -rf "$D/include/SDL2"; mkdir -p "$D/include/SDL2"; cp "$W/SDL2-2.30.12/include/"*.h "$D/include/SDL2/"
cp $(find "$W/sdl-build" -name SDL_config.h -path "*include*" | head -1) "$D/include/SDL2/SDL_config.h" 2>/dev/null || true
cp $(find "$W/sdl-build" -name SDL_revision.h | head -1) "$D/include/SDL2/" 2>/dev/null || true
printf '%s\n' "$flags" > "$STAMP"
echo "== deps in $D"
