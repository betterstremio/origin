/* Android (TV boxes, Google TV, Fire TV, phones): SDL2's activity runs the app. The Java side
   (packaging/android/app/src/main/java/tv/stremioorigin/app/MainActivity.java) copies the assets out of the APK and
   hands their folder, the data folder, the native library folder and the device's facts over as
   environment variables before SDL starts (SB_ASSETS, SB_DATA, SB_LIBDIR, SB_DEVICE, SB_LOCALE). Remote
   keys SDL has no name for (the colour keys, channel up/down) arrive as F1-F4 and Page Up/Down, swapped
   on the Java side. stderr goes to logcat, tag "StremioOrigin". */
#include "platform/plat.h"
#include "core/keys.h"
#include "core/applog.h"
#include <android/log.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/syscall.h>

static char assetDir[1024], dataDir[1024], devName[160], locale[24];

/* every line written to stderr (and stdout) reaches logcat */
static int logPipe[2] = { -1, -1 };
static void *log_pump(void *a) { (void)a;
  char buf[2048]; size_t n = 0; ssize_t r;
  while ((r = read(logPipe[0], buf + n, sizeof buf - 1 - n)) > 0) {
    n += (size_t)r; buf[n] = 0; char *s = buf, *nl;
    while ((nl = strchr(s, '\n'))) { *nl = 0; __android_log_write(ANDROID_LOG_INFO, "StremioOrigin", s); s = nl + 1; }
    n = strlen(s); memmove(buf, s, n + 1);
    if (n == sizeof buf - 1) { __android_log_write(ANDROID_LOG_INFO, "StremioOrigin", buf); n = 0; } }
  return NULL; }
static void log_start(void) {
  if (logPipe[0] >= 0 || pipe(logPipe)) return;
  setvbuf(stderr, NULL, _IOLBF, 0); setvbuf(stdout, NULL, _IOLBF, 0);
  dup2(logPipe[1], 2); dup2(logPipe[1], 1);
  pthread_t t; if (!pthread_create(&t, NULL, log_pump, NULL)) pthread_detach(t);
}
static void mkdirs(const char *d) { char p[1100]; snprintf(p, sizeof p, "%s", d);
  for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0700); *s = '/'; } mkdir(p, 0700); }

/* the variables MainActivity set, asked for again over JNI and set here: native code under a translation layer (an
   ARM build on an x86 emulator) has its own environment, which Java's Os.setenv does not reach */
static void env_from_java(void) {
  JNIEnv *e = (JNIEnv *)SDL_AndroidGetJNIEnv(); if (!e) return;
  jclass c = (*e)->FindClass(e, "tv/stremioorigin/app/MainActivity"); if (!c) { (*e)->ExceptionClear(e); return; }
  jmethodID m = (*e)->GetStaticMethodID(e, c, "envList", "()Ljava/lang/String;"); if (!m) { (*e)->ExceptionClear(e); return; }
  jstring js = (jstring)(*e)->CallStaticObjectMethod(e, c, m); if ((*e)->ExceptionCheck(e)) { (*e)->ExceptionClear(e); return; }
  const char *s = js ? (*e)->GetStringUTFChars(e, js, NULL) : NULL; int n = 0;
  for (const char *p = s; p && *p; ) { const char *nl = strchr(p, '\n'), *eq = strchr(p, '='); size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (eq && eq < p + len) { char k[128], v[4096]; snprintf(k, sizeof k, "%.*s", (int)(eq - p), p); snprintf(v, sizeof v, "%.*s", (int)(p + len - eq - 1), eq + 1); setenv(k, v, 1); n++; }
    p = nl ? nl + 1 : p + len; }
  if (s) (*e)->ReleaseStringUTFChars(e, js, s); if (js) (*e)->DeleteLocalRef(e, js); (*e)->DeleteLocalRef(e, c);
  (void)n;
}
void plat_pre_init(const char *appId) {
  log_start();
  env_from_java();
  const char *a = getenv("SB_ASSETS"), *d = getenv("SB_DATA"), *dev = getenv("SB_DEVICE"), *loc = getenv("SB_LOCALE");
  snprintf(assetDir, sizeof assetDir, "%s", a ? a : "/data/local/tmp/sb/assets");
  if (d) snprintf(dataDir, sizeof dataDir, "%s", d); else snprintf(dataDir, sizeof dataDir, "/data/data/%s/files/data", appId);
  mkdirs(dataDir);
  snprintf(devName, sizeof devName, "%s", dev && *dev ? dev : "Android");
  if (loc && *loc) snprintf(locale, sizeof locale, "%s", loc);
  SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");           /* Back is the app's key, not the activity's end */
  int touch = getenv("SB_FORM") && strcmp(getenv("SB_FORM"), "tv"); /* a phone or a tablet: MainActivity turns the screen, fingers are read as fingers */
  SDL_SetHint(SDL_HINT_ORIENTATIONS, touch ? "Portrait LandscapeLeft LandscapeRight" : "LandscapeLeft LandscapeRight");
  SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "1");
  SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, touch ? "0" : "1");    /* a TV's touch screen (rare): taps act as the pointer */
  SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");
  applog_start(dataDir);
  fprintf(stderr, "[store] data in %s, assets in %s\n", dataDir, assetDir);
}
void plat_post_window(SDL_Window *w) { (void)w; }
void plat_cursor_visible(int on) { (void)on; }
const char *plat_asset_dir(void) { return assetDir; }
const char *plat_data_dir(void) { return dataDir; }
/* fullscreen at the surface Java gives SDL: a 16:9 box in the middle of the screen (a wider screen gets black bars),
   1920x1080 or less; a phone or a tablet: a window between the system bars (MainActivity hides them for a film) */
