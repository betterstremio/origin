#include "core/form.h"
#include "core/touch.h"
#include "screens/phone.h"
#include <stdint.h>
#include <math.h>
/* Origin, native. The window, the GL context, the frame loop; everything else is in
   core/, ui/ and screens/. */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <unistd.h>
#include <signal.h>
#if SB_PREVIEW
#include <execinfo.h>
static void on_alarm(int sig) { (void)sig; void *fr[48]; int n = backtrace(fr, 48); backtrace_symbols_fd(fr, n, 2); _exit(3); }
static volatile int profN; /* the profiler is the TV's */
#elif SB_CRASH_TO_SYSTEM
/* Android: the crash is logged, then handed back to the system, which writes its own report (a tombstone, in logcat) */
static volatile int profN;
static void prof_start(void) {}
static void prof_report(void) {}
static void on_crash(int sig, siginfo_t *si, void *uc) { (void)uc;
  char b[160]; int n = snprintf(b, sizeof b, "[crash] signal %d addr %p\n", sig, si ? si->si_addr : NULL); if (write(2, b, n) < 0) {}
  signal(sig, SIG_DFL); raise(sig); }
#else
#include <ucontext.h>
#include <time.h>
/* a crash on the TV: the fault address, the program counter and the link register plus where the binary is
   mapped, enough to resolve the place from the unstripped build (build/webos/stremioorigin.dbg) */
/* SB_PROF=1: a sampling profiler (SIGPROF every 2 ms records the program counter; the frame log prints the
   hottest addresses as offsets into the binary, to resolve with `nm -n build/webos/stremioorigin.dbg`) */
#include <sys/time.h>
#define PROF_N 8192
static unsigned long profPc[PROF_N], profLr[PROF_N], profUp[PROF_N][3]; static volatile int profN; static unsigned long profBase; static unsigned long stackLo, stackHi;
#include <sys/syscall.h>
static long profTid; /* SB_PROF=main: only the drawing thread's samples */
static void on_prof(int sig, siginfo_t *si, void *uc) { (void)sig; (void)si; if (profTid && syscall(SYS_gettid) != profTid) return; ucontext_t *u = uc; int n = profN; if (n < PROF_N) { profPc[n] = u->uc_mcontext.arm_pc; profLr[n] = u->uc_mcontext.arm_lr;
    /* three callers up the frame-pointer chain (a build with SB_FP=1: [fp] = the caller's fp, [fp + 4] = its return address) */
    unsigned long fp = u->uc_mcontext.arm_fp; for (int k = 0; k < 3; k++) { if (fp < stackLo || fp + 8 > stackHi || (fp & 3)) { profUp[n][k] = 0; fp = 0; continue; } profUp[n][k] = ((unsigned long *)fp)[1]; fp = ((unsigned long *)fp)[0]; }
    profN = n + 1; } }
static void prof_maps(void) { FILE *m = fopen("/proc/self/maps", "r"); if (!m) return; char l[300]; while (fgets(l, sizeof l, m)) if (strstr(l, "r-xp") && strchr(l, '/')) fprintf(stderr, "[maps] %s", l); fclose(m); }
/* the hottest 64-byte buckets, and for the hottest one the callers (link register) in our own binary */
static void prof_report(void) {
  static int once; if (!once) { once = 1; prof_maps(); }
  int n = profN; if (!n) return;
  struct { unsigned long a; int c; } b[256]; int nb = 0;
  for (int i = 0; i < n; i++) { unsigned long k = profPc[i] & ~63ul; int j; for (j = 0; j < nb; j++) if (b[j].a == k) break; if (j == nb) { if (nb == 256) continue; b[nb].a = k; b[nb].c = 0; nb++; } b[j].c++; }
  for (int i = 1; i < nb; i++) for (int j = i; j > 0 && b[j].c > b[j - 1].c; j--) { __typeof__(b[0]) t = b[j]; b[j] = b[j - 1]; b[j - 1] = t; }
  fprintf(stderr, "[prof] %d samples, base %lx:", n, profBase); for (int i = 0; i < nb && i < 24; i++) fprintf(stderr, " %lx=%d", b[i].a, b[i].c); fprintf(stderr, "\n");
  if (nb) { struct { unsigned long a; int c; } c[64]; int nc = 0; for (int i = 0; i < n; i++) { if ((profPc[i] & ~63ul) != b[0].a) continue; unsigned long k = profLr[i] & ~15ul; int j; for (j = 0; j < nc; j++) if (c[j].a == k) break; if (j == nc) { if (nc == 64) continue; c[nc].a = k; c[nc].c = 0; nc++; } c[j].c++; }
    for (int i = 1; i < nc; i++) for (int j = i; j > 0 && c[j].c > c[j - 1].c; j--) { __typeof__(c[0]) t = c[j]; c[j] = c[j - 1]; c[j - 1] = t; }
    fprintf(stderr, "[prof] callers of %lx:", b[0].a); for (int i = 0; i < nc && i < 8; i++) fprintf(stderr, " %lx(+%lx)=%d", c[i].a, c[i].a - profBase, c[i].c); fprintf(stderr, "\n"); }
  if (getenv("SB_PROF_RAW")) for (int i = 0; i < n; i += 8) { char l[400]; int k = 0; for (int j = i; j < n && j < i + 8; j++) k += snprintf(l + k, sizeof l - k, " %lx:%lx:%lx:%lx:%lx", profPc[j], profLr[j], profUp[j][0], profUp[j][1], profUp[j][2]); fprintf(stderr, "[praw]%s\n", l); } /* every sample, for resolving offline */
  profN = 0;
}
static void prof_start(void) {
  if (strcmp(getenv("SB_PROF"), "1")) profTid = syscall(SYS_gettid); /* main / slow: the drawing thread only */
  { FILE *m = fopen("/proc/self/maps", "r"); char l[300]; while (m && fgets(l, sizeof l, m)) if (strstr(l, "[stack]")) { stackLo = strtoul(l, NULL, 16); stackHi = strtoul(strchr(l, '-') + 1, NULL, 16); } if (m) fclose(m); }
  FILE *m = fopen("/proc/self/maps", "r"); if (m) { char l[300]; while (fgets(l, sizeof l, m)) { if (strstr(l, "r-xp") && strchr(l, '/')) { if (!profBase && strstr(l, "stremioorigin")) profBase = strtoul(l, NULL, 16); fprintf(stderr, "[maps] %s", l); } } fclose(m); } /* where each library sits: the samples are absolute */
  struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_prof; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigaction(SIGPROF, &sa, NULL);
  if (!strcmp(getenv("SB_PROF"), "wall") || !strcmp(getenv("SB_PROF"), "slow")) {
    /* wall-clock samples of the drawing thread alone, waiting included (a frame lost to a lock or the driver shows
       where it waits; CPU-time samples miss it) */
    struct { union { int i; void *p; } v; int signo, notify; int tid; int pad[12]; } ev; memset(&ev, 0, sizeof ev);
    ev.signo = SIGPROF; ev.notify = 4 /* SIGEV_THREAD_ID */; ev.tid = (int)profTid; int timer = -1;
    if (syscall(SYS_timer_create, CLOCK_MONOTONIC, &ev, &timer) == 0) { struct itimerspec ts = { { 0, 2000000 }, { 0, 2000000 } }; syscall(SYS_timer_settime, timer, 0, &ts, NULL); return; }
    fprintf(stderr, "[prof] no thread timer, CPU time instead\n"); }
  struct itimerval it = { { 0, 2000 }, { 0, 2000 } }; setitimer(ITIMER_PROF, &it, NULL);
}
static void on_crash(int sig, siginfo_t *si, void *uc) {
  ucontext_t *u = uc; char b[512]; int n = snprintf(b, sizeof b, "[crash] signal %d addr %p pc %p lr %p sp %p\n", sig, si ? si->si_addr : NULL, (void *)u->uc_mcontext.arm_pc, (void *)u->uc_mcontext.arm_lr, (void *)u->uc_mcontext.arm_sp);
  if (write(2, b, n) < 0) {}
  FILE *m = fopen("/proc/self/maps", "r"); if (m) { char l[300]; while (fgets(l, sizeof l, m)) if (strstr(l, "stremioorigin")) fputs(l, stderr); fclose(m); }
  fflush(stderr); _exit(4);
}
#endif
#include "core/castlink.h"
#include "core/applog.h"
#include "core/httpd.h"
#include "platform/plat.h"
#include "ui/gfx.h"
#include "ui/text.h"
#include "ui/img.h"
#include "ui/nerds.h"
#include "platform/mem.h"
#include "ui/asssubs.h"
#include "ui/icon.h"
#include "core/keys.h"
#include "core/app.h"
#include "core/bgtrailer.h"
#include "core/pace.h"
#include "core/stress.h"
#include "core/nuvio.h"
#include "screens/profiles.h"
#include "core/video.h"
#include "core/p2p.h"
static int vsyncOn __attribute__((unused));   /* SB_VSYNC=1: let the driver wait for the screen instead of pacing ourselves */
#ifndef SB_VSYNC_DEFAULT
#define SB_VSYNC_DEFAULT 0
#endif
#if SB_PLAT_GL /* the platform makes the GL surface (Windows: a composition layer above the video's window) */
#define GL_MAKE(w) plat_gl_create(w)
#define GL_SWAP(w) plat_gl_swap(w)
#define GL_INTERVAL(n) plat_gl_interval(n)
#define GL_SIZE(w, pw, ph) plat_gl_size(w, pw, ph)
#define GL_FREE(c) plat_gl_destroy(c)
#else
#define GL_MAKE(w) SDL_GL_CreateContext(w)
#define GL_SWAP(w) SDL_GL_SwapWindow(w)
#define GL_INTERVAL(n) SDL_GL_SetSwapInterval(n)
#define GL_SIZE(w, pw, ph) SDL_GL_GetDrawableSize(w, pw, ph)
#define GL_FREE(c) SDL_GL_DeleteContext(c)
#endif
#include "core/net.h"
#include "core/store.h"
#include "core/i18n.h"
#include "core/stremio.h"
#include "screens/board.h"
#include "screens/login.h"
#include "screens/detail.h"
#include "screens/similar.h"
#include "screens/catgrid.h"
#include "screens/cwmenu.h"
#include "screens/search.h"
#include "screens/settings.h"
#include "screens/splash.h"
#include "screens/player.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#ifndef SB_APP_ID
#define SB_APP_ID "tv.stremioorigin.app"
#endif
#define APP_ID SB_APP_ID   /* the package id: tools/build.sh APPID=... */

