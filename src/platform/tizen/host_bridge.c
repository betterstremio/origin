/* Samsung: the bridge between the .NET host (tizentv/) and the app.

   The drawing contract, the same for every host:
   - The host calls sb_host_frame() once per display frame with its GL context current on whatever thread it draws on.
   - The app runs its usual loop (main.c) on a thread of its own. It draws only during a frame call: the call lends it
     the context (released on the host's thread, made current on the app's), the app draws one frame and ends it in
     SDL_GL_SwapWindow (sbh_present), which gives the context back; the call returns and the host shows the frame.
   - Until the app is ready for its first frame, each frame call clears to black (a frame left undrawn shows garbage
     or black on some sets): the host never waits on the app's start.
   - The context and surface are the host's, picked up on the first frame call (never at init: a host may only make
     its context on its drawing thread).

   The exported calls (exports.map keeps everything else inside):
     sb_host_init(art, data, w, h)  the app's folders and the surface's size; starts the app's thread (0 = fine)
     sb_host_frame()                one frame: 1 drawn, 0 nothing new (only with a wait limit), -1 the app has ended (the host closes)
     sb_host_config(waitMs)         optional: how long a frame call waits for the app's frame (never called = without limit)
     sb_host_key(name, down)        a remote key by its Tizen name
     sb_host_life(front)            the app sent to the back (0) or brought to the front (1)
     sb_host_window(native)         the host's native window, for the player's picture
     sb_host_error()                why init failed
     sb_host_playing()              optional: 1 while the app has a film or trailer in the player (loading, playing, paused)
     sb_host_finished()             1 once the app's loop has returned */
#ifdef SB_TPK
#include "host_bridge.h"
#include "core/video.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <locale.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define SBH_API __attribute__((visibility("default")))

extern int main(int argc, char **argv);

enum { TO_HOST, TO_APP };

static struct {
  pthread_mutex_t mu; pthread_cond_t cv;
  int holder;          /* who may draw: TO_HOST between frames, TO_APP during a frame call */
  int parked;          /* the app is waiting for its next frame (so a frame call can hand over) */
  int ended;           /* the app's loop has returned */
  int begun;           /* sb_host_init has run */
  EGLDisplay dpy; EGLSurface surf; EGLContext ctx;
  int w, h;
  void *window;
  long presents; double firstAt;
  int keySeen;
  int waitMs;          /* how long a frame call waits for the app's frame; 0 = without limit (sb_host_config) */
  int lent;            /* a frame call gave up waiting: the app still has the context */
  long skips; double skipSaid;
  char art[512], data[512], err[512], trace[600];
} B = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, TO_HOST, 0, 0, 0, EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_CONTEXT, 1920, 1080 };

static double now_s(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec + tv.tv_usec / 1e6; }

/* ---- the start trace: one line a stage, appended with a single write (whatever happens next, the line is there) ---- */
void sbh_trace(const char *stage, const char *detail) {
  if (!B.trace[0]) return;
  char line[400]; int n = snprintf(line, sizeof line, "%.3f %s%s%s\n", now_s(), stage, detail && detail[0] ? " " : "", detail ? detail : "");
  if (n <= 0) return; if (n >= (int)sizeof line) n = sizeof line - 1;
  int fd = open(B.trace, O_WRONLY | O_APPEND | O_CREAT, 0644); if (fd < 0) return;
  ssize_t w = write(fd, line, (size_t)n); (void)w; close(fd);
}

