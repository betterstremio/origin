# Samsung TV: 32-bit ARM softfp, GLES2 through the .NET host.
case "$(uname -s)-$(uname -m)" in
  Darwin-arm64) ZIGHOST=aarch64-macos;;
  Linux-aarch64) ZIGHOST=aarch64-linux;;
  *) ZIGHOST=x86_64-linux;;
esac
ZIG="${ZIG:-$ORIGIN_BUILD/zig-$ZIGHOST-0.17.0/zig}"
export ZIG
SDK="${TIZEN_SYSROOT:-$ORIGIN_BUILD/tizen/sysroot}"   # (EGL, GLES2, curl and PulseAudio for ARM: docs/tizen.md)
ZT="-target arm-linux-gnueabi.2.24 -mcpu=cortex_a9 -mfloat-abi=softfp"
CC="$ZIG cc $ZT"
CXX="$ZIG c++ $ZT"
AR="$ZIG ar"
WEBPCC="$ZIG cc -target arm-linux-gnueabi.2.24 -mcpu=cortex_a9-neon -mfloat-abi=softfp"
PLAT=tizen
MODULES_OFF="core/cast.c core/castlink.c core/usb.c"
B="$BUILD_ROOT/tizen$BSUF"
OPT="$OPT -fPIC -ffile-prefix-map=$ROOT=/src -ffile-prefix-map=$ORIGIN_BUILD=/toolchain"
TFLAGS="-DSTBI_NO_THREAD_LOCALS -DSTBI_NO_THREAD_LOCAL -D_FILE_OFFSET_BITS=64 -idirafter $SDK/usr/include"
DEP_INC="-I$B/deps/include"
webp_skip() { case "$1" in *_sse*) return 0;; esac; return 1; }

prepare_target() {
  command -v "$ZIG" >/dev/null || { echo "tizen: Zig not found at $ZIG (set ZIG)" >&2; return 1; }
  [ -f "$SDK/usr/lib/libGLESv2.so" ] || {
    echo "tizen: missing ARM libGLESv2.so at $SDK/usr/lib (set TIZEN_SYSROOT)" >&2
    return 1
  }
  bash "$ROOT/tools/build-tizen-deps.sh" "$SDK" "$B/deps"
}

link_app() {
  $CXX $OPT -shared -Wl,--hash-style=both -Wl,--gc-sections -Wl,--no-undefined \
    -Wl,--version-script="$ROOT/src/platform/tizen/exports.map" -o "$B/libstremioorigin.so.dbg" \
    "${OBJS[@]}" "$B/tp/libass.a" "$B/tp/libtp.a" "$B/deps/lib/libSDL2.a" "$B/deps/lib/libz.a" \
    "$SDK/usr/lib/libGLESv2.so" -ldl -lpthread -lm
  python3 "$ROOT/tools/elfstrip.py" "$B/libstremioorigin.so.dbg" "$B/libstremioorigin.so"
  python3 "$ROOT/packaging/tizen/check-elf.py" "$B/libstremioorigin.so"
}