static void screenshot(const char *path, int w, int h) {
  unsigned char *px = malloc((size_t)w * h * 4), *flip = malloc((size_t)w * h * 4);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
  for (int y = 0; y < h; y++) memcpy(flip + (size_t)y * w * 4, px + (size_t)(h - 1 - y) * w * 4, (size_t)w * 4);
  for (size_t i = 3; i < (size_t)w * h * 4; i += 4) flip[i] = 255;
  stbi_write_png(path, w, h, 4, flip, w * 4); free(px); free(flip);
  fprintf(stderr, "[shot] %s\n", path);
}
/* SB_PLAY_FROM="Mock Addon", "*p2p" or "*" (any addon) (with SB_PLAY_TYPE / SB_PLAY_ID / SB_PLAY_VID): plays that addon's first stream */
static int playFromDone;
static void pf_part(StreamGroup *g, int gi, void *ud) { (void)gi; (void)ud;
  int nth = getenv("SB_PLAY_NTH") ? atoi(getenv("SB_PLAY_NTH")) : 0; /* the nth of its streams (an externalUrl one too) */
  for (int i = 0; i < g->n && !playFromDone; i++) if ((!strcmp(getenv("SB_PLAY_FROM"), "*p2p") ? g->items[i].p2p : !strcmp(getenv("SB_PLAY_FROM"), "*") ? !g->items[i].p2p : !strcmp(g->items[i].addon, getenv("SB_PLAY_FROM"))) && (nth ? i == nth : g->items[i].url[0] != 0)) { /* "*p2p": the first P2P stream of any addon; "*": the first other one */ playFromDone = 1; Stream s = g->items[i];
    fprintf(stderr, "[dev] playing %s's stream %d: %.120s\n", s.addon, i, s.name);
    const char *ty = getenv("SB_PLAY_TYPE") ? getenv("SB_PLAY_TYPE") : "movie", *id = getenv("SB_PLAY_ID") ? getenv("SB_PLAY_ID") : "tt0111161"; app_play(&s, ty, id, getenv("SB_PLAY_VID") ? getenv("SB_PLAY_VID") : id, 0); }
  free(g->items); }
static void pf_done(int total, void *ud) { (void)ud; if (!playFromDone) fprintf(stderr, "[dev] no stream from %s (%d in all)\n", getenv("SB_PLAY_FROM"), total); }
static void play_from_addon(void) { const char *id = getenv("SB_PLAY_ID") ? getenv("SB_PLAY_ID") : "tt0111161"; sb_streams(getenv("SB_PLAY_TYPE") ? getenv("SB_PLAY_TYPE") : "movie", getenv("SB_PLAY_VID") ? getenv("SB_PLAY_VID") : id, pf_part, pf_done, NULL); }
/* SB_SCRIPT="40:right,60:down,80:ok" presses keys at those frames (for screenshots of deeper screens) */
static int script_key(const char *name) {
  static const struct { const char *n; int k; } M[] = { { "left", K_LEFT }, { "right", K_RIGHT }, { "up", K_UP }, { "down", K_DOWN }, { "ok", K_OK }, { "back", K_BACK }, { "red", K_RED }, { "green", K_GREEN }, { "play", K_PLAYPAUSE } };
  for (size_t i = 0; i < sizeof M / sizeof M[0]; i++) if (!strcmp(M[i].n, name)) return M[i].k; return 0;
}
__attribute__((noinline)) static void crash_deep(int n) { volatile int *z = (volatile int *)(uintptr_t)0x8; if (n > 0) { crash_deep(n - 1); __asm__ volatile(""); } else *z = 1; } /* development: a crash a few calls deep (SB_CRASH_AT) */
static void run_script(const char *script, int frame) {
  if (!script) return; char buf[1024]; snprintf(buf, sizeof buf, "%s", script);
  for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) { int f = atoi(tok); char *c = strchr(tok, ':'); if (!c) continue;
    if (f == frame && !strncmp(c + 1, "move ", 5)) { float x, y; if (sscanf(c + 6, "%f %f", &x, &y) == 2) app_pointer_move(x, y); continue; } /* "300:move 400 600", "320:click 400 600", "340:wheel -1" */
    if (f == frame && !strncmp(c + 1, "click ", 6)) { float x, y; if (sscanf(c + 7, "%f %f", &x, &y) == 2) { app_pointer_button(1, x, y); app_pointer_button(0, x, y); } continue; }
    if (f == frame && !strncmp(c + 1, "wheel ", 6)) { app_pointer_wheel(0, (float)atof(c + 7)); continue; }
    if (f == frame && !strncmp(c + 1, "text ", 5)) { app_text(c + 6); continue; } /* "400:text abc": typed on a keyboard */
    /* a finger (phones): "300:fdown 200 500,302:fmove 200 300,304:fup 200 300" drags, "320:tap 100 200" taps; a
       long press is an fdown left for 30 frames or more before its fup */
    if (f == frame && !strncmp(c + 1, "fdown ", 6)) { float x, y; if (sscanf(c + 7, "%f %f", &x, &y) == 2) touch_down(x, y); continue; }
    if (f == frame && !strncmp(c + 1, "fmove ", 6)) { float x, y; if (sscanf(c + 7, "%f %f", &x, &y) == 2) touch_move(x, y); continue; }
    if (f == frame && !strncmp(c + 1, "fup ", 4)) { float x, y; if (sscanf(c + 5, "%f %f", &x, &y) == 2) touch_up(x, y); continue; }
    if (f == frame && !strncmp(c + 1, "tap ", 4)) { float x, y; if (sscanf(c + 5, "%f %f", &x, &y) == 2) { touch_down(x, y); touch_up(x, y); } continue; }
    if (f == frame && !strncmp(c + 1, "hwheel ", 7)) { app_pointer_wheel((float)atof(c + 8), 0); continue; } /* "360:hwheel 1": sideways (right) */
    if (f == frame && !strncmp(c + 1, "rclick ", 7)) { float x, y; if (sscanf(c + 8, "%f %f", &x, &y) == 2 && !app_pointer_menu(x, y)) { app_key(K_BACK, 1, 0); app_key(K_BACK, 0, 0); } continue; }
    if (f == frame && !strncmp(c + 1, "press ", 6)) { float x, y; if (sscanf(c + 7, "%f %f", &x, &y) == 2) app_pointer_button(1, x, y); continue; } /* "press"/"release": a drag in between with "move" */
    if (f == frame && !strncmp(c + 1, "release ", 8)) { float x, y; if (sscanf(c + 9, "%f %f", &x, &y) == 2) app_pointer_button(0, x, y); continue; }
    if (f == frame) { int k = script_key(c + 1); if (k) { app_key(k, 1, 0); app_key(k, 0, 0); } }
    else if (f + 40 == frame && !strncmp(c + 1, "hold", 4)) { app_key(K_OK, 0, 0); }
    else if (f == frame + 0 && 0) {}
    if (f == frame && !strncmp(c + 1, "hold", 4)) app_key(K_OK, 1, 0); }
}