void plat_window_size(int *w, int *h, Uint32 *flags) {
  SDL_DisplayMode m; *w = 1920; *h = 1080;
  if (!SDL_GetDesktopDisplayMode(0, &m) && m.w > 0 && m.h > 0) { *w = m.w; *h = m.h; }
  *flags = SDL_WINDOW_OPENGL | (getenv("SB_FORM") && strcmp(getenv("SB_FORM"), "tv") ? SDL_WINDOW_RESIZABLE : SDL_WINDOW_FULLSCREEN);
}
int plat_map_key(const SDL_KeyboardEvent *e) {
  switch (e->keysym.scancode) {
    case SDL_SCANCODE_AC_BACK: return K_BACK;
    case SDL_SCANCODE_AUDIOPLAY: return K_PLAYPAUSE;
    case SDL_SCANCODE_AUDIOSTOP: case SDL_SCANCODE_STOP: return K_STOP;
    case SDL_SCANCODE_AUDIOREWIND: return K_REW;
    case SDL_SCANCODE_AUDIOFASTFORWARD: return K_FF;
    default: break;
  }
  switch (e->keysym.sym) {
    case SDLK_LEFT: return K_LEFT; case SDLK_RIGHT: return K_RIGHT; case SDLK_UP: return K_UP; case SDLK_DOWN: return K_DOWN;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SELECT: return K_OK;
    case SDLK_AC_BACK: case SDLK_ESCAPE: return K_BACK;
    case SDLK_BACKSPACE: { static int ph = -1; if (ph < 0) ph = getenv("SB_FORM") && strcmp(getenv("SB_FORM"), "tv"); return ph ? K_ERASE : K_BACK; } /* a phone's keyboard deletes */
    case SDLK_AUDIOPLAY: case SDLK_SPACE: return K_PLAYPAUSE;
    case SDLK_PAUSE: return K_PAUSE; case SDLK_AUDIOSTOP: return K_STOP;
    case SDLK_AUDIOREWIND: case SDLK_COMMA: return K_REW; case SDLK_AUDIOFASTFORWARD: case SDLK_PERIOD: return K_FF;
    case SDLK_F1: return K_RED; case SDLK_F2: return K_GREEN; case SDLK_F3: return K_YELLOW; case SDLK_F4: return K_BLUE;
    case SDLK_PAGEUP: return K_CH_UP; case SDLK_PAGEDOWN: return K_CH_DOWN;
    default: break;
  }
  return K_NONE;
}
const char *plat_device_name(void) { return devName; }
int plat_text_input(void) { return getenv("SB_TEXTINPUT") ? 1 : 0; } /* a remote has no keyboard: the app draws its own (a phone opens the system's per field: core/form.h) */
void plat_thread_background(int nice) { setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), nice); }
int plat_mem_total_mb(void) {
  if (getenv("SB_MEMMB")) return atoi(getenv("SB_MEMMB"));
  FILE *f = fopen("/proc/meminfo", "r"); if (!f) return 0;
  char line[128]; long kb = 0; while (fgets(line, sizeof line, f)) if (sscanf(line, "MemTotal: %ld kB", &kb) == 1) break;
  fclose(f); return (int)(kb / 1024);
}
/* Home, another app or the screen going off: SDL's app events; only a return after a leave counts */
int plat_lifecycle(const SDL_Event *e) {
  static int away;
  if (e->type == SDL_APP_WILLENTERBACKGROUND || e->type == SDL_APP_DIDENTERBACKGROUND) { if (!away) fprintf(stderr, "[life] to the back\n"); away = 1; return -1; }
  if ((e->type == SDL_APP_DIDENTERFOREGROUND || e->type == SDL_APP_WILLENTERFOREGROUND) && away) { away = 0; fprintf(stderr, "[life] back in front\n"); return 1; }
  return 0;
}
const char *plat_ui_locale(void) { return locale[0] ? locale : NULL; }
char *plat_luna_call(const char *uri, const char *payload, int timeoutMs) { (void)uri; (void)payload; (void)timeoutMs; return NULL; }
unsigned long plat_luna_subscribe(const char *uri, const char *payload, PlatLunaCb cb, void *ud) { (void)uri; (void)payload; (void)cb; (void)ud; return 0; }
void plat_luna_cancel(unsigned long handle) { (void)handle; }
void plat_luna_pump(void) {}
int plat_luna_send(const char *uri, const char *payload) { (void)uri; (void)payload; return 0; }
int plat_panel_lines(void) { SDL_DisplayMode m; if (!SDL_GetDesktopDisplayMode(0, &m)) return m.h; return 0; }
/* MainActivity asks Android whether any remote or keyboard has the red and green keys (KeyCharacterMap.deviceHasKeys) */
int plat_color_keys(void) { const char *v = getenv("SB_COLORKEYS"); return v && !strcmp(v, "1"); }
int plat_random(void *buf, int n) { FILE *f = fopen("/dev/urandom", "rb"); if (!f) return 0; size_t g = fread(buf, 1, (size_t)n, f); fclose(f); return g == (size_t)n; }
