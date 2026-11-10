# Android, one CPU type at a time ($ABI): libmain.so, which SDL's activity loads, with the NDK. Sourced by tools/build.sh.
# Needs tools/android/build-deps.sh first. Output: $O/libmain.so (stripped) and libmain.dbg.so (symbols, for crash addresses).
SDK_ROOT="${ANDROID_HOME:-$HOME/android-sdk}"
NDK="${NDK:-$SDK_ROOT/ndk/27.2.12479018}"; API="${API:-24}"
NDKBIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
case "$ABI" in
  arm64-v8a) TRIPLE=aarch64-linux-android;;
  armeabi-v7a) TRIPLE=armv7a-linux-androideabi;;
  x86) TRIPLE=i686-linux-android;;
  x86_64) TRIPLE=x86_64-linux-android;;
  *) echo "unknown ABI $ABI"; exit 1;;
esac
CC="$NDKBIN/clang --target=$TRIPLE$API"; WEBPCC="$CC"; CXX="$NDKBIN/clang++ --target=$TRIPLE$API"; AR="$NDKBIN/llvm-ar"
PLAT=android
D="$AB/deps/$ABI"
B="$BUILD_ROOT/android/$ABI$BSUF"; O="$BUILD_ROOT/android/out/$ABI"; mkdir -p "$O"
OPT="$OPT -fPIC"
case "$ABI" in armeabi-v7a|x86) OPT="$OPT -D_FILE_OFFSET_BITS=64";; esac
TFLAGS="-DSB_ANDROID=1 -Wno-sign-compare -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable"
DEP_INC="-I$D/include"
webp_skip() { case "$ABI:$1" in x86*:*_neon*|arm*:*_sse*) return 0;; esac; return 1; }

link_app() {
  $CXX -shared -o "$O/libmain.dbg.so" "${OBJS[@]}" "$B/tp/libass.a" "$B/tp/libtp.a" -L"$D/lib" -lSDL2 -lcurl -lGLESv2 -lEGL -llog -landroid -lz -lm -ldl \
    -static-libstdc++ -Wl,--gc-sections -Wl,-z,max-page-size=16384 -Wl,--build-id=sha1 -Wl,--no-undefined
  "$NDKBIN/llvm-strip" --strip-unneeded -o "$O/libmain.so" "$O/libmain.dbg.so"
  cp "$D/lib/libSDL2.so" "$D/lib/libcurl.so" "$O/"; "$NDKBIN/llvm-strip" --strip-unneeded "$O/libcurl.so" "$O/libSDL2.so"
  if [ -f "$AB/engine/$ABI/libsbengine.so" ]; then cp "$AB/engine/$ABI/libsbengine.so" "$O/"; else echo "warning: no P2P engine for $ABI (tools/build-engine.sh android-$ABI)"; fi
  ls -l "$O" | awk 'NR>1 {printf "  %-22s %6.1f MB\n", $NF, $5/1048576}'
}