/* the start before this one, in a line of the app's log (what a shared log tells of a start that went wrong) */
static void trace_report_previous(void) {
  FILE *f = fopen(B.trace, "r"); if (!f) return;
  char prev[700] = "", cur[700] = "", line[400];
  while (fgets(line, sizeof line, f)) {
    char *sp = strchr(line, ' '); if (!sp) continue; char *st = sp + 1; st[strcspn(st, "\n")] = 0;
    if (!strncmp(st, "start", 5) && (st[5] == 0 || st[5] == ' ')) { snprintf(prev, sizeof prev, "%s", cur); cur[0] = 0; }
    size_t l = strlen(cur); if (l < sizeof cur - 2) snprintf(cur + l, sizeof cur - l, "%s%s", l ? " | " : "", st);
  }
  fclose(f);
  if (prev[0]) fprintf(stderr, "[start] the start before: %s\n", prev);
}

/* for a shared log's first lines: this start as the trace has it (the picker's line gives the Tizen version, the TV's
   model, the cores the app may use and the runtime; then the host chosen and every stage reached) */
const char *plat_log_header(void) {
  static char out[1200]; size_t cap = sizeof out; out[0] = 0;
  FILE *f = B.trace[0] ? fopen(B.trace, "r") : NULL; if (!f) return out;
  char cur[800] = "", line[400], pick[300] = "";
  while (fgets(line, sizeof line, f)) {
    char *sp = strchr(line, ' '); if (!sp) continue; char *st = sp + 1; st[strcspn(st, "\n")] = 0;
    if (!strncmp(st, "picker ", 7)) { snprintf(pick, sizeof pick, "%s", st + 7); cur[0] = 0; continue; } /* (a start begins with the picker's line) */
    size_t l = strlen(cur); if (l < sizeof cur - 2) snprintf(cur + l, sizeof cur - l, "%s%s", l ? " | " : "", st);
  }
  fclose(f);
  snprintf(out, cap, "%s%s%s%s", pick[0] ? pick : "", pick[0] && cur[0] ? "\n" : "", cur[0] ? "this start: " : "", cur);
  return out;
}

/* ---- EGL, opened by name at init ----
   The library does not name libEGL among the libraries it needs: a TV without that exact file name would refuse the
   whole library before a line of ours ran (no log, no code). Opened here instead, under the names the TVs have it,
   every call of ours through this table; a TV without it gets a failed start with its reason. */
static struct {
  EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
  EGLBoolean (*QueryContext)(EGLDisplay, EGLContext, EGLint, EGLint *);
  EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
  EGLBoolean (*GetConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint *);
  EGLDisplay (*GetCurrentDisplay)(void);
  EGLSurface (*GetCurrentSurface)(EGLint);
  EGLContext (*GetCurrentContext)(void);
  EGLint     (*GetError)(void);
  EGLBoolean (*QuerySurface)(EGLDisplay, EGLSurface, EGLint, EGLint *);
  int ready;
} E;

/* 0 = every call found; else B.err says what is missing */
static int egl_open(void) {
  static const char *names[] = { "libEGL.so.1", "libEGL.so", NULL };
  void *h = NULL; char why[300] = "";
  for (int i = 0; names[i] && !h; i++) { h = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL); if (!h) { const char *e = dlerror(); size_t l = strlen(why); snprintf(why + l, sizeof why - l, "%s%s", l ? " | " : "", e ? e : names[i]); } }
  if (!h) { snprintf(B.err, sizeof B.err, "the TV's EGL library could not be opened (%s)", why); return -1; }
  const char *miss = NULL;
#define EGL_GET(f) do { *(void **)&E.f = dlsym(h, "egl" #f); if (!E.f && !miss) miss = "egl" #f; } while (0)
  EGL_GET(MakeCurrent); EGL_GET(QueryContext); EGL_GET(ChooseConfig); EGL_GET(GetConfigAttrib); EGL_GET(GetCurrentDisplay);
  EGL_GET(GetCurrentSurface); EGL_GET(GetCurrentContext); EGL_GET(GetError); EGL_GET(QuerySurface);
#undef EGL_GET
  if (miss) { snprintf(B.err, sizeof B.err, "the TV's EGL library has no %s", miss); return -1; }
  E.ready = 1; return 0;
}

