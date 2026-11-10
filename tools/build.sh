#!/bin/bash
# The one compile script, for every package:
#   tools/build.sh desktop [--run]    Linux, WSL or a Mac: for development and tests
#   tools/build.sh webos              LG webOS
#   tools/build.sh android [abi...]   Android, per CPU type (default: arm64-v8a armeabi-v7a x86)
#   tools/build.sh windows            Windows x64 (mingw-w64; tools/windows/build-deps.sh first)
# Options: --clean   compile everything again (always for a release: object files are kept unless their source is newer)
#          --clean-tp  compile only the third-party code again
# What differs per target (compiler, flags, libraries, the link) is in tools/targets/<target>.sh, and where things are in
# tools/paths.sh. Third-party code goes into <build>/tp/libtp.a once per target. SB_ASAN=1: a sanitizer build, in its own folder.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/paths.sh"

TARGET="${1:-desktop}"; shift || true
[ "$TARGET" = mac ] && TARGET=desktop
RUN=0; CLEAN=0; CLEAN_TP=0; ABIS=()
for a in "$@"; do case "$a" in --run) RUN=1;; --clean) CLEAN=1;; --clean-tp) CLEAN_TP=1;; -*) echo "unknown option $a"; exit 1;; *) ABIS+=("$a");; esac; done

run_cmds() {   # a file of shell commands, one per line, run in parallel
  [ -s "$1" ] || return 0
  tr '\n' '\0' < "$1" | xargs -0 -P "$JOBS" -I{} sh -c '{}' || { echo "COMPILE FAILED ($TARGET: $2)"; exit 1; }
}

