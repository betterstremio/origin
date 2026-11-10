# LG webOS: ARMv7 through zig, SDL2 and GLES2 from the webOS SDK. Sourced by tools/build.sh.
case "$(uname -s)-$(uname -m)" in Darwin-arm64) ZIGHOST=aarch64-macos;; Linux-aarch64) ZIGHOST=aarch64-linux;; *) ZIGHOST=x86_64-linux;; esac
ZIG="${ZIG:-$ORIGIN_BUILD/zig-$ZIGHOST-0.17.0/zig}"
SDK="${SDK:-$ORIGIN_BUILD/sdk/arm-webos-linux-gnueabi_sdk-buildroot/arm-webos-linux-gnueabi/sysroot}"
export ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-$HOME/.zigcache}"
ZT="-target arm-linux-gnueabi.2.12"
CC="$ZIG cc $ZT -mcpu=cortex_a9 -mfloat-abi=softfp"
WEBPCC="$ZIG cc $ZT -mcpu=cortex_a9-neon -mfloat-abi=softfp"   # libwebp's NEON paths break this compiler's backend: plain C there
CXX="$ZIG c++ $ZT -mcpu=cortex_a9 -mfloat-abi=softfp"
AR="$ZIG ar"
PLAT=webos
TFLAGS="-DSB_WEBOS=1 -D_FILE_OFFSET_BITS=64 -idirafter $SDK/usr/include"   # after the compiler's own libc headers: the SDK's old glibc headers must not mix with them
LIBS="$SDK/usr/lib/libSDL2.so $SDK/usr/lib/libGLESv2.so $SDK/usr/lib/libz.so -ldl -lpthread -lm"
B="$BUILD_ROOT/webos$BSUF"
webp_skip() { case "$1" in *_sse*) return 0;; esac; return 1; }

link_app() {
  $CXX $OPT -Wl,--gc-sections -o "$B/stremioorigin.dbg" "${OBJS[@]}" "$B/tp/libass.a" "$B/tp/libtp.a" $LIBS   # symbols kept: crash addresses resolve against it
  python3 "$ROOT/tools/elfstrip.py" "$B/stremioorigin.dbg" "$B/stremioorigin" && chmod +x "$B/stremioorigin"   # the same link, stripped: crash addresses match the symbols byte for byte (two links came out apart)
  local bid; bid=$(LC_ALL=C grep -ao 'SBBUILDID:[0-9a-f]\{32\}' "$B/stremioorigin.dbg" | head -1 | cut -c11-22)   # every build's debug copy kept by its id (the last 40): a crash from any build installed can be named
  if [ -n "$bid" ]; then mkdir -p "$ROOT/dist/dbg/builds"; cp "$B/stremioorigin.dbg" "$ROOT/dist/dbg/builds/$bid.dbg"; ls -t "$ROOT"/dist/dbg/builds/*.dbg | tail -n +41 | xargs rm -f; fi
  ls -l "$B/stremioorigin" | awk '{printf "%s  %.1f MB\n", $NF, $5/1048576}'
}
