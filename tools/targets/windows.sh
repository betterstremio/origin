# Windows x64: Origin.exe, cross-compiled with mingw-w64 (posix threads) in Linux or WSL. Sourced by tools/build.sh.
# Needs tools/windows/build-deps.sh first. Output: $O/Origin.exe (stripped), Origin.dbg.exe (symbols, for crash
# addresses), and the ANGLE DLLs beside it. Everything else is linked in: no C runtime or SDL DLL to ship.
X=x86_64-w64-mingw32
CC="$X-gcc-posix"; WEBPCC="$CC"; CXX="$X-g++-posix"; AR="$X-ar"
PLAT=windows
D="$WB/deps"
B="$BUILD_ROOT/windows/build$BSUF"; O="$BUILD_ROOT/windows/out"; mkdir -p "$O"   # (apart: --clean empties $B)
OPT="$OPT -D_FILE_OFFSET_BITS=64 -D__USE_MINGW_ANSI_STDIO=1"   # (C99 printf: the old C library knows no %zu)
MODULES_OFF="core/usb.c core/cast.c core/castlink.c"
# gnu11: mingw hides strdup, fileno and the like under strict C11. compat.h comes before target.h.
TFLAGS="-std=gnu11 -I$ROOT/src/platform/windows/compat -include compat.h -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DSDL_MAIN_HANDLED -DCURL_STATICLIB \
  -Wno-sign-compare -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-format-truncation -Wno-misleading-indentation"
DEP_INC="-I$D/include"
webp_skip() { case "$1" in *_neon*) return 0;; esac; return 1; }

link_app() {
  [ "$B/origin.ico" -nt "$ROOT/assets/icon.png" ] || python3 -c "import sys; from PIL import Image; Image.open(sys.argv[1]).convert('RGBA').save(sys.argv[2], sizes=[(16,16),(24,24),(32,32),(48,48),(64,64),(128,128),(256,256)])" "$ROOT/assets/icon.png" "$B/origin.ico"
  local v; v="$(tr -d '[:space:]' < "$ROOT/VERSION")"; IFS=. read -r va vb vc <<<"$v"
  $X-windres -I"$ROOT/packaging/windows" -I"$B" -DVER_A="$va" -DVER_B="$vb" -DVER_C="$vc" -DVER_STR="\\\"$v\\\"" "$ROOT/packaging/windows/origin.rc" -O coff -o "$B/origin_res.o"
  $CXX $OPT -o "$O/Origin.dbg.exe" "${OBJS[@]}" "$B/origin_res.o" "$B/tp/libass.a" "$B/tp/libtp.a" -L"$D/lib" \
    -lSDL2 -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -leverest -lp256m -lz "$D/lib/libGLESv2.dll.a" \
    -lws2_32 -liphlpapi -lcrypt32 -lbcrypt -lsetupapi -lwinmm -limm32 -lversion -loleaut32 -lole32 -lgdi32 -luser32 -lshell32 -ladvapi32 -lpsapi -luuid -ldwmapi \
    -static -static-libgcc -static-libstdc++ -mwindows -Wl,--gc-sections
  $X-strip --strip-unneeded -o "$O/Origin.exe" "$O/Origin.dbg.exe"
  cp -u "$D/bin/libEGL.dll" "$D/bin/libGLESv2.dll" "$D/bin/libmpv-2.dll" "$O/"
  if [ -f "$WB/engine/libsbengine.dll" ]; then cp -u "$WB/engine/libsbengine.dll" "$O/"; else echo "warning: no P2P engine (tools/build-engine.sh windows)"; fi
  ls -l "$O" | awk 'NR>1 {printf "  %-22s %6.1f MB\n", $NF, $5/1048576}'
}