third_party() {
  [ -f "$B/tp/libtp.a" ] && [ -f "$B/tp/libass.a" ] && return 0
  echo "== third-party ($TARGET ${ABI:-})"
  rm -f "$B"/tp/*.o "$B"/tp/ass/*.o
  local q="-w $OPT" ft="-DFT2_BUILD_LIBRARY '-DFT_CONFIG_MODULES_H=<ftmodule-sb.h>' -I$TP/config -I$TP/freetype/include" c="$B/tp/cmds" f n
  local fbi="-I$TP/config/fribidi -I$TP/fribidi/lib"
  : > "$c"
  for f in base/ftbase base/ftinit base/ftsystem base/ftdebug base/ftbitmap base/ftglyph base/ftmm base/ftbbox base/ftsynth base/ftstroke \
           gzip/ftgzip truetype/truetype sfnt/sfnt smooth/smooth autofit/autofit psnames/psnames pshinter/pshinter cff/cff psaux/psaux; do
    echo "$CC $q $ft -c $TP/freetype/src/$f.c -o $B/tp/ft_$(basename "$f").o" >> "$c"
  done
  echo "$CXX -w $OPT -std=c++11 -fno-exceptions -fno-rtti -DHAVE_FREETYPE=1 -DHAVE_PTHREAD=1 -I$TP/freetype/include -I$TP/harfbuzz/src -c $TP/harfbuzz/src/harfbuzz.cc -o $B/tp/harfbuzz.o" >> "$c"
  for f in "$TP"/fribidi/lib/fribidi*.c; do echo "$CC $q -DHAVE_CONFIG_H $fbi -c $f -o $B/tp/fb_$(basename "${f%.c}").o" >> "$c"; done
  echo "$CC $q -c $TP/cjson/cJSON.c -o $B/tp/cJSON.o" >> "$c"
  echo "$CC $q -c $TP/qrcodegen/qrcodegen.c -o $B/tp/qrcodegen.o" >> "$c"
  for f in "$TP"/webp/src/dec/*.c "$TP"/webp/src/dsp/*.c "$TP"/webp/src/utils/*.c; do   # libwebp, decoder only
    n="$(basename "$f")"
    case "$n" in *enc*|*writer*|*quant_levels_utils*|*_msa*|*_mips*) continue;; esac
    if webp_skip "$n"; then continue; fi
    echo "$WEBPCC $q -DWEBP_REDUCE_CSP -I$TP/webp -c $f -o $B/tp/webp_${n%.c}.o" >> "$c"
  done
  for f in "$TP"/libass/libass/*.c "$TP"/libass/libass/c/*.c; do   # styled ASS / SSA subtitles, no system font provider
    echo "$CC -w $OPT -std=gnu11 -DHAVE_CONFIG_H -I$TP/config/libass -I$TP/libass/libass -I$TP/freetype/include -I$TP/harfbuzz/src $fbi -c $f -o $B/tp/ass/$(basename "${f%.c}").o" >> "$c"
  done
  echo "$CC -w $OPT -DFT2_BUILD_LIBRARY '-DFT_CONFIG_MODULES_H=<ftmodule-sb.h>' -I$TP/config -I$TP/freetype/include -c $TP/freetype/src/base/fttype1.c -o $B/tp/ass/ft_fttype1.o" >> "$c"   # (the Type 1 info libass asks FreeType for)
  run_cmds "$c" third-party
  $AR rcs "$B/tp/libtp.a" "$B"/tp/*.o
  $AR rcs "$B/tp/libass.a" "$B"/tp/ass/*.o
}

app() {
  echo "== app ($TARGET ${ABI:-})"
  local c="$B/obj/cmds" newh o fl f
  OBJS=()
  if [ "$(cat "$B/obj/.appid" 2>/dev/null)" != "$APPID" ]; then rm -f "$B"/obj/*.o; echo "$APPID" > "$B/obj/.appid"; fi
  newh="$(find "$ROOT/src" -name '*.h' -newer "$B/obj/.stamp" 2>/dev/null | head -1 || true)"; [ -f "$B/obj/.stamp" ] || newh=1
  : > "$c"
  for f in "$TP"/crypto/*.c "$ROOT"/src/core/*.c "$ROOT"/src/ui/*.c "$ROOT"/src/screens/*.c "$ROOT"/src/platform/$PLAT/*.c "$ROOT"/src/main.c; do
    [ -f "$f" ] || continue
    case " $MODULES_OFF " in *" ${f#$ROOT/src/} "*) continue;; esac   # code this package leaves out (tools/targets/<target>.sh)
    case "$f" in
      "$TP"/*) o="$B/obj/tp_$(basename "${f%.c}").o"; fl="-w $OPT -I$TP/crypto";;
      *) o="$B/obj/$(echo "${f#$ROOT/src/}" | tr / _).o"; fl="$CFLAGS";;
    esac
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ -n "$newh" ] || [ "$B/sbkeys.h" -nt "$o" ]; then
      echo "$CC $fl -c '$f' -o '$o' 2>&1 | grep -v '^\$' || true; [ -f '$o' ]" >> "$c"; rm -f "$o"
    fi
    OBJS+=("$o")
  done
  run_cmds "$c" app
  touch "$B/obj/.stamp"
  link_app
}

build_one() {   # one target, or one CPU type of Android
  OPT="-O2 -fno-strict-aliasing -ffunction-sections -fdata-sections"
  [ -n "${SB_FP:-}" ] && OPT="$OPT -fno-omit-frame-pointer -marm"   # SB_FP=1: frame pointers for the profiler
  [ -n "${SB_ASAN:-}" ] && OPT="-O1 -g -fsanitize=address -fno-omit-frame-pointer"
  DEP_INC=""; BSUF=""; [ -n "${SB_ASAN:-}" ] && BSUF=-asan; MODULES_OFF=""
  unset -f prepare_target 2>/dev/null || true
  source "$ROOT/tools/targets/$1.sh"
  [ $CLEAN = 1 ] && rm -rf "$B"
  [ $CLEAN_TP = 1 ] && rm -rf "$B/tp"
  mkdir -p "$B/tp/ass" "$B/obj"
  if declare -F prepare_target >/dev/null; then prepare_target; fi
  python3 "$ROOT/tools/sbkeys.py" "$KEYS" "$B/sbkeys.h"
  CFLAGS="$OPT -std=c11 -D_GNU_SOURCE '-DSB_APP_ID=\"$APPID\"' -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers $TFLAGS -I$ROOT/src/platform/$PLAT -include target.h -I$ROOT/src $DEP_INC -I$TP/freetype/include -I$TP/harfbuzz/src -I$TP/fribidi/lib -I$TP/config/fribidi -I$TP/cjson -I$TP/stb -I$TP/qrcodegen -I$TP/webp -I$TP/crypto -I$TP/libass -I$B"
  third_party
  app
}

case "$TARGET" in
  desktop|webos|windows) ABI=""; build_one "$TARGET";;
  android) [ ${#ABIS[@]} -gt 0 ] || ABIS=(arm64-v8a armeabi-v7a x86); for ABI in "${ABIS[@]}"; do build_one android; done;;
  *) echo "unknown target $TARGET (desktop, webos, android or windows)"; exit 1;;
esac
echo BUILD_OK
[ $RUN = 1 ] && [ "$TARGET" = desktop ] && exec "$B/stremioorigin" "$ROOT/assets"
exit 0