/* development: SB_P2PTEST=p2p://<hash>/<idx> resolves a P2P stream; with SB_P2PCYCLES=N it then reads it like a player
   (the head, the tail, a seek to 40 %, SB_P2PREADMB MB each), closes it, and does it again N times (SB_P2PIDLE=5 stops
   the engine between cycles), then quits: start/stop cycles for leak checks (leaks --atExit on the Mac) */
static struct { int cycle, cycles; double next; volatile int readDone; char url[600]; uint64_t size; } PT;
static void *p2p_test_read(void *a) { (void)a;
#if SB_PREVIEW
  uint64_t mb = getenv("SB_P2PREADMB") ? (uint64_t)atoi(getenv("SB_P2PREADMB")) : 8, n = mb << 20, s = PT.size, mid = s / 5 * 2;
  uint64_t r[3][2] = { { 0, n - 1 }, { s > (2u << 20) ? s - (2u << 20) : 0, s - 1 }, { mid, mid + n - 1 } };
  for (int i = 0; i < 3; i++) { char cmd[1000]; Uint32 t0 = SDL_GetTicks();
    snprintf(cmd, sizeof cmd, "curl -s -o /dev/null --max-time 180 -r %llu-%llu -w '%%{http_code} %%{size_download}' '%s'", (unsigned long long)r[i][0], (unsigned long long)r[i][1], PT.url);
    FILE *f = popen(cmd, "r"); char out[64] = ""; if (f) { if (!fgets(out, sizeof out, f)) out[0] = 0; pclose(f); }
    fprintf(stderr, "[p2ptest] cycle %d read %s: %s in %.1f s\n", PT.cycle + 1, i == 0 ? "head" : i == 1 ? "tail" : "seek 40%", out, (SDL_GetTicks() - t0) / 1000.0); }
#endif
  PT.readDone = 1; return NULL; }
static void p2p_test_done(void *ud, const char *url, const char *err, uint64_t size) { (void)ud;
  if (!url) { fprintf(stderr, "[p2ptest] failed: %s\n", err); PT.readDone = 1; return; }
  fprintf(stderr, "[p2ptest] url %s %llu MB\n", url, (unsigned long long)(size >> 20));
  if (PT.cycles) { snprintf(PT.url, sizeof PT.url, "%s", url); PT.size = size; pthread_t th; if (!pthread_create(&th, NULL, p2p_test_read, NULL)) pthread_detach(th); else PT.readDone = 1; }
}
static void p2p_test_tick(int frame) {
  if (!getenv("SB_P2PTEST")) return;
  if (frame == 30) { PT.cycles = getenv("SB_P2PCYCLES") ? atoi(getenv("SB_P2PCYCLES")) : 0; p2p_resolve(getenv("SB_P2PTEST"), p2p_test_done, NULL); }
  if (frame % 120 == 0 && p2p_status()[0]) fprintf(stderr, "[p2ptest] %s\n", p2p_status());
  if (!PT.cycles) return;
  double t = SDL_GetTicks() / 1000.0;
  if (PT.readDone) { PT.readDone = 0; p2p_player_closed(); PT.cycle++; fprintf(stderr, "[p2ptest] cycle %d done\n", PT.cycle);
    double idle = getenv("SB_P2PIDLE") ? atof(getenv("SB_P2PIDLE")) : 300; PT.next = t + (PT.cycle % 2 ? idle + 4 : 2); } /* every other gap lets the engine stop */
  if (PT.next > 0 && t >= PT.next) { PT.next = 0;
    if (PT.cycle < PT.cycles) p2p_resolve(getenv("SB_P2PTEST"), p2p_test_done, NULL);
    else { fprintf(stderr, "[p2ptest] %d cycles done, quitting\n", PT.cycles); app_quit(); } }
}
/* the mouse's place in layout units through window and drawable pixels */
static void to_layout(SDL_Window *win, int mx, int my, float *x, float *y) {
  int ww, wh, pw, ph, vx, vy, vw, vh; SDL_GetWindowSize(win, &ww, &wh); GL_SIZE(win, &pw, &ph); gfx_viewport(&vx, &vy, &vw, &vh);
  float kx = ww > 0 ? (float)pw / ww : 1, ky = wh > 0 ? (float)ph / wh : 1; *x = vw > 0 ? (mx * kx - vx) * SB_W / vw : 0; *y = vh > 0 ? (my * ky - vy) * SB_H / vh : 0;
}
#if SB_WINDOWED
/* a desktop: the swap waits for the monitor, at most 120 frames a second (a 240 Hz one draws every second refresh), and the
   pacer is told that rate, so motion steps evenly; again when the window moves to another monitor */
/* Nothing pressed for 2 s and no film open: at most 60 frames a second (a 240 Hz monitor: every 4th refresh), which
   halves what an idle Home costs on a fast monitor; the next key, click or move brings the full rate back that frame.
   Motion keeps its speed: the pacer hears of the rate */
static int dHz = 60, dK = 1, dIdle;
static int idle_k(void) { int k = (dHz + 59) / 60; if (k < dK) k = dK; return k > 4 ? 4 : k; }
static void desktop_apply(void) { int k = dIdle ? idle_k() : dK; GL_INTERVAL(k); pace_set_hz(dHz / k); }
/* one frame while the window's border is dragged (plat_live_draw: Windows' own loop runs then, not this one): the new
   size, a tick and the drawing, as the loop below does them */