/* ---- the context, lent for one frame ---- */
static void ctx_drop(void) { if (B.dpy != EGL_NO_DISPLAY) E.MakeCurrent(B.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); }
static void ctx_take(void) { if (B.dpy != EGL_NO_DISPLAY && B.surf != EGL_NO_SURFACE && B.ctx != EGL_NO_CONTEXT) E.MakeCurrent(B.dpy, B.surf, B.surf, B.ctx); }

/* the app's side: (its frame done, the turn given back and) parked until a frame call hands it the next one; giving
   back and parking are one step, so a frame call never finds the app between the two (it would draw black) */
static void app_wait_turn(int giveBack) {
  pthread_mutex_lock(&B.mu);
  if (giveBack) B.holder = TO_HOST;
  B.parked = 1; pthread_cond_broadcast(&B.cv);
  while (B.holder != TO_APP && !B.ended) pthread_cond_wait(&B.cv, &B.mu);
  B.parked = 0;
  pthread_mutex_unlock(&B.mu);
}

#ifdef SB_TPK_NETLOG
int tpk_dev_screen_w(void) { return B.w; } int tpk_dev_screen_h(void) { return B.h; } /* (development tools: the surface's size for a screenshot) */
#endif
void sbh_present(void) {
#ifdef SB_TPK_NETLOG
  { extern void tpk_devnet_frame(void); tpk_devnet_frame(); }
#endif
  long n = ++B.presents;
  if (n == 1) { B.firstAt = now_s(); sbh_trace("frame", "the app's first frame"); }
  else if (n == 31) { char d[48]; snprintf(d, sizeof d, "30 more frames in %.2f s", now_s() - B.firstAt); sbh_trace("steady", d); }
  ctx_drop();
  app_wait_turn(1);
  ctx_take();
}

void *sbh_context(void) { return (void *)B.ctx; }
void sbh_drawable(int *w, int *h) { if (w) *w = B.w; if (h) *h = B.h; }
void *sbh_native_window(void) { return B.window; }

int sbh_attribute(SDL_GLattr a, int *v) {
  if (!v) return -1;
  EGLint key = a == SDL_GL_RED_SIZE ? EGL_RED_SIZE : a == SDL_GL_GREEN_SIZE ? EGL_GREEN_SIZE : a == SDL_GL_BLUE_SIZE ? EGL_BLUE_SIZE
             : a == SDL_GL_ALPHA_SIZE ? EGL_ALPHA_SIZE : a == SDL_GL_DEPTH_SIZE ? EGL_DEPTH_SIZE : a == SDL_GL_STENCIL_SIZE ? EGL_STENCIL_SIZE : 0;
  *v = 0; if (!key || B.dpy == EGL_NO_DISPLAY) return 0;
  EGLint id = 0, n = 0; EGLConfig cf; EGLint want[] = { EGL_CONFIG_ID, 0, EGL_NONE };
  if (E.QueryContext(B.dpy, B.ctx, EGL_CONFIG_ID, &id)) { want[1] = id; if (E.ChooseConfig(B.dpy, want, &cf, 1, &n) && n) { EGLint x = 0; if (E.GetConfigAttrib(B.dpy, cf, key, &x)) *v = x; } }
  return 0;
}

/* the package's res/ folder: the art folder's parent, with its slash */
char *sbh_base_path(void) {
  char p[520]; snprintf(p, sizeof p, "%s", B.art);
  size_t n = strlen(p); while (n > 1 && p[n - 1] == '/') p[--n] = 0;
  char *cut = strrchr(p, '/'); if (cut) cut[1] = 0; else snprintf(p, sizeof p, "./");
  return SDL_strdup(p);
}

