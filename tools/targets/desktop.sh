# The development build, for screenshots and tests: Linux (or WSL) or a Mac, SDL2 + OpenGL 2.1. Sourced by tools/build.sh.
# It uses the Mac platform layer on both. On Linux a few headers stand in for the Mac's.
CC="${HOSTCC:-cc}"; WEBPCC="$CC"; CXX="${HOSTCXX:-c++}"; AR=ar
PLAT=mac
B="$BUILD_ROOT/desktop$BSUF"
case "$(uname -m)" in
  x86_64|i?86) webp_skip() { case "$1" in *_neon*) return 0;; esac; return 1; };;
  *)           webp_skip() { case "$1" in *_sse*) return 0;; esac; return 1; };;
esac
if [ "$(uname -s)" = Darwin ]; then
  SDLP="${MACSDL:-$ORIGIN_BUILD/mac/sdl2}"
  TFLAGS="-DSB_MAC=1 -I$SDLP/include -I$SDLP/include/SDL2"
  LIBS="$("$SDLP/bin/sdl2-config" --static-libs) -Wl,-framework,OpenGL -lcurl -liconv -lz"
else
  prepare_target() {
    local s="$B/shim"; mkdir -p "$s/OpenGL" "$s/mach"
    for h in gl gl3; do printf '#define GL_GLEXT_PROTOTYPES 1\n#include <GL/gl.h>\n#include <GL/glext.h>\n' > "$s/OpenGL/$h.h"; done
    cat > "$s/mach/mach.h" <<'H'
typedef struct { unsigned long resident_size; } mach_task_basic_info_data_t; typedef unsigned mach_msg_type_number_t; typedef void *task_info_t;
#define MACH_TASK_BASIC_INFO_COUNT 0
#define MACH_TASK_BASIC_INFO 0
#define KERN_SUCCESS 0
static inline int mach_task_self(void) { return 0; }
static inline int task_info(int a, int b, task_info_t c, mach_msg_type_number_t *d) { return 1; }
H
  }
  TFLAGS="-DSB_MAC=1 -w -I$B/shim -I/usr/include/SDL2"
  LIBS="$(sdl2-config --libs) -lGL -lcurl -lz -ldl -lpthread -lm"
fi

link_app() {
  $CXX $OPT -o "$B/stremioorigin" "${OBJS[@]}" "$B/tp/libass.a" "$B/tp/libtp.a" $LIBS
  ls -l "$B/stremioorigin" | awk '{printf "%s  %.1f MB\n", $NF, $5/1048576}'
}
