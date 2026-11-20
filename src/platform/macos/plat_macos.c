/* macOS: a desktop window as on Windows (resized, fullscreen, where it was left), the keyboard standing in for the
   remote with the Mac's Command key where Windows has Ctrl. The app runs from Origin.app: assets in
   Contents/Resources, data in ~/Library/Application Support/<app id>. */
#include "platform/plat.h"
#include "platform/mem.h"
#include "core/keys.h"
#include "core/applog.h"
#include "core/store.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static char assetDir[1024], dataDir[1024], devName[128], locale[32];
static SDL_Window *sw;
static int testSize;
static struct {
  int have, x, y, w, h, max, full;
} G;

static void mkdirs(const char *p) {
  char b[1024];
  snprintf(b, sizeof b, "%s", p);
  for (char *s = b + 1; *s; s++)
    if (*s == '/') {
      *s = 0;
      mkdir(b, 0755);
      *s = '/';
    }
  mkdir(b, 0755);
}
static int sysctl_str(const char *name, char *out, size_t cap) {
  size_t n = cap;
  return sysctlbyname(name, out, &n, NULL, 0) == 0 && n > 0;
}
static int main_display_lines(void) {
  CGDisplayModeRef m = CGDisplayCopyDisplayMode(CGMainDisplayID());
  int h = m ? (int)CGDisplayModeGetPixelHeight(m) : 0;
  if (m)
    CGDisplayModeRelease(m);
  return h;
}

void plat_pre_init(const char *appId) {
  SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1"); /* a desktop's display may sleep while nothing plays */
  SDL_SetHint(SDL_HINT_MAC_CTRL_CLICK_EMULATE_RIGHT_CLICK, "1");
  const char *a = getenv("SB_ASSETS");
  if (a)
    snprintf(assetDir, sizeof assetDir, "%s", a);
  else {
    char *base = SDL_GetBasePath(); /* inside the app: Contents/Resources/ */
    snprintf(assetDir, sizeof assetDir, "%sassets", base ? base : "./");
    SDL_free(base);
  }
  const char *d = getenv("SB_DATA");
  if (d)
    snprintf(dataDir, sizeof dataDir, "%s", d);
  else {
    const char *h = getenv("HOME");
    snprintf(dataDir, sizeof dataDir, "%s/Library/Application Support/%s", h ? h : "/tmp", appId);
  }
  mkdirs(dataDir);
  { /* one app per data folder (two would write over each other's settings); macOS brings a running app forward
       itself when it is opened again, so a second copy started another way just leaves */
    char lp[1100];
    snprintf(lp, sizeof lp, "%s/.lock", dataDir);
    int fd = open(lp, O_RDWR | O_CREAT, 0644);
    if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) && errno == EWOULDBLOCK) {
      fprintf(stderr, "[start] already running for %s\n", dataDir);
      exit(0);
    } /* (the descriptor stays open for the app's life: the lock goes with the process) */
  }
  applog_start(dataDir);
  { /* what the screen and decoders can do, for choosing streams (best.c): mpv decodes everything in software if it
       must, so only the screen limits the size; HDR is tone-mapped to SDR for now */
    int lines = main_display_lines();
    char caps[160];
    snprintf(caps, sizeof caps, "maxRes=%d,hdr=0,dv=0,av1=1,dts=1,truehd=1,ac3=1,eac3=1", lines >= 1440 ? 2160 : 1080);
    setenv("SB_TVCAPS", caps, 0);
  }
  {
    char ver[32] = "", model[64] = "";
    sysctl_str("kern.osproductversion", ver, sizeof ver);
    sysctl_str("hw.model", model, sizeof model);
    snprintf(devName, sizeof devName, "macOS %s %s", ver[0] ? ver : "?", model);
  }
  fprintf(stderr, "[start] %s, data %s, assets %s\n", devName, dataDir, assetDir);
}

/* While the window is resized, macOS runs its own loop and the app's stops: the picture would freeze until the mouse
   is let go. SDL reports each new size from inside that loop; a frame is drawn there through the app's callback. */
