#include "platform/plat.h"
#include "core/keys.h"
#include "core/applog.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/syscall.h>
#include <fcntl.h>

static char assetDir[1024], dataDir[1024], devName[160];

static int ensure_dir(const char *d) {
  char p[1100]; snprintf(p, sizeof p, "%s", d);
  for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
  mkdir(p, 0755);
  struct stat st;
  return (stat(d, &st) == 0 && S_ISDIR(st.st_mode));
}

void plat_pre_init(const char *appId) {
  const char *envArt = getenv("SB_ASSET_DIR");
  if (envArt && envArt[0]) {
    snprintf(assetDir, sizeof assetDir, "%s", envArt);
  } else {
    snprintf(assetDir, sizeof assetDir, "assets");
  }

  const char *envData = getenv("SB_DATA_DIR");
  if (envData && envData[0]) {
    snprintf(dataDir, sizeof dataDir, "%s", envData);
  } else {
    snprintf(dataDir, sizeof dataDir, "/tmp/%s", appId ? appId : "stremioorigin");
  }

  ensure_dir(dataDir);
  applog_start(dataDir);
  fprintf(stderr, "[store] data in %s\n", dataDir);
  fprintf(stderr, "[store] assets in %s\n", assetDir);

  snprintf(devName, sizeof devName, "Samsung Smart TV (Tizen)");
}

void plat_post_window(SDL_Window *w) { (void)w; }

const char *plat_asset_dir(void) { return assetDir; }
const char *plat_data_dir(void) { return dataDir; }

void plat_window_size(int *w, int *h, Uint32 *flags) {
  *w = 1920;
  *h = 1080;
  *flags = 0; /* SDL dummy driver under TPK */
}

int plat_map_key(const SDL_KeyboardEvent *e) {
  switch (e->keysym.sym) {
    case SDLK_LEFT: return K_LEFT;
    case SDLK_RIGHT: return K_RIGHT;
    case SDLK_UP: return K_UP;
    case SDLK_DOWN: return K_DOWN;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return K_OK;
    case SDLK_ESCAPE:
    case SDLK_AC_BACK: return K_BACK;
    case SDLK_AUDIOPLAY: return K_PLAY;
    case SDLK_F5: return K_PLAYPAUSE; /* (host_bridge.c: a smart remote's single play/pause key) */
    case SDLK_PAUSE: return K_PAUSE;
    case SDLK_AUDIOSTOP: return K_STOP;
    case SDLK_AUDIOREWIND: return K_REW;
    case SDLK_AUDIOFASTFORWARD: return K_FF;
    case SDLK_F1: return K_RED;
    case SDLK_F2: return K_GREEN;
    case SDLK_F3: return K_YELLOW;
    case SDLK_F4: return K_BLUE;
    case SDLK_PAGEUP: return K_CH_UP;
    case SDLK_PAGEDOWN: return K_CH_DOWN;
    default: break;
  }
  return K_NONE;
}

int plat_text_input(void) { return 0; }

const char *plat_device_name(void) { return devName; }

int plat_mem_total_mb(void) {
  FILE *f = fopen("/proc/meminfo", "r");
  if (!f) return 1024;
  char line[128];
  long kb = -1;
  while (fgets(line, sizeof line, f)) {
    if (sscanf(line, "MemTotal: %ld kB", &kb) == 1) break;
  }
  fclose(f);
  return kb > 0 ? (int)(kb / 1024) : 1024;
}

void plat_thread_background(int nice) { setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), nice); } /* per thread on Linux: the drawing thread keeps the one core first */

/* sent to the back / brought in front (the host's OnPause / OnResume as SDL's app events, see host_bridge.c): as on webOS */
int plat_lifecycle(const SDL_Event *e) {
  static int away; int in = 0, out = 0;
  if (e->type == SDL_APP_WILLENTERBACKGROUND || e->type == SDL_APP_DIDENTERBACKGROUND) out = 1;
  else if (e->type == SDL_APP_WILLENTERFOREGROUND || e->type == SDL_APP_DIDENTERFOREGROUND) in = 1;
  if (out) { if (!away) fprintf(stderr, "[life] to the back\n"); away = 1; return -1; }
  if (in && away) { away = 0; fprintf(stderr, "[life] back in front\n"); return 1; }
  return 0;
}

/* the TV's menu language (Settings > General > Language): "fr_FR.UTF8" in vconf, given as "fr-FR" */
const char *plat_ui_locale(void) {
  static char loc[32]; if (loc[0]) return loc;
  char *v = NULL; void *h = dlopen("libvconf.so.0", RTLD_NOW); if (!h) h = dlopen("libvconf.so", RTLD_NOW);
  char *(*get)(const char *) = h ? (char *(*)(const char *))dlsym(h, "vconf_get_str") : NULL;
  if (get) v = get("db/menu_widget/language");
  const char *src = v && *v ? v : getenv("LANG");
  if (src && *src) { snprintf(loc, sizeof loc, "%s", src); loc[strcspn(loc, ".@")] = 0; for (char *c = loc; *c; c++) if (*c == '_') *c = '-'; }
  free(v); if (!loc[0]) snprintf(loc, sizeof loc, "en-US");
  fprintf(stderr, "[start] TV language %s\n", loc); return loc;
}

char *plat_luna_call(const char *uri, const char *payload, int timeoutMs) {
  (void)uri; (void)payload; (void)timeoutMs;
  return NULL;
}

unsigned long plat_luna_subscribe(const char *uri, const char *payload, PlatLunaCb cb, void *ud) {
  (void)uri; (void)payload; (void)cb; (void)ud;
  return 0;
}

void plat_luna_cancel(unsigned long handle) {
  (void)handle;
}

void plat_luna_pump(void) {}

int plat_luna_send(const char *uri, const char *payload) {
  (void)uri; (void)payload;
  return 0;
}

void plat_cursor_visible(int on) {
  (void)on;
}

int plat_panel_lines(void) {
  return 1080;
}

int plat_color_keys(void) { return 0; } /* Samsung Smart Remotes may not have dedicated colour keys. */

int plat_random(void *buf, int n) {
  FILE *f = fopen("/dev/urandom", "rb");
  if (!f)
    return 0;
  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  return got == (size_t)n;
}

void plat_orientation(int landscape) { (void)landscape; }
void plat_immersive(int on) { (void)on; }
void plat_haptic(void) {}
void plat_safe_area(int *top, int *bottom, int *left, int *right) {
  *top = *bottom = *left = *right = 0;
}
int plat_brightness_get(void) {
  return -1;
}
int plat_brightness_set(int pct) {
  (void)pct;
  return 0;
}
int plat_sysvol_get(void) {
  return -1;
}
int plat_sysvol_set(int pct) {
  (void)pct;
  return 0;
}
void plat_gesture_insets(int *left, int *top, int *right, int *bottom) {
  *left = *top = *right = *bottom = 0;
}
unsigned plat_touch_cancel_event(void) {
  return 0;
}
int plat_open_url(const char *url) { (void)url; return 0; }