/* ---- the app's thread ---- */
static void *app_main(void *unused) {
  (void)unused;
  app_wait_turn(0); /* the first frame call: the host's surface is known from here */
  ctx_take();
  SDL_SetMainReady(); /* (the host started us, not SDL's own main) */
  char *argv[] = { "stremioorigin", B.art, NULL };
  int rc = main(2, argv);
  fprintf(stderr, "[host] the app's loop returned %d\n", rc);
  { char d[24]; snprintf(d, sizeof d, "%d", rc); sbh_trace("exit", d); }
  pthread_mutex_lock(&B.mu);
  ctx_drop(); B.ended = 1; B.holder = TO_HOST; pthread_cond_broadcast(&B.cv);
  pthread_mutex_unlock(&B.mu);
  return NULL;
}

SBH_API int sb_host_init(const char *art, const char *data, int w, int h) {
  if (B.begun) return 0;
  B.begun = 1;
  snprintf(B.art, sizeof B.art, "%s", art ? art : "");
  snprintf(B.data, sizeof B.data, "%s", data ? data : "");
  if (B.data[0]) snprintf(B.trace, sizeof B.trace, "%s/start-trace.log", B.data);
  if (w > 0 && h > 0) { B.w = w; B.h = h; }
#ifdef SB_TPK_NETLOG
  { extern void tpk_devnet_start(void); tpk_devnet_start(); }
#endif
  setlocale(LC_NUMERIC, "C");
  setenv("SB_ASSET_DIR", B.art, 1);
  setenv("SB_DATA_DIR", B.data, 1);
  setenv("HOME", B.data, 1);
  setenv("SDL_VIDEODRIVER", "dummy", 1);
  trace_report_previous();
  if (egl_open() < 0) { sbh_trace("fail", B.err); return -1; } /* (before the app's thread: nothing started to undo) */
  pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 8u << 20); /* (the app's loop: 8 MB of stack) */
  pthread_t th; int rc = pthread_create(&th, &at, app_main, NULL); pthread_attr_destroy(&at);
  if (rc) { snprintf(B.err, sizeof B.err, "the app's thread could not start (%d)", rc); sbh_trace("fail", B.err); return -1; }
  pthread_detach(th);
  sbh_trace("init", "the app's thread started");
  return 0;
}

/* the first frame call: the host's display, surface and context, and the surface's size */
static int pick_up_surface(void) {
  B.dpy = E.GetCurrentDisplay(); B.surf = E.GetCurrentSurface(EGL_DRAW); B.ctx = E.GetCurrentContext();
  if (B.dpy == EGL_NO_DISPLAY || B.surf == EGL_NO_SURFACE || B.ctx == EGL_NO_CONTEXT) {
    snprintf(B.err, sizeof B.err, "the host's first frame came without a current GL context (error 0x%x)", E.GetError());
    sbh_trace("fail", B.err); B.ctx = EGL_NO_CONTEXT; return -1; }
  EGLint sw = 0, sh = 0; E.QuerySurface(B.dpy, B.surf, EGL_WIDTH, &sw); E.QuerySurface(B.dpy, B.surf, EGL_HEIGHT, &sh);
  if (sw >= 640 && sh >= 360) { B.w = sw; B.h = sh; }
  int alpha = 0; sbh_attribute(SDL_GL_ALPHA_SIZE, &alpha);
  char d[96]; snprintf(d, sizeof d, "%dx%d, alpha %d bits", B.w, B.h, alpha); sbh_trace("gl", d);
  fprintf(stderr, "[host] the host's GL surface: %s%s\n", d, alpha ? "" : " (no alpha: the player's picture cannot show through)");
  return 0;
}

/* A host whose frames come on the framework's own drawing thread (C, D) sets a limit: a frame of the app's that takes
   longer no longer holds that thread, the call says "nothing new" and the frame is shown by a later call. Hosts that
   never call this (A, B) wait without limit, as they always did. */
SBH_API void sb_host_config(int waitMs) { B.waitMs = waitMs > 0 ? waitMs : 0; }