static void (*liveDraw)(void);
static int live_watch(void *ud, SDL_Event *e) {
  (void)ud;
  if (liveDraw && e->type == SDL_WINDOWEVENT &&
      (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e->window.event == SDL_WINDOWEVENT_EXPOSED))
    liveDraw();
  return 1;
}
void plat_live_draw(void (*fn)(void)) {
  if (!liveDraw)
    SDL_AddEventWatch(live_watch, NULL);
  liveDraw = fn;
}
void plat_post_window(SDL_Window *w) {
  sw = w;
  SDL_SetWindowMinimumSize(w, 640, 360);
  if (G.have) {
    SDL_SetWindowPosition(w, G.x, G.y);
    if (G.max)
      SDL_MaximizeWindow(w);
    fprintf(stderr, "[window] as left: %d,%d %dx%d%s%s\n", G.x, G.y, G.w, G.h, G.max ? ", maximised" : "",
            G.full ? ", fullscreen" : "");
  }
  if (getenv("SB_HIDE"))
    return; /* tests: the window stays hidden (it still draws) */
  SDL_ShowWindow(w);
  SDL_RaiseWindow(w);
  if (G.have && G.full)
    SDL_SetWindowFullscreen(w, SDL_WINDOW_FULLSCREEN_DESKTOP);
}
void plat_cursor_visible(int on) {
  SDL_ShowCursor(on ? SDL_ENABLE : SDL_DISABLE);
} /* (the app hides it while the arrows are used, shows it when the mouse moves) */
const char *plat_asset_dir(void) {
  return assetDir;
}
const char *plat_data_dir(void) {
  return dataDir;
}
/* ---- the window: where it was left (store key winGeom: "x y w h maximised fullscreen"), else 16:9 at 80 % of the
   screen's free area; SB_WINDOW=1280x720 for tests (centred, never the saved place) ---- */