static SDL_Window *liveWin;
static void live_frame(void) {
  static Uint32 t0; Uint32 now = SDL_GetTicks(); float dt = t0 ? (now - t0) / 1000.0f : 0.016f; t0 = now; if (dt > 0.05f) dt = 0.05f;
  int dw, dh; GL_SIZE(liveWin, &dw, &dh); if (dw <= 0 || dh <= 0) return; gfx_resize(dw, dh);
  text_frame_begin(); app_tick(dt); splash_tick(dt);
  gfx_begin(player_live() ? RGBA(0, 0, 0, 0) : PAGE); app_draw(); splash_draw(); app_clock_top(); gfx_end();
  GL_SWAP(liveWin);
}
static void desktop_rate(SDL_Window *win) {
  SDL_DisplayMode m; int hz = 0; if (!SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(win), &m)) hz = m.refresh_rate;
  if (hz < 24) hz = 60; dHz = hz; dK = (hz + 119) / 120; desktop_apply();
  fprintf(stderr, "[pace] monitor %d Hz: the app at %d frames a second, %d when idle\n", hz, hz / dK, hz / idle_k());
}
static Uint32 ptrMovedAt; static int curHid; /* the cursor hidden over a film that has the mouse still */
#endif
/* a phone: the finger being followed (others are ignored); the desktop preview's mouse plays the finger (SB_FORM=phone) */
static SDL_FingerID fingerId; static int fingerOn, mouseFinger;
int main(int argc, char **argv) {
  (void)argc; (void)argv;
  setlocale(LC_NUMERIC, "C");
#if SB_PREVIEW
  signal(SIGABRT, on_alarm); signal(SIGSEGV, on_alarm); signal(SIGBUS, on_alarm); /* a crash leaves a backtrace in the log */
  if (getenv("SB_ALARM")) { signal(SIGALRM, on_alarm); alarm(atoi(getenv("SB_ALARM"))); }
#else
  if (getenv("SB_PROF")) prof_start();
  { struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_crash; sa.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGABRT, &sa, NULL); sigaction(SIGFPE, &sa, NULL); }
#endif /* development: a stuck run stops itself (under a debugger: with a backtrace) */
  plat_pre_init(APP_ID);
#if SB_PREVIEW
  if (!getenv("SB_SHOW")) SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1"); /* the preview takes no Dock icon and no focus */
#endif
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
#if !SB_GL_ES
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#else
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
#endif
  { int cfg = getenv("SB_GLCFG") ? atoi(getenv("SB_GLCFG")) : 0; /* 0 RGBA8, 1 RGB8 (no alpha), 2 RGB565: which surface the compositor takes without a copy */
    if (cfg == 2) { SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5); SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 6); SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5); SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0); }
    else { SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8); SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8); SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8); SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, cfg == 1 ? 0 : 8); } }
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0); SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
  store_init(plat_data_dir()); /* before the window: the drawing size is a setting */
  int memMb = plat_mem_total_mb();
  int w, h; Uint32 flags; plat_window_size(&w, &h, &flags);
  { /* always the TV's own resolution (a saved "uiRes" from older versions is ignored); SB_RES=720 draws at 1280x720
       and lets the TV scale it up, for development only */
    const char *res = getenv("SB_RES") ? getenv("SB_RES") : "full";
    int light = !strcmp(res, "720");
#if SB_720_FULLSCREEN
    if (light && !getenv("SB_WIN")) { w = 1280; h = 720; flags |= SDL_WINDOW_FULLSCREEN; } /* measured on a 43LK5900: a fullscreen window gets a 1280x720 surface the TV scales to the panel */
#else
    if (light) { w = 1280; h = 720; flags &= ~SDL_WINDOW_ALLOW_HIGHDPI; } /* the preview shows the real 1280x720 drawing */
#endif
    fprintf(stderr, "[start] %d MB of RAM, drawing size %s%s\n", memMb, light ? "720p light" : "full", ""); }
  SDL_Window *win = SDL_CreateWindow("Origin", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);
#if SB_PREVIEW
  if (win && getenv("SB_SHOW")) { SDL_ShowWindow(win); SDL_RaiseWindow(win); } /* (started from a terminal it could stay behind, a Dock icon with no window in view) */
#endif
  if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
  void *ctx = GL_MAKE(win);
  if (!ctx) { fprintf(stderr, "context: %s\n", SDL_GetError()); return 1; }
#if !SB_TV_PACER
  GL_INTERVAL(0); /* the Mac swap blocks for ever on an occluded window with vsync; the loop paces itself */
#else
  vsyncOn = getenv("SB_VSYNC") ? atoi(getenv("SB_VSYNC")) : SB_VSYNC_DEFAULT;
  GL_INTERVAL(vsyncOn); /* measured on the TV: with vsync the driver blocks a whole extra frame at the first GL call; the loop paces itself instead */
  pace_init();
#if SB_WINDOWED
  if (vsyncOn) desktop_rate(win);
#endif
#endif
  plat_post_window(win);
#if SB_WINDOWED
  liveWin = win; plat_live_draw(live_frame);
  if (plat_text_input()) SDL_StartTextInput(); else SDL_StopTextInput(); /* a keyboard types anywhere: the screens decide what text means */
#else
  SDL_StopTextInput();
#endif
  int dw, dh; GL_SIZE(win, &dw, &dh);
  fprintf(stderr, "[start] %s window %dx%d drawable %dx%d\n", plat_device_name(), w, h, dw, dh);
  if (!gfx_init(dw, dh)) return 1;
  if (form_touch()) { /* a phone or a tablet: layout units are dp */
#if SB_PREVIEW
    form_set_density(w > 0 ? (float)dw / w * (getenv("SB_SHOW") ? 1.0f : (flags & SDL_WINDOW_ALLOW_HIGHDPI) ? 2.0f : 1.0f) : 1); /* (the hidden preview draws at twice the window) */
#endif
    gfx_touch_layout(form_density()); plat_orientation(0);
    fprintf(stderr, "[form] %s, %.2f px per dp: canvas %.0fx%.0f dp\n", form_tablet() ? "tablet" : "phone", form_density(), SB_W, SB_H); }
  video_set_surface(dw, dh); /* the TV reads the video plane's rectangle in the drawable's pixels (720p light: 1280x720) */
#if SB_PREVIEW
  if (SDL_GetWindowFlags(win) & SDL_WINDOW_HIDDEN) { /* a hidden window has no screen to draw to: draw into a picture the size the window would have */
    if (getenv("SB_SHOW") == NULL) { float k = (flags & SDL_WINDOW_ALLOW_HIGHDPI) ? 2.0f : 1.0f; dw = (int)(w * k); dh = (int)(h * k); gfx_resize(dw, dh); }
    static GfxTarget screen; if (gfx_target_create(&screen, dw, dh)) gfx_set_screen_fbo(screen.fbo); else fprintf(stderr, "[start] no offscreen picture: the window stays hidden and blank\n"); }