/* a frame not shown: counted, and said in the log at most every 5 s while it goes on */
static int frame_skipped(void) {
  B.skips++;
  double t = now_s();
  if (t - B.skipSaid >= 5) { B.skipSaid = t; fprintf(stderr, "[host] frames the app was late for: %ld so far (limit %d ms)\n", B.skips, B.waitMs); }
  return 0;
}

SBH_API int sb_host_frame(void) {
  if (B.ended || !E.ready) return -1; /* (no frame without EGL: init has said why) */
  if (B.lent) { /* a call before this one gave up waiting: the context is the app's until it gives the turn back */
    pthread_mutex_lock(&B.mu);
    int ended = B.ended, still = B.holder == TO_APP;
    if (!ended && !still) B.lent = 0;
    pthread_mutex_unlock(&B.mu);
    if (ended) return -1;
    if (still) return frame_skipped(); /* (the context not touched: it is current on the app's thread) */
    ctx_take(); /* the app's late frame is drawn and the context dropped (sbh_present): taken back, the host shows it */
    return 1; }
  if (B.ctx == EGL_NO_CONTEXT && pick_up_surface() < 0) return -1;
  pthread_mutex_lock(&B.mu);
  if (!B.parked) { /* the app still starting: black, the loop kept going */
    pthread_mutex_unlock(&B.mu);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    return 1; }
  ctx_drop();
  B.holder = TO_APP; pthread_cond_broadcast(&B.cv);
  if (B.waitMs <= 0) { while (B.holder == TO_APP && !B.ended) pthread_cond_wait(&B.cv, &B.mu); }
  else { /* (the wall clock: were it set back meanwhile, the wait ends with the app's frame, as without a limit) */
    struct timeval tv; gettimeofday(&tv, NULL);
    struct timespec until; long ns = tv.tv_usec * 1000L + (long)B.waitMs * 1000000L;
    until.tv_sec = tv.tv_sec + ns / 1000000000L; until.tv_nsec = ns % 1000000000L;
    while (B.holder == TO_APP && !B.ended) if (pthread_cond_timedwait(&B.cv, &B.mu, &until)) break;
    if (B.holder == TO_APP && !B.ended) { B.lent = 1; pthread_mutex_unlock(&B.mu); return frame_skipped(); } /* late: the app keeps the context */
  }
  int r = B.ended ? -1 : 1;
  pthread_mutex_unlock(&B.mu);
  ctx_take();
  return r;
}

/* the remote's keys by their Tizen names, as the app's SDL keys (plat_map_key turns them into the app's own) */
static const struct { const char *name; SDL_Keycode key; } KEYS[] = {
  { "Left", SDLK_LEFT }, { "Right", SDLK_RIGHT }, { "Up", SDLK_UP }, { "Down", SDLK_DOWN },
  { "Return", SDLK_RETURN }, { "Select", SDLK_RETURN }, { "Enter", SDLK_RETURN },
  { "XF86Back", SDLK_ESCAPE }, { "Back", SDLK_ESCAPE }, { "Escape", SDLK_ESCAPE },
  { "XF86AudioPlay", SDLK_AUDIOPLAY }, { "MediaPlay", SDLK_AUDIOPLAY }, { "Play", SDLK_AUDIOPLAY },
  { "XF86AudioPause", SDLK_PAUSE }, { "MediaPause", SDLK_PAUSE }, { "Pause", SDLK_PAUSE },
  { "XF86AudioPlayPause", SDLK_F5 }, { "XF86PlayBack", SDLK_F5 }, { "MediaPlayPause", SDLK_F5 }, /* (one key that toggles) */
  { "XF86AudioStop", SDLK_AUDIOSTOP }, { "MediaStop", SDLK_AUDIOSTOP }, { "Stop", SDLK_AUDIOSTOP },
  { "XF86AudioRewind", SDLK_AUDIOREWIND }, { "MediaRewind", SDLK_AUDIOREWIND }, { "Rewind", SDLK_AUDIOREWIND },
  { "XF86AudioForward", SDLK_AUDIOFASTFORWARD }, { "MediaFastForward", SDLK_AUDIOFASTFORWARD }, { "FastForward", SDLK_AUDIOFASTFORWARD },
  { "XF86RaiseChannel", SDLK_PAGEUP }, { "ChannelUp", SDLK_PAGEUP }, { "XF86LowerChannel", SDLK_PAGEDOWN }, { "ChannelDown", SDLK_PAGEDOWN },
  { "XF86Red", SDLK_F1 }, { "Red", SDLK_F1 }, { "XF86Green", SDLK_F2 }, { "Green", SDLK_F2 },
  { "XF86Yellow", SDLK_F3 }, { "Yellow", SDLK_F3 }, { "XF86Blue", SDLK_F4 }, { "Blue", SDLK_F4 },
};