static int on_a_screen(int x, int y, int w,
                       int h) { /* most of it on some display (one unplugged since must not strand it) */
  for (int i = 0; i < SDL_GetNumVideoDisplays(); i++) {
    SDL_Rect b;
    if (SDL_GetDisplayUsableBounds(i, &b))
      continue;
    int ix = (x > b.x ? x : b.x), iy = (y > b.y ? y : b.y), ax = (x + w < b.x + b.w ? x + w : b.x + b.w),
        ay = (y + h < b.y + b.h ? y + h : b.y + b.h);
    if (ax - ix >= w / 2 && ay - iy >= h / 2)
      return 1;
  }
  return 0;
}
void plat_window_size(int *w, int *h, Uint32 *flags) {
  int ww = 1280, hh = 720;
  const char *s = getenv("SB_WINDOW");
  int test = testSize = s && sscanf(s, "%dx%d", &ww, &hh) == 2 && ww > 100 && hh > 100;
  if (!test) {
    SDL_Rect ub;
    if (SDL_GetDisplayUsableBounds(0, &ub) == 0) {
      ww = ub.w * 8 / 10;
      hh = ww * 9 / 16;
      if (hh > ub.h * 8 / 10) {
        hh = ub.h * 8 / 10;
        ww = hh * 16 / 9;
      }
    }
    const char *g = store_get("winGeom", NULL);
    if (g && sscanf(g, "%d %d %d %d %d %d", &G.x, &G.y, &G.w, &G.h, &G.max, &G.full) == 6 && G.w >= 640 && G.h >= 360 &&
        on_a_screen(G.x, G.y, G.w, G.h)) {
      G.have = 1;
      ww = G.w;
      hh = G.h;
    }
  }
  *w = ww;
  *h = hh;
  *flags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE |
           SDL_WINDOW_HIDDEN; /* (shown in plat_post_window, in its place) */
}
static void save_geom(void) {
  if (!sw || testSize)
    return;
  Uint32 f = SDL_GetWindowFlags(sw); /* (a test's fixed size is never kept) */
  if (!(f & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_MINIMIZED))) {
    SDL_GetWindowPosition(sw, &G.x, &G.y);
    SDL_GetWindowSize(sw, &G.w, &G.h);
  } /* (the normal place, kept under a maximised one) */
  G.max = (f & SDL_WINDOW_MAXIMIZED) != 0;
  G.full = (f & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP;
  if (G.w < 640 || G.h < 360)
    return;
  char b[96];
  snprintf(b, sizeof b, "%d %d %d %d %d %d", G.x, G.y, G.w, G.h, G.max, G.full);
  const char *o = store_get("winGeom", "");
  if (strcmp(o, b))
    store_set("winGeom", b);
}
int plat_fullscreen_on(void) {
  return sw && (SDL_GetWindowFlags(sw) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP;
}
void plat_fullscreen_toggle(void) {
  if (!sw)
    return;
  int full = plat_fullscreen_on();
  SDL_SetWindowFullscreen(sw, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
  fprintf(stderr, "[window] %s\n", full ? "windowed" : "fullscreen");
  save_geom();
}
/* the keyboard: arrows, Return and Escape are the remote's arrows, OK and Back; Delete erases where text is typed;
   Ctrl+Command+F or F11: fullscreen; Command+F: Search; Command+V: paste on a code card; F1-F4: the colour keys;
   Space and the media keys: play and pause. Command+Q quits (SDL's own menu) */
int plat_map_key(const SDL_KeyboardEvent *e) {
  SDL_Keycode k = e->keysym.sym;
  int cmd = (e->keysym.mod & KMOD_GUI) != 0, ctrl = (e->keysym.mod & KMOD_CTRL) != 0,
      alt = (e->keysym.mod & KMOD_ALT) != 0;
  if (k == SDLK_F11 || (cmd && ctrl && k == SDLK_f)) {
    if (e->type == SDL_KEYDOWN && !e->repeat)
      plat_fullscreen_toggle();
    return K_NONE;
  }
  if (cmd && k == SDLK_f)
    return K_GREEN;
  if (cmd && k == SDLK_v)
    return K_PASTE;
  if (cmd || ctrl || alt)
    return K_NONE; /* (Command+Tab, Command+C...: not the app's keys) */
  switch (k) {
  case SDLK_LEFT:
    return K_LEFT;
  case SDLK_RIGHT:
    return K_RIGHT;
  case SDLK_UP:
    return K_UP;
  case SDLK_DOWN:
    return K_DOWN;
  case SDLK_RETURN:
  case SDLK_KP_ENTER:
    return K_OK;
  case SDLK_BACKSPACE:
    return K_ERASE;
  case SDLK_ESCAPE:
  case SDLK_AC_BACK:
    return K_BACK;
  case SDLK_SPACE:
  case SDLK_AUDIOPLAY:
    return K_PLAYPAUSE;
  case SDLK_AUDIOSTOP:
    return K_STOP;
  case SDLK_F1:
    return K_RED;
  case SDLK_F2:
    return K_GREEN;
  case SDLK_F3:
    return K_YELLOW;
  case SDLK_F4:
    return K_BLUE;
  case SDLK_COMMA:
  case SDLK_AUDIOREWIND:
    return K_REW;
  case SDLK_PERIOD:
  case SDLK_AUDIOFASTFORWARD:
    return K_FF;
  case SDLK_PAGEUP:
    return K_CH_UP;
  case SDLK_PAGEDOWN:
    return K_CH_DOWN;
  default:
    return K_NONE;
  }
}
const char *plat_device_name(void) {
  return devName;
}
int plat_text_input(void) {
  return getenv("SB_NO_TEXTINPUT") ? 0 : 1;
} /* a keyboard types; SB_NO_TEXTINPUT=1 shows the TV's own */
void plat_thread_background(int nice) {
  if (nice > 0)
    pthread_set_qos_class_self_np(nice >= 10 ? QOS_CLASS_BACKGROUND : QOS_CLASS_UTILITY, 0);
}
int plat_mem_total_mb(void) {
  if (getenv("SB_MEMMB"))
    return atoi(getenv("SB_MEMMB")); /* development: act like a small device */
  uint64_t m = 0;
  size_t n = sizeof m;
  return sysctlbyname("hw.memsize", &m, &n, NULL, 0) == 0 ? (int)(m >> 20) : 0;
}
/* minimised and back stands in for the TV sending the app away and back; focus moving to another window is not that */
int plat_lifecycle(const SDL_Event *e) {
  if (e->type == SDL_WINDOWEVENT &&
      (e->window.event == SDL_WINDOWEVENT_MOVED || e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
       e->window.event == SDL_WINDOWEVENT_MAXIMIZED || e->window.event == SDL_WINDOWEVENT_RESTORED))
    save_geom();
  if (e->type == SDL_WINDOWEVENT) {
    if (e->window.event == SDL_WINDOWEVENT_RESTORED)
      return 1;
    if (e->window.event == SDL_WINDOWEVENT_MINIMIZED)
      return -1;
  }
  return 0;
}
static int cf_str(CFStringRef s, char *out, int cap) {
  return s && CFStringGetCString(s, out, cap, kCFStringEncodingUTF8);
}
/* the region set in System Settings > General > Language & Region: the picks' region (Top 10, where titles stream)
   follows it */
const char *plat_country(void) {
  static char cc[4];
  static int done;
  if (done)
    return cc[0] ? cc : NULL;
  done = 1;
  CFLocaleRef l = CFLocaleCopyCurrent();
  char b[8] = "";
  if (l && cf_str(CFLocaleGetValue(l, kCFLocaleCountryCode), b, sizeof b) && strlen(b) == 2)
    snprintf(cc, sizeof cc, "%s", b);
  if (l)
    CFRelease(l);
  return cc[0] ? cc : NULL;
}
/* the first of the user's preferred languages ("en-GB") */
const char *plat_ui_locale(void) {
  if (!locale[0]) {
    CFArrayRef a = CFLocaleCopyPreferredLanguages();
    if (a && CFArrayGetCount(a) > 0)
      cf_str(CFArrayGetValueAtIndex(a, 0), locale, sizeof locale);
    if (a)
      CFRelease(a);
  }
  return locale[0] ? locale : NULL;
}
char *plat_luna_call(const char *uri, const char *payload, int timeoutMs) {
  (void)uri;
  (void)payload;
  (void)timeoutMs;
  return NULL;
}
unsigned long plat_luna_subscribe(const char *uri, const char *payload, PlatLunaCb cb, void *ud) {
  (void)uri;
  (void)payload;
  (void)cb;
  (void)ud;
  return 0;
}
void plat_luna_cancel(unsigned long handle) {
  (void)handle;
}
void plat_luna_pump(void) {}
int plat_luna_send(const char *uri, const char *payload) {
  (void)uri;
  (void)payload;
  return 0;
}
int plat_panel_lines(void) {
  return main_display_lines();
}
int plat_color_keys(void) {
  return 1;
} /* F1-F4 stand in for them */
int plat_random(void *buf, int n) {
  if (n < 0)
    return 0;
  arc4random_buf(buf, (size_t)n);
  return 1;
}

/* phones only (core/form.h): nothing to turn, hide or buzz here */
void plat_orientation(int landscape) {
  (void)landscape;
}
void plat_immersive(int on) {
  (void)on;
}
void plat_haptic(void) {}
void plat_safe_area(int *top, int *bottom, int *left, int *right) {
  *top = *bottom = *left = *right = 0;
}
/* a link in the default browser (the update's DMG, a support page) */
int plat_open_url(const char *url) {
  if (strncmp(url, "https://", 8))
    return 0;
  pid_t pid;
  char *argv[] = { "/usr/bin/open", (char *)url, NULL };
  if (posix_spawn(&pid, argv[0], NULL, NULL, argv, environ))
    return 0;
  waitpid(pid, NULL, 0); /* (open hands the link over and leaves at once) */
  fprintf(stderr, "[open] %.200s\n", url);
  return 1;
}