#endif
  /* data + the services */
  char p[1100]; snprintf(p, sizeof p, "%s/cacert.pem", plat_asset_dir()); net_init(p);
  cache_prune_start(SB_CACHE_MB);
  snprintf(p, sizeof p, "%s/cache", store_data_dir()); img_init(p); if (form_touch()) img_set_draw_scale_up(form_density()); else img_set_draw_scale(gfx_scale());
  if (memMb > 0) img_set_budget_mb(memMb < 1200 ? 40 : memMb < 3000 ? 128 : 192); /* pictures under the RAM the TV has (the shared atlas, backdrop and text add ~20 MB) */
  i18n_init(plat_asset_dir(), getenv("SB_LANG") ? getenv("SB_LANG") : store_get("uiLang", "auto")); /* SB_LANG=ar: development */
  if (!strcmp(store_get("netLogs", "false"), "true")) httpd_net_logs(1); /* "Logs over the network": the server is up from the start */
  if (!text_init(plat_asset_dir(), gfx_scale())) return 1;
  text_set_rtl(i18n_rtl()); icon_init(plat_asset_dir()); sb_init();
  app_init(); player_init();
  app_register(SCR_LOGIN, &loginScreen); app_register(SCR_HOME, &homeScreen); app_register(SCR_DISC, &discScreen); app_register(SCR_LIB, &libScreen);
  app_register(SCR_DETAIL, &detailScreen); app_register(SCR_SIM, &simScreen); app_register(SCR_CAT, &catScreen); app_register(SCR_COL, &colScreen); app_register(SCR_SEARCH, &searchScreen); app_register(SCR_SET, &setScreen); app_card_menu_hook = cwmenu_show;
  if (form_touch()) phone_register(); /* a phone or a tablet: its own pages over the TV's */
  acct_restore_kept();
  { const char *a = getenv("SB_ACCT_ACTION"); if (a) { if (!strcmp(a, "switch")) acct_switch(); else if (!strcmp(a, "add")) acct_begin_add(); else if (!strcmp(a, "logout")) { sb_logout(); acct_restore_kept(); } } } /* development: tests/nuvio */
  if (sb_signed_in() || getenv("SB_SKIP_LOGIN")) { app_go(SCR_HOME, NULL, 1); splash_start(1, 0); } /* SB_SKIP_LOGIN: the default rows without an account (development) */
  else { app_splash_done = 1; app_go(SCR_LOGIN, NULL, 1); }
  profiles_at_start(); /* a Nuvio account: "Who's watching?", with Home built behind it for the last profile */

  const char *shot = getenv("SB_SHOT"); int shotAt = shot ? atoi(getenv("SB_SHOT_FRAME") ? getenv("SB_SHOT_FRAME") : "40") : -1;
  const char *script = getenv("SB_SCRIPT");
  Uint32 last = SDL_GetTicks(), fpsT = last; int frames = 0, fps = 0, frame = 0;
  fprintf(stderr, "[loop] start, signed in: %d\n", sb_signed_in());
  stress_init();
#if !SB_TV_PACER
  if (stress_on()) pace_init();
#endif
  while (!app_quitting()) {
    SDL_Event e; Uint32 tEv0 = SDL_GetTicks();
    while (SDL_PollEvent(&e)) {
      if (getenv("SB_EVLOG") || frame < 120) if (e.type == SDL_QUIT || e.type == SDL_KEYDOWN || e.type == SDL_KEYUP || e.type == SDL_WINDOWEVENT) fprintf(stderr, "[ev] type=%d%s sc=%d sym=%d win=%d\n", e.type, e.type == SDL_QUIT ? " QUIT" : "", e.type == SDL_KEYDOWN || e.type == SDL_KEYUP ? e.key.keysym.scancode : 0, e.type == SDL_KEYDOWN || e.type == SDL_KEYUP ? e.key.keysym.sym : 0, e.type == SDL_WINDOWEVENT ? e.window.event : 0);
      if (e.type == SDL_QUIT) { fprintf(stderr, "[exit] SDL_QUIT\n"); app_quit(); }
      else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
#if SB_WINDOWED
        if (e.key.keysym.sym == SDLK_APPLICATION || (e.key.keysym.sym == SDLK_F10 && (e.key.keysym.mod & KMOD_SHIFT))) { if (e.type == SDL_KEYDOWN && !e.key.repeat) app_menu_key(); continue; } /* the menu key, Shift+F10: the card's menu */
#endif
        int k = plat_map_key(&e.key);
#if !SB_WINDOWED
        if (k == K_BACK && e.key.keysym.sym == SDLK_ESCAPE && e.type == SDL_KEYDOWN && SCR == SCR_LOGIN) app_quit(); /* (a desktop window has its own close button) */
#endif
        if (k) app_key(k, e.type == SDL_KEYDOWN, e.key.repeat); }
      else if (e.type == SDL_TEXTINPUT) app_text(e.text.text);
      else if (form_touch() && (e.type == SDL_APP_WILLENTERBACKGROUND || e.type == SDL_APP_DIDENTERBACKGROUND)) phone_peek_shut_now(); /* a phone sent away: the card and its preview are gone when it comes back */
      else if (form_touch() && (e.type == SDL_FINGERDOWN || e.type == SDL_FINGERMOTION || e.type == SDL_FINGERUP)) { /* a phone's finger, in window coordinates */
        if (script) continue; int ww, wh; SDL_GetWindowSize(win, &ww, &wh); float x, y; to_layout(win, (int)(e.tfinger.x * ww), (int)(e.tfinger.y * wh), &x, &y);
        if (e.type == SDL_FINGERDOWN) { if (fingerOn) continue; fingerOn = 1; fingerId = e.tfinger.fingerId; touch_down(x, y); }
        else if (!fingerOn || e.tfinger.fingerId != fingerId) continue;
        else if (e.type == SDL_FINGERMOTION) touch_move(x, y);
        else { fingerOn = 0; touch_up(x, y); } }
      else if (form_touch() && !script && (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP || e.type == SDL_MOUSEMOTION) && (e.type == SDL_MOUSEMOTION ? e.motion.which : e.button.which) != SDL_TOUCH_MOUSEID) { /* the preview's mouse as a finger */
        float x, y; if (e.type == SDL_MOUSEMOTION) to_layout(win, e.motion.x, e.motion.y, &x, &y); else to_layout(win, e.button.x, e.button.y, &x, &y);
        if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) { mouseFinger = 1; touch_down(x, y); }
        else if (e.type == SDL_MOUSEMOTION && mouseFinger) touch_move(x, y);
        else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT && mouseFinger) { mouseFinger = 0; touch_up(x, y); }
        else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT) { app_key(K_BACK, 1, 0); app_key(K_BACK, 0, 0); } }
      else if (script && (e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP || e.type == SDL_MOUSEWHEEL)) {} /* a scripted run: the real mouse stays out */
      else if (e.type == SDL_MOUSEMOTION) { if (e.motion.x || e.motion.y || e.motion.xrel || e.motion.yrel) { /* (a real move to the corner counts) */ float x, y; to_layout(win, e.motion.x, e.motion.y, &x, &y); app_pointer_move(x, y); }
#if SB_WINDOWED
        ptrMovedAt = SDL_GetTicks(); if (curHid) { curHid = 0; SDL_ShowCursor(SDL_ENABLE); }
#endif
      }
      else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) { float x, y; to_layout(win, e.button.x, e.button.y, &x, &y);
#if SB_WINDOWED
        static int eatUp, pausedBefore; /* a double click on a film: fullscreen and back; its second click is not a click, and what the first did to play/pause is undone */
        if (e.button.button == SDL_BUTTON_LEFT && e.type == SDL_MOUSEBUTTONDOWN && e.button.clicks == 1) pausedBefore = video_state() == VID_PAUSED;
        if (e.button.button == SDL_BUTTON_LEFT && e.type == SDL_MOUSEBUTTONDOWN && e.button.clicks == 2 && player_open()) {
          if ((video_state() == VID_PAUSED) != pausedBefore) { app_key(K_PLAYPAUSE, 1, 0); app_key(K_PLAYPAUSE, 0, 0); }
          plat_fullscreen_toggle(); eatUp = 1; continue; }
        if (e.button.button == SDL_BUTTON_LEFT && e.type == SDL_MOUSEBUTTONUP && eatUp) { eatUp = 0; continue; }
        if (e.button.button == SDL_BUTTON_RIGHT && e.type == SDL_MOUSEBUTTONDOWN && app_pointer_menu(x, y)) continue; /* right button: the card's menu there ... */
        if ((e.button.button == SDL_BUTTON_RIGHT || e.button.button == SDL_BUTTON_X1) && e.type == SDL_MOUSEBUTTONDOWN) { app_key(K_BACK, 1, 0); app_key(K_BACK, 0, 0); } /* ... else Back, as the mouse's back button */
#endif
        if (e.button.button == SDL_BUTTON_LEFT) app_pointer_button(e.type == SDL_MOUSEBUTTONDOWN, x, y);
      }
      else if (e.type == SDL_MOUSEWHEEL) {
#if SB_WINDOWED
        float wx = e.wheel.preciseX, wy = e.wheel.preciseY; if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) { wx = -wx; wy = -wy; }
        if ((SDL_GetModState() & KMOD_SHIFT) && !wx) { wx = -wy; wy = 0; } /* Shift+wheel: sideways (down = right) */
        app_pointer_wheel(wx, wy);