SBH_API void sb_host_key(const char *name, int down) {
  if (!name || !name[0]) return;
  SDL_Keycode k = SDLK_UNKNOWN;
  if (name[0] >= '0' && name[0] <= '9' && !name[1]) k = SDLK_0 + (name[0] - '0');
  for (size_t i = 0; k == SDLK_UNKNOWN && i < sizeof KEYS / sizeof *KEYS; i++) if (!strcmp(KEYS[i].name, name)) k = KEYS[i].key;
  if (down && !B.keySeen) { B.keySeen = 1; sbh_trace("key", name); }
  if (k == SDLK_UNKNOWN) { if (down) fprintf(stderr, "[keys] not used: %s\n", name); return; }
  SDL_Event e; memset(&e, 0, sizeof e);
  e.type = down ? SDL_KEYDOWN : SDL_KEYUP; e.key.state = down ? SDL_PRESSED : SDL_RELEASED; e.key.keysym.sym = k;
  SDL_PushEvent(&e);
}

/* The TV's own keyboard, shown by the host: 1 while the app asks for typed text (Search's keyboard button turns the
   app's text input on; the app's start turns it off, so this follows what is asked and nothing else) */
SBH_API int sb_host_text_wanted(void) { return B.begun && !B.ended && B.presents > 0 && SDL_IsTextInputActive() ? 1 : 0; }
/* what was typed on it (UTF-8), handed on as the app's typed-text events, a few letters an event, never a letter cut */
SBH_API void sb_host_text(const char *utf8) {
  if (!utf8 || !B.begun || B.ended) return;
  size_t n = strlen(utf8), at = 0;
  while (at < n) { size_t take = n - at; if (take > 28) { take = 28; while (take > 0 && ((unsigned char)utf8[at + take] & 0xc0) == 0x80) take--; if (!take) take = 1; }
    SDL_Event e; memset(&e, 0, sizeof e); e.type = SDL_TEXTINPUT; memcpy(e.text.text, utf8 + at, take); e.text.text[take] = 0; SDL_PushEvent(&e); at += take; }
}

/* to the back (Home, another app) or the front: the app's usual events, and the picture paused at once (a host may
   stop calling frames while away, so the app's loop would not get to it) */
SBH_API void sb_host_life(int front) {
  SDL_Event e; memset(&e, 0, sizeof e); e.type = front ? SDL_APP_DIDENTERFOREGROUND : SDL_APP_DIDENTERBACKGROUND; SDL_PushEvent(&e);
  sbh_trace(front ? "front" : "back", NULL);
  if (!front) video_pause_now();
}

SBH_API void sb_host_window(void *native) { B.window = native; }
/* (asked by host D before its silent clip: a second player beside the app's is never opened) */
SBH_API int sb_host_playing(void) { return B.begun && !B.ended && video_state() != VID_IDLE ? 1 : 0; }
SBH_API const char *sb_host_error(void) { return B.err; }
SBH_API int sb_host_finished(void) { return B.ended; }
#endif /* SB_TPK */
