# macOS on Apple Silicon, 11 or later: Origin, the binary in Origin.app (packaging/macos/pack.sh). Sourced by
# tools/build.sh. Needs tools/macos/build-deps.sh first. SDL2.framework is linked; libmpv is loaded at run time from the
# app's Frameworks folder (video_macos.c), so only its headers are used here. curl, zlib and iconv are the system's.
CC="${HOSTCC:-clang}"; WEBPCC="$CC"; CXX="${HOSTCXX:-clang++}"; AR=ar
PLAT=macos
B="$BUILD_ROOT/macos$BSUF"
OPT="$OPT -arch arm64 -mmacosx-version-min=11.0"
MODULES_OFF="core/usb.c core/cast.c core/castlink.c"
webp_skip() { case "$1" in *_sse*) return 0;; esac; return 1; }
[ -d "$MB/Frameworks/SDL2.framework" ] || { echo "no $MB/Frameworks/SDL2.framework: run tools/macos/build-deps.sh"; exit 1; }
TFLAGS="-DGL_SILENCE_DEPRECATION -F$MB/Frameworks -I$MB/include -Wno-sign-compare -Wno-unused-function"
LIBS="-F$MB/Frameworks -framework SDL2 -lcurl -liconv -lz -framework OpenGL -framework CoreFoundation \
  -framework CoreGraphics -framework CoreAudio -framework IOKit -Wl,-rpath,@executable_path/../Frameworks"

link_app() {
  $CXX $OPT -o "$B/Origin" "${OBJS[@]}" "$B/tp/libass.a" "$B/tp/libtp.a" $LIBS -Wl,-dead_strip \
    -Wl,-headerpad_max_install_names
  ls -l "$B/Origin" | awk '{printf "%s  %.1f MB\n", $NF, $5/1048576}'
}