#else
        app_pointer_wheel((float)e.wheel.x, (float)e.wheel.y);
#endif
      }
      else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) { GL_SIZE(win, &dw, &dh); gfx_resize(dw, dh); form_safe_refresh(); if (form_touch()) { touch_cancel(); fingerOn = 0; fprintf(stderr, "[form] surface %dx%d: canvas %.0fx%.0f\n", dw, dh, SB_W, SB_H); } }
#if SB_WINDOWED
      else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED && vsyncOn) desktop_rate(win);
#endif
    }
#if SB_WINDOWED
    if (plat_text_input() && !SDL_IsTextInputActive()) SDL_StartTextInput(); /* (the sign-in page stops it when a field closes) */
    if (!curHid && player_open() && SDL_GetTicks() - ptrMovedAt > 2500) { curHid = 1; SDL_ShowCursor(SDL_DISABLE); app_pointer_system(0); fprintf(stderr, "[window] cursor hidden over the film\n"); } /* a film: the cursor goes after 2.5 s still */
    else if (curHid && !player_open()) { curHid = 0; SDL_ShowCursor(SDL_ENABLE); }
    if (vsyncOn) { static int off = -1; if (off < 0) off = getenv("SB_IDLE_OFF") != NULL; /* (development: the old full rate, for comparisons) */
      int idle = !off && app_idle() > 2 && !player_open(); if (idle != dIdle) { dIdle = idle; desktop_apply(); } }
#endif
    Uint32 now = SDL_GetTicks(); float dt = (now - last) / 1000.0f; last = now;
    { float pp = (float)pace_period() / 1000.0f; if (pp <= 0) pp = 1 / 60.0f; /* motion on the 60 Hz clock: an on-time frame is one period (no millisecond jitter); a slow one moves it 3 periods at most (no jump) */
      if (dt > pp * 0.5f && dt < pp * 1.5f) dt = pp; else if (dt > pp * 3) dt = pp * 3; }
    static int profSlow = -1; if (profSlow < 0) profSlow = getenv("SB_PROF") && !strcmp(getenv("SB_PROF"), "slow"); int profMark = profN; /* SB_PROF=slow: only frames over 30 ms keep their samples */
    Uint32 tEv = SDL_GetTicks() - tEv0;
    Uint32 tP0 = SDL_GetTicks(); net_pump(); Uint32 tNet = SDL_GetTicks() - tP0; store_pump(); p2p_pump(); img_frame(); Uint32 tI0 = SDL_GetTicks(); img_pump(); Uint32 tImg = SDL_GetTicks() - tI0; static double accPre; double accPreFrame = SDL_GetTicks() - tP0 + tEv; accPre += accPreFrame;
    run_script(script, frame); stress_tick();
    { static int crashAt = -2; if (crashAt == -2) crashAt = getenv("SB_CRASH_AT") ? atoi(getenv("SB_CRASH_AT")) : -1; /* development: a crash at that frame, to test its report */
      if (crashAt >= 0 && frame == crashAt) crash_deep(3);
      static int bundleAt = -2; if (bundleAt == -2) bundleAt = getenv("SB_BUNDLE_OUT") ? 400 : -1; /* development: what Share logs would send, written to a file */
      { static int shareAt = -2, said; if (shareAt == -2) shareAt = getenv("SB_SHARE_AT") ? atoi(getenv("SB_SHARE_AT")) : -1; /* development: Share logs at that frame, its code in the log */
        if (shareAt >= 0 && frame == shareAt) applog_share_start();
        if (shareAt >= 0 && frame > shareAt && !said) { char o[64]; int st = applog_share_state(o, sizeof o); if (st != 0) { said = 1; fprintf(stderr, "[dev] share %s: %s\n", st > 0 ? "code" : "failed", o); } } }
      if (frame == bundleAt) { extern char *applog_bundle(void); char *bb = applog_bundle(); FILE *bf = fopen(getenv("SB_BUNDLE_OUT"), "w"); if (bf && bb) fputs(bb, bf); if (bf) fclose(bf); free(bb); } }
    p2p_test_tick(frame); /* development: SB_P2PTEST / SB_P2PCYCLES */
    if (frame == 90 && getenv("SB_PLAY_FROM")) play_from_addon(); /* development: the first stream an addon gives, as its row would play it */
    if (frame == 90 && getenv("SB_OPEN")) app_open_external(getenv("SB_OPEN"), ""); /* development: a title's page, SB_OPEN=stremio:///detail/movie/tt1228705 */
    if (frame == 90 && getenv("SB_PLAY")) { Stream st; memset(&st, 0, sizeof st); snprintf(st.url, sizeof st.url, "%s", getenv("SB_PLAY")); snprintf(st.name, sizeof st.name, "Dev\n1080p"); snprintf(st.title, sizeof st.title, "%s", getenv("SB_PLAY_TITLE") ? getenv("SB_PLAY_TITLE") : "Development stream HDR10 DDP 5.1"); snprintf(st.addon, sizeof st.addon, "Torrentio"); app_play(&st, getenv("SB_PLAY_TYPE") ? getenv("SB_PLAY_TYPE") : "movie", getenv("SB_PLAY_ID") ? getenv("SB_PLAY_ID") : "tt27369017", getenv("SB_PLAY_VID") ? getenv("SB_PLAY_VID") : "", 0); } /* development: play a stream straight away */
    /* a phone at rest (no finger, nothing scrolling or loading, no film, nothing pressed for 1.5 s): the screen keeps its
       last frame and the loop waits for input, drawing twice a second for what arrives by itself (a row, a picture
       asked for by that draw brings the full rate back until it is in). A TV and a scripted run draw every frame */
    if (form_touch() && !script && !shot) { static Uint32 lastDraw; int q, w2; img_queue_stats(&q, &w2); BgtState bs = bgt_state();
      int rest = !touch_busy() && !player_open() && app_splash_done && app_idle() > 1.5f && !q && !w2 && (bs == BGT_OFF || bs == BGT_GONE);
      if (rest && SDL_GetTicks() - lastDraw < 500) { SDL_WaitEventTimeout(NULL, 40); continue; }
      lastDraw = SDL_GetTicks(); }
    Uint32 tCpu0 = SDL_GetTicks();
    text_frame_begin();
    app_tick(dt); splash_tick(dt);
#if SB_WINDOWED
    if (SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED) { SDL_WaitEventTimeout(NULL, 50); continue; } /* minimised: nothing drawn; the loop wakes for events, or every 50 ms for the network and timers */
#endif
    Uint32 tA = SDL_GetTicks();
    gfx_begin(player_live() ? RGBA(0, 0, 0, 0) : PAGE); /* the video plane shows through the transparent window */
    { static int bench = -2, benchCalls = 0; if (bench == -2) { bench = getenv("SB_BENCH") ? atoi(getenv("SB_BENCH")) : -1; benchCalls = getenv("SB_BENCHCALLS") ? atoi(getenv("SB_BENCHCALLS")) : 0; }
      static GLuint bt[64]; static int btN = -1; if (btN < 0) { btN = getenv("SB_BENCHTEX") ? atoi(getenv("SB_BENCHTEX")) : 0; if (btN > 64) btN = 64; int ch = getenv("SB_BENCHTEX_ALPHA") ? 1 : 4, tw = getenv("SB_BENCHTEX_W") ? atoi(getenv("SB_BENCHTEX_W")) : 300, th = getenv("SB_BENCHTEX_H") ? atoi(getenv("SB_BENCHTEX_H")) : 40; int lin = getenv("SB_BENCHTEX_NEAREST") ? 0 : 1;
        unsigned char *px = malloc((size_t)tw * th * ch); for (int i = 0; i < btN; i++) { memset(px, 40 + i * 5, (size_t)tw * th * ch); bt[i] = gfx_tex_create(px, tw, th, ch, lin); } free(px); } /* SB_BENCHTEX=N: N distinct textures bound per frame */
      if (bench >= 0) { for (int i = 0; i < btN; i++) { if (getenv("SB_BENCHTEX_ALPHA")) gfx_alpha_tex(bt[i], 40 + (i % 8) * 230, 100 + (i / 8) * 60, 200, 40, WHITE_A(255)); else gfx_image(bt[i], 40 + (i % 8) * 230, 100 + (i / 8) * 60, 200, 40, 0, WHITE_A(255), 0, 0, 1, 1); }
        for (int i = 0; i < bench; i++) gfx_rrect(40 + (i % 40) * 46, 40 + (i / 40) * 46, 40, 40, 8, RGB(60 + i % 100, 80, 160)); /* one call, many small quads */
        for (int i = 0; i < benchCalls; i++) { gfx_rrect(40 + (i % 40) * 46, 600 + (i / 40) * 46, 40, 40, 8, RGB(160, 80, 60)); gfx_flush(); } /* many calls */
        { extern void bench_uniform(int, int, int); static int nu = -1; if (nu < 0) nu = getenv("SB_BENCHUNI") ? atoi(getenv("SB_BENCHUNI")) : 0; if (nu) { int dw, dh; GL_SIZE(win, &dw, &dh); bench_uniform(nu, dw, dh); } }
        if (getenv("SB_BENCHBIG")) for (int i = 0; i < atoi(getenv("SB_BENCHBIG")); i++) { gfx_rect(0, 0, SB_W, SB_H, RGBA(20, 20, 40, 120)); gfx_flush(); } } /* full-screen layers */
      else { app_draw(); splash_draw(); app_clock_top(); castlink_draw(); nerds_draw(); } }
    Uint32 tB = SDL_GetTicks(); static double accTick, accDraw, accEnd, accPump; accTick += tA - tCpu0; accDraw += tB - tA;
    if (getenv("SB_STATS")) { char fs[96]; snprintf(fs, sizeof fs, "%d fps  %d calls  %d quads  %d MB", fps, gfx_draw_calls(), gfx_quads(), img_used_mb()); text_draw(fs, 1870, 1030, 22, 500, WHITE_A(120), 0, 0, ALIGN_RIGHT); }
    gfx_end();
    Uint32 tC = SDL_GetTicks(); accEnd += tC - tB;
    img_release_unused(0);
    Uint32 tCpu1 = SDL_GetTicks(); accPump += tCpu1 - tC;
    static int glFinishDev = -1; if (glFinishDev < 0) glFinishDev = getenv("SB_GLFINISH") != NULL; /* development: the GPU's own time shows as "gpu" */
    Uint64 tG0 = SDL_GetPerformanceCounter(); if (glFinishDev) glFinish(); Uint64 tG1 = SDL_GetPerformanceCounter();
    GL_SWAP(win); Uint32 tSwap = SDL_GetTicks();
    { /* the rate the screen really takes frames at: a webOS 4 TV takes 48 a second, not its panel's 60 (paced at 60, a
         fifth of the frames were never shown and all motion ran at four fifths of its speed). Measured once, at the first
         quiet moment (nothing pressed for 3 s, after the start): 60 frames with the driver waiting for the screen, their
         median; then the loop paces itself again, at that rate */
#if SB_TV_PACER
      static int hzState = -1, hzTries, baseHz = 60, curHz = 60, follow; static Uint64 hzLast; static float hzIv[60]; static int hzN; static Uint32 hzNext;
      if (hzState < 0) { hzState = !vsyncOn && !getenv("SB_HZ") && !getenv("SB_PACE") ? 0 : 3; follow = hzState == 0; }
      if (hzState == 0 && frame > 300 && SDL_GetTicks() >= hzNext && app_idle() > 3 && !player_open() && video_state() == VID_IDLE) { int q, w; img_queue_stats(&q, &w); if (q || w) goto hzDone; /* (pictures still coming: not quiet) */ hzState = 1; hzLast = 0; hzN = 0; GL_INTERVAL(1); }
      else if (hzState == 1) { Uint64 c = SDL_GetPerformanceCounter(); if (hzLast) hzIv[hzN++] = (float)((double)(c - hzLast) * 1000.0 / (double)SDL_GetPerformanceFrequency()); hzLast = c;
        if (app_idle() < 0.5f || video_state() != VID_IDLE) { hzState = 0; GL_INTERVAL(0); } /* a key or a video (a clip makes the TV run at 50): measured again at the next quiet moment */
        else if (hzN == 60) { hzState = 2; GL_INTERVAL(0);
          for (int i = 1; i < hzN; i++) { float x = hzIv[i]; int j = i - 1; while (j >= 0 && hzIv[j] > x) { hzIv[j + 1] = hzIv[j]; j--; } hzIv[j + 1] = x; }
          float med = hzIv[hzN / 4], best = 1e9f; /* (the quicker quarter: a frame that waits on the app is not the screen) */ static const int R[] = { 48, 50, 60 }; int hz = 0;
          for (int i = 0; i < 3; i++) { float d = fabsf(1000.0f / R[i] - med); if (d < best && d < 0.6f) { best = d; hz = R[i]; } }
          fprintf(stderr, "[pace] frames reach the screen every %.1f ms (%.1f to %.1f)\n", med, hzIv[hzN / 10], hzIv[hzN - 1 - hzN / 10]); if (hz) { pace_set_hz(hz); baseHz = curHz = hz; } else if (++hzTries < 30) { hzState = 0; hzNext = SDL_GetTicks() + 20000; } } } /* (no rate matched: something was still busy; again in 10 s) */
      hzDone:;
      /* a 25 or 50 fps clip makes a 60 Hz TV run its screen at 50: the app follows it while the clip is loaded (the
         other rates leave the screen at 60); one comparison a frame, pace only hears of a change */
      if (follow) { float vf = video_frame_rate(); int want = baseHz == 60 && ((vf > 24.9f && vf < 25.1f) || (vf > 49.9f && vf < 50.1f)) ? 50 : baseHz;
        if (want != curHz) { curHz = want; pace_set_hz(want); if (want != baseHz) fprintf(stderr, "[pace] a %.0f fps clip is playing: the app at %d\n", vf, want); else fprintf(stderr, "[pace] back at %d\n", want); } }
#endif
    }
    { static int first = 1, ms = 30; if (first) { first = 0; ms = getenv("SB_PROF_MS") ? atoi(getenv("SB_PROF_MS")) : 30; if (profSlow) profMark = 0; } /* start-up is not a frame */
      if (profSlow && (double)(tSwap - tCpu0) + accPreFrame < ms) profN = profMark; }
    { /* SB_SLOWLOG: every frame over the period says where its time went (at most 20 lines a second) */
      static int slowLog = -1; if (slowLog < 0) slowLog = getenv("SB_SLOWLOG") != NULL; int nerdLog = !slowLog && nerds_on(); /* (Stats for nerds on: only the frames a viewer would see stutter) */
      if (slowLog || nerdLog) { double hz = (double)SDL_GetPerformanceFrequency(), gpu = (tG1 - tG0) * 1000.0 / hz; double tot = (double)(tSwap - tCpu0) + accPreFrame;
        static Uint32 logT; static int logN; if (SDL_GetTicks() - logT > 1000) { logT = SDL_GetTicks(); logN = 0; }
        if (tot > (nerdLog ? 26.0 : 15.0) && logN++ < 20) fprintf(stderr, "[slow] f%d %s %.0f ms: pumps %.0f (events %u net %u pictures %u) tick %u draw %u end %u evict %u gpu %.1f swap %u | %d calls %d quads\n", frame, stress_screen(), tot, accPreFrame, tEv, tNet, tImg, tA - tCpu0, tB - tA, tC - tB, tCpu1 - tC, gpu, tSwap - tCpu1, gfx_draw_calls(), gfx_quads()); } }
    nerds_frame((float)(tCpu1 - tCpu0), gfx_draw_calls(), gfx_quads());
    static double accCpu, accSwap; static int accN; accCpu += tCpu1 - tCpu0; accSwap += tSwap - tCpu1; accN++;
#if !SB_TV_PACER
    if (stress_on()) pace_wait((double)(tSwap - tCpu0) + accPreFrame); else SDL_Delay(16); /* a stress run on the Mac checks the run itself (its numbers are not the TV's) */
#else
    { int pm = profN; if (!vsyncOn) pace_wait((double)(tSwap - tCpu0) + accPreFrame); else pace_observe(); if (profSlow) profN = pm; } /* the wait is not the frame's */
#endif
    frames++; frame++; if (frame == 1 || frame == 10 || frame % 300 == 0) { { int q, w; img_queue_stats(&q, &w); fprintf(stderr, "[frame] pictures: %d queued, %d to upload\n", q, w); } fprintf(stderr, "[frame] %d drawn, %d fps, %d MB of pictures, cpu %.1f ms (pumps %.1f tick %.1f draw %.1f flush %.1f evict %.1f), swap %.1f ms, %d calls %d quads\n", frame, fps, img_used_mb(), accN ? accCpu / accN : 0, accN ? accPre / accN : 0, accN ? accTick / accN : 0, accN ? accDraw / accN : 0, accN ? accEnd / accN : 0, accN ? accPump / accN : 0, accN ? accSwap / accN : 0, gfx_draw_calls(), gfx_quads()); { char pr[256]; pace_report(pr, sizeof pr); fprintf(stderr, "[frame] %s\n", pr); } if (getenv("SB_MIDLOG")) { extern int gfx_mid_switches(void); fprintf(stderr, "[mid] %d mid-frame target switches since the last line\n", gfx_mid_switches()); } if (getenv("SB_FILL")) { char fr[300]; gfx_fill_report(fr, sizeof fr); fprintf(stderr, "[fill] %s %s\n", stress_screen(), fr); } { double lm, wm; int r, sh, lk; text_stats(&lm, &wm, &r, &sh, &lk); fprintf(stderr, "[frame] gl flushes %.1f ms per frame; text: lines %.1f ms, widths %.1f ms, %d rasterised, %d shaped, %d lookups (per frame over %d)\n", accN ? gfx_flush_ms(1) / accN : 0, accN ? lm / accN : 0, accN ? wm / accN : 0, r, sh, lk, accN); } accCpu = accSwap = accPre = accTick = accDraw = accEnd = accPump = 0; accN = 0;
#if !SB_PREVIEW
      if (getenv("SB_PROF")) prof_report();
#endif
    }
    if (SDL_GetTicks() - fpsT >= 1000) { fps = frames; frames = 0; fpsT = SDL_GetTicks(); }
    { /* the memory guard, every 2 s: the TV ends an app that grows too big (it did at 340 MB of 641), so what can be made
         again goes before that: the pictures of the pages under a playing video, then more as the app or the TV gets tight */
      static Uint32 gAt; static int gLvl; if (SDL_GetTicks() - gAt >= 2000) { gAt = SDL_GetTicks(); int rss, swp; plat_mem_self(&rss, &swp); int fr = plat_mem_free_mb(), tot = plat_mem_total_mb();
        int lvl = 0; if (tot > 0 && rss > 0) { if (rss > tot * 30 / 100) lvl = 1; if (rss > tot * 40 / 100) lvl = 2; }
        if (fr >= 0 && fr < 60 && lvl < 1) lvl = 1; if (fr >= 0 && fr < 35) lvl = 2; /* (a 641 MB TV runs with 70-120 MB free on its own: only real shortage counts) */
        int film = player_open() && !player_page_under();
        if (lvl >= 2) { img_release_unused(4); gfx_pool_clear(); }
        else if (lvl == 1) { img_release_unused(img_budget_mb() / 2); gfx_pool_clear(); }
        else if (film) { img_release_unused(12); gfx_pool_clear(); } /* (back on the page, they come again as they are drawn) */
        if (lvl != gLvl) { fprintf(stderr, "[mem] guard %d -> %d: app %d MB, TV free %d MB of %d%s\n", gLvl, lvl, rss, fr, tot, film ? " (a film plays)" : ""); gLvl = lvl; } } }
    { /* every 30 s a line of where the memory is (the TV ends an app that takes too much: the shared log then says what grew) */
      static Uint32 memAt; if (SDL_GetTicks() - memAt >= 30000 || !memAt) { memAt = SDL_GetTicks(); int rss, swp; plat_mem_self(&rss, &swp); long tb, pb; int tn; gfx_mem(&tb, &tn, &pb);
        static const char *SN[] = { "sign-in", "home", "discover", "library", "title", "similar", "search", "settings" };
        fprintf(stderr, "[mem] app %d MB (swap %d) · TV free %d MB · pictures %d of %d MB · textures %ld MB in %d (kept for reuse %ld MB) · %s%s%s\n", rss, swp, plat_mem_free_mb(), img_used_mb(), img_budget_mb(),
          tb >> 20, tn, pb >> 20, (unsigned)SCR < 8 ? SN[SCR] : "?", player_open() ? " · player" : "", asssubs_on() ? " · styled subtitles" : ""); } }
    if (shot && frame == shotAt) { screenshot(shot, dw, dh); if (getenv("SB_SHOT_EXIT")) app_quit(); }
  }
  fprintf(stderr, "[exit] after %d frames\n", frame);
  p2p_shutdown(); /* the engine's threads end and its buffer files go */
  store_flush();
  GL_FREE(ctx); SDL_DestroyWindow(win); SDL_Quit();
  return 0;
}
