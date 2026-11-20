/* Video on Samsung Tizen TVs (native package): the TV's own media player (capi-media-player), its picture on the
   hardware video plane under the app's window, as the TV's own toolkit plays video. The library is found at run time
   (dlopen), so the app still starts on a set without it. Its callbacks come on the player's threads: they only set
   flags; everything else runs on the app's thread in video_pump(). */
#include "core/video.h"
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <math.h>
#include <strings.h>
#include <SDL2/SDL.h>
#include <pthread.h>
#include "core/net.h"
#include "core/hproxy.h"

enum { PS_NONE = 0, PS_IDLE, PS_READY, PS_PLAYING, PS_PAUSED };
enum { ST_AUDIO = 1, ST_VIDEO = 2, ST_TEXT = 3 };
enum { DT_OVERLAY = 0 };
enum { DM_LETTER_BOX = 0, DM_FULL_SCREEN = 2, DM_DST_ROI = 5 };

typedef void (*cb0)(void *);
typedef void (*cbi)(int, void *);
static struct {
  int (*create)(void **); int (*destroy)(void *); int (*set_uri)(void *, const char *);
  int (*set_ecore_wl_display)(void *, int, void *, int, int, int, int); int (*set_display_mode)(void *, int); int (*set_display_roi_area)(void *, int, int, int, int);
  int (*prepare_async)(void *, cb0, void *); int (*unprepare)(void *); int (*start)(void *); int (*stop)(void *); int (*pause)(void *);
  int (*set_play_position)(void *, int, bool, cb0, void *); int (*get_play_position)(void *, int *); int (*get_duration)(void *, int *); int (*get_state)(void *, int *);
  int (*set_completed_cb)(void *, cb0, void *); int (*set_error_cb)(void *, cbi, void *); int (*set_buffering_cb)(void *, cbi, void *);
  int (*get_track_count)(void *, int, int *); int (*get_track_language_code)(void *, int, int, char **); int (*get_current_track)(void *, int, int *); int (*select_track)(void *, int, int);
  int (*set_volume)(void *, float, float); int (*get_video_size)(void *, int *, int *); int (*get_streaming_download_progress)(void *, int *, int *);
  int (*get_codec_info)(void *, char **, char **); int (*set_subtitle_silent)(void *, int);
  int (*set_video_roi)(void *, double, double, double, double);
} P;
static void *lib, *win, *pl;

static _Atomic VidState st = VID_IDLE;
static atomic_bool pauseFromHost;
static char curUrl[4096], err[160], timedBase[4096];
static double timedDur, timedFrom; /* a stream that can't seek: each seek loads <base>?t=<s>, times count from timedFrom */
static volatile int evPrepared, evCompleted, evError, evBuffering = -1, evSeekDone; /* (set by the player's threads) */
static int startWait, infoWait, wantPause, pendingSeekMs = -1, seeking, liveShown, isLiveSrc, vol = 100, setWin; static double seekTo, seekAt; /* (a Samsung does not always call back when a seek is done: its position says it) */
static double posSec, posAt, durSec, lastPollAt;
static int vW, vH; /* the frame's size, once prepared */
static int audioUnsup, viaApp; /* viaApp: this stream already goes through the app's own connection (hproxy) */
static float wx, wy, ww = 1920, wh = 1080;
extern void sbh_drawable(int *w, int *h); /* (the drawing surface: 1920x1080 on the sets seen, read rather than assumed) */
static void screen(int *w, int *h) { sbh_drawable(w, h); if (*w < 640 || *h < 360) { *w = 1920; *h = 1080; } }
/* (each call into the TV's player service timed: one that takes 30 ms or more is named in the log) */
#define TC(nm, e) ({ Uint32 tc0_ = SDL_GetTicks(); __typeof__(e) tcr_ = (e); Uint32 tcd_ = SDL_GetTicks() - tc0_; if (tcd_ >= 30) fprintf(stderr, "[video-tizen] slow call: %s %u ms%s\n", nm, (unsigned)tcd_, pthread_equal(pthread_self(), qThread) ? " (worker)" : ""); tcr_; })
static pthread_t qThread;
static double now_s(void) { return SDL_GetTicks() / 1000.0; }
static unsigned tGen = 1; /* (the stream and its track choice) */

/* Every call into the TV's player service after a stream is created goes to one worker thread, in the order asked:
   the service answers one call at a time and can hold one for seconds (a jump 2 s, the volume 1.35-4.9 s while a film
   prepares, closing a stream on a stuck server 21 s, a state asked behind them as long: all measured), which froze the
   drawing. The drawing thread reads what the worker last saw (position, state, buffering, details, tracks). A new
   stream is created on the drawing thread only when the worker has nothing left (the one before closed: one video
   decoder). A jump or a volume still waiting is replaced by the newer one; a start goes before a waiting volume. Its
   callbacks carry their stream's number: an old stream's late word is ignored. */
enum { Q_DROP = 1, Q_START, Q_PAUSE, Q_SEEK, Q_VOL, Q_TRACK, Q_SUBOFF, Q_SUBON, Q_PLACE, Q_INFO, Q_TRACKS };
typedef struct { int op; void *h; int i, j; } QCmd;
static QCmd q[64]; static int qn, qBusy, qStarted; static pthread_mutex_t qmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t qcv = PTHREAD_COND_INITIALIZER;
static volatile unsigned curGen = 1; static volatile int startFailGen; static volatile unsigned startedGen; /* (the stream the worker last started: playing only from then, as before the worker) */
static void *volatile polH; static volatile unsigned polGen; /* the stream the worker keeps an eye on (its position every 0.25 s) */
/* what the worker last read, for the drawing thread */
static pthread_mutex_t pmu = PTHREAD_MUTEX_INITIALIZER;
static struct { unsigned gen; int posMs, pstate, buf; double at; unsigned infoGen; double dur; int w, h; char ac[32], vc[32]; unsigned trGen; double trAt; int nAud, curAud; VidTrack aud[16]; } pub;
static void *q_run(void *a);
static void q_put_locked(int op, void *h, int i, int j) {
  if (!h)
    return;
  if (!qStarted) {
    int rc = pthread_create(&qThread, NULL, q_run, NULL);
    if (rc) {
      fprintf(stderr, "[video-tizen] cannot start the player worker (%d)\n", rc);
      return;
    }
    qStarted = 1;
    pthread_detach(qThread);
  }
  if (op == Q_SEEK || op == Q_VOL || op == Q_PLACE || op == Q_TRACKS) for (int k = 0; k < qn; k++) if (q[k].op == op && q[k].h == h) { q[k].i = i; q[k].j = j; return; } /* (still waiting: the newer one) */
  if (qn >= 64) { fprintf(stderr, "[video-tizen] the player's queue is full: %d dropped\n", op); return; }
  int at = qn; if (op == Q_START) for (int k = 0; k < qn; k++) if (q[k].op == Q_VOL && q[k].h == h) { at = k; break; } /* (the picture first, its sound a moment after) */
  memmove(q + at + 1, q + at, sizeof(QCmd) * (size_t)(qn - at)); q[at] = (QCmd){ op, h, i, j }; qn++;
  pthread_cond_signal(&qcv);
}
static void q_put(int op, void *h, int i, int j) {
  pthread_mutex_lock(&qmu);
  q_put_locked(op, h, i, j);
  pthread_mutex_unlock(&qmu);
}
static int q_idle(void) { pthread_mutex_lock(&qmu); int r = !qn && !qBusy; pthread_mutex_unlock(&qmu); return r; }

/* the stream's volume, reached over easeMs (a trailer's sound comes up softly, goes out softly); each new stream starts at 100 */
static double volFrom = 100, volTo = 100, volT0, volMs; static int volSent = -1;
static int startSilent;
static int prepared;
static void vol_apply(int v) { vol = v; if (pl && prepared && P.set_volume && v != volSent) { volSent = v; q_put(Q_VOL, pl, v, 0); } } /* (while it prepares: kept, sent once it starts; the service holds a volume until it is ready, seconds, and everything after it waited) */
void video_set_volume(int pct, int easeMs) { pct = pct < 0 ? 0 : pct > 100 ? 100 : pct; volFrom = vol; volTo = pct; volT0 = now_s(); volMs = easeMs > 0 ? easeMs : 0; if (!volMs) vol_apply(pct); }
static void vol_tick(void) { if (vol == (int)volTo) return; double k = volMs > 0 ? (now_s() - volT0) * 1000.0 / volMs : 1; if (k >= 1) k = 1; vol_apply((int)(volFrom + (volTo - volFrom) * k + 0.5)); }
#define MINE(u) ((unsigned)(uintptr_t)(u) == curGen) /* (a callback of the stream playing now, not of one being closed) */
static void on_prepared(void *u) { if (MINE(u)) evPrepared = 1; }
static void on_completed(void *u) { if (MINE(u)) evCompleted = 1; }
static void on_error(int code, void *u) { if (MINE(u)) evError = code ? code : -1; }
static void on_buffering(int pct, void *u) { if (MINE(u)) evBuffering = pct; }
static void on_seek(void *u) { if (MINE(u)) evSeekDone = 1; }

#define SYM(n) (*(void **)&P.n = dlsym(lib, "player_" #n))
static void *find_window(void) {
  if (sbh_native_window()) { fprintf(stderr, "[video-tizen] the host's window\n"); return sbh_native_window(); } /* (the host knows its window: no guessing) */
  void *(*f1)(unsigned) = (void *(*)(unsigned))dlsym(RTLD_DEFAULT, "ecore_wl_window_find");
  void *(*f2)(unsigned) = (void *(*)(unsigned))dlsym(RTLD_DEFAULT, "ecore_wl2_window_find");
  for (unsigned id = 0; id < 64; id++) { void *w = f1 ? f1(id) : NULL; if (!w && f2) w = f2(id); if (w) { fprintf(stderr, "[video-tizen] window %u (%s)\n", id, f1 && f1(id) ? "ecore_wl" : "ecore_wl2"); return w; } }
  return NULL;
}
int video_init(const char *appId) {
  (void)appId;
  const char *names[] = { "libcapi-media-player.so.0", "libcapi-media-player.so", NULL };
  for (int i = 0; names[i] && !lib; i++) lib = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL);
  if (!lib) { fprintf(stderr, "[video-tizen] no media player library: %s\n", dlerror()); return 0; }
  SYM(create); SYM(destroy); SYM(set_uri); SYM(set_ecore_wl_display); SYM(set_display_mode); SYM(set_display_roi_area); SYM(prepare_async); SYM(unprepare);
  SYM(start); SYM(stop); SYM(pause); SYM(set_play_position); SYM(get_play_position); SYM(get_duration); SYM(get_state); SYM(set_completed_cb); SYM(set_error_cb);
  SYM(set_buffering_cb); SYM(get_track_count); SYM(get_track_language_code); SYM(get_current_track); SYM(select_track); SYM(set_volume); SYM(get_video_size);
  SYM(get_streaming_download_progress); SYM(get_codec_info); SYM(set_subtitle_silent);
  *(void **)&P.set_video_roi = dlsym(lib, "player_set_video_roi_area"); if (!P.set_video_roi) SYM(set_video_roi); /* (the source crop: its Tizen name, an older one) */
  fprintf(stderr, "[video-tizen] player library in: create %d prepare %d wl-display %d crop %d\n", !!P.create, !!P.prepare_async, !!P.set_ecore_wl_display, !!P.set_video_roi);
  if (P.create && P.destroy && P.set_uri && P.prepare_async && P.start)
    return 1;
  lib = NULL;
  return 0;
}


/* Fill / Zoom: a part of the frame (source pixels) shown in a box of the screen; set by video_set_crop, kept until a
   plain window is asked for (each new stream gets it again) */
static int cropOn, csx, csy, csw, csh; static float cdx, cdy, cdw, cdh;
static void place_on(void *h) { /* (the worker) */
  if (!h) return; int sw, sh; screen(&sw, &sh);
  if (cropOn && vW > 0 && vH > 0 && P.set_video_roi && P.set_display_roi_area && P.set_display_mode) {
    TC("set_display_mode", P.set_display_mode(h, DM_DST_ROI)); TC("set_display_roi_area", P.set_display_roi_area(h, (int)cdx, (int)cdy, (int)cdw, (int)cdh));
    TC("set_video_roi", P.set_video_roi(h, (double)csx / vW, (double)csy / vH, (double)csw / vW, (double)csh / vH)); return; }
  if (P.set_video_roi) TC("set_video_roi", P.set_video_roi(h, 0, 0, 1, 1));
  int full = ww >= sw - 1 && wh >= sh - 1 && wx <= 0.5f && wy <= 0.5f;
  if (P.set_display_mode) TC("set_display_mode", P.set_display_mode(h, full ? DM_LETTER_BOX : DM_DST_ROI));
  if (!full && P.set_display_roi_area) TC("set_display_roi_area", P.set_display_roi_area(h, (int)wx, (int)wy, (int)ww, (int)wh));
}
static void tracks_read(void *h, unsigned gen) { /* (the worker) the audio tracks and the one playing */
  int n = 0; VidTrack t[16]; memset(t, 0, sizeof t);
  if (P.get_track_count && TC("get_track_count", P.get_track_count(h, ST_AUDIO, &n)) != 0) n = 0; if (n > 16) n = 16;
  for (int i = 0; i < n; i++) { t[i].idx = i; char *c = NULL; if (P.get_track_language_code && TC("get_track_language_code", P.get_track_language_code(h, ST_AUDIO, i, &c)) == 0 && c) { snprintf(t[i].lang, sizeof t[i].lang, "%s", c); free(c); } snprintf(t[i].name, sizeof t[i].name, "%s", t[i].lang[0] ? t[i].lang : "Track"); }
  int cur = 0; if (P.get_current_track) TC("get_current_track", P.get_current_track(h, ST_AUDIO, &cur));
  pthread_mutex_lock(&pmu); memcpy(pub.aud, t, sizeof t); pub.nAud = n; pub.curAud = cur; pub.trGen = gen; pub.trAt = now_s(); pthread_mutex_unlock(&pmu); }
static void *q_run(void *a) { (void)a;
  for (;;) {
    pthread_mutex_lock(&qmu);
    if (!qn) { if (polH) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 250000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; } pthread_cond_timedwait(&qcv, &qmu, &ts); } else pthread_cond_wait(&qcv, &qmu); }
    QCmd c = { 0, NULL, 0, 0 }; if (qn) { c = q[0]; memmove(q, q + 1, sizeof(QCmd) * (size_t)(--qn)); }
    void *ph = polH; unsigned pg = polGen; qBusy = 1; pthread_mutex_unlock(&qmu);
    void *h = c.h; int r;
    switch (c.op) {
      case 0: if (ph) { int ms = 0, s = 0, b0 = 0, b1 = 0, ok = P.get_play_position && TC("get_play_position", P.get_play_position(ph, &ms)) == 0; if (P.get_state) TC("get_state", P.get_state(ph, &s));
          if (P.get_streaming_download_progress && TC("get_streaming_download_progress", P.get_streaming_download_progress(ph, &b0, &b1)) != 0) b1 = -1;
          pthread_mutex_lock(&pmu); if (ok) pub.posMs = ms; else if (pub.gen != pg) pub.posMs = -1; pub.pstate = s; pub.buf = b1; pub.gen = pg; pub.at = now_s(); pthread_mutex_unlock(&pmu); } break; /* (the stream's position, state and download, every 0.25 s) */
      case Q_DROP: { int s = 0; if (P.get_state) TC("get_state", P.get_state(h, &s));
        if (P.stop && (s == PS_PLAYING || s == PS_PAUSED)) TC("stop", P.stop(h));
        if (s >= PS_READY && P.unprepare) TC("unprepare", P.unprepare(h));
        if (P.destroy) TC("destroy", P.destroy(h)); break; }
      case Q_START: r = TC("start", P.start(h)); if (r) { fprintf(stderr, "[video-tizen] the player would not start (%d)\n", r); startFailGen = c.i; } else startedGen = (unsigned)c.i; break;
      case Q_PAUSE: if (P.pause) TC("pause", P.pause(h)); break;
      case Q_SEEK: if (P.set_play_position) TC("set_play_position", P.set_play_position(h, c.i, false, on_seek, (void *)(uintptr_t)(unsigned)c.j)); break;
      case Q_VOL: if (P.set_volume) TC("set_volume", P.set_volume(h, c.i / 100.0f, c.i / 100.0f)); break;
      case Q_TRACK: if (P.select_track) TC("select_track", P.select_track(h, c.i, c.j)); break;
      case Q_SUBOFF: if (P.set_subtitle_silent) TC("set_subtitle_silent", P.set_subtitle_silent(h, 1)); break;
      case Q_SUBON: if (P.select_track) TC("select_track", P.select_track(h, ST_TEXT, c.i)); if (P.set_subtitle_silent) TC("set_subtitle_silent", P.set_subtitle_silent(h, 0)); break;
      case Q_PLACE: place_on(h); break;
      case Q_INFO: { int d = 0, w = 0, hh = 0; char *ac = NULL, *vc = NULL; if (P.get_duration) TC("get_duration", P.get_duration(h, &d)); if (P.get_video_size) TC("get_video_size", P.get_video_size(h, &w, &hh)); if (P.get_codec_info) TC("get_codec_info", P.get_codec_info(h, &ac, &vc));
        tracks_read(h, (unsigned)c.i);
        pthread_mutex_lock(&pmu); pub.dur = d > 0 ? d / 1000.0 : 0; pub.w = w; pub.h = hh; snprintf(pub.ac, sizeof pub.ac, "%s", ac ? ac : "?"); snprintf(pub.vc, sizeof pub.vc, "%s", vc ? vc : "?"); pub.infoGen = (unsigned)c.i; pthread_mutex_unlock(&pmu); free(ac); free(vc); break; }
      case Q_TRACKS: tracks_read(h, (unsigned)c.i); break; }
    pthread_mutex_lock(&qmu); qBusy = 0; pthread_mutex_unlock(&qmu); }
  return NULL; }
static void drop(void) { if (!pl) return; curGen++; pthread_mutex_lock(&qmu); polH = NULL; pthread_mutex_unlock(&qmu); q_put(Q_DROP, pl, 0, 0); pl = NULL; prepared = 0; infoWait = 0; } /* (closed by the worker; its callbacks from now on are ignored) */
/* The TV's player follows no redirect (a debrid link answers with one): the final address is found first, on a worker
   thread (the first byte asked for, every hop's Location read), then the player is given it */
static pthread_mutex_t rsMu = PTHREAD_MUTEX_INITIALIZER;
static int rsGen, rsDone;
static char rsOut[4096];
static int rsStart;
static int cancel_resolve(void) {
  pthread_mutex_lock(&rsMu);
  int gen = ++rsGen;
  rsDone = 0;
  pthread_mutex_unlock(&rsMu);
  return gen;
}
typedef struct { char cur[4096], last[4096], in[4096]; int status, gen; } RsHop; /* (each lookup its own: an older one may still be running) */
static void rs_hdr(const char *line, void *ud) {
  RsHop *h = ud; int code;
  if (sscanf(line, "HTTP/%*s %d", &code) == 1) { h->status = code; return; }
  if (strncasecmp(line, "Location:", 9)) return;
  const char *v = line + 9; while (*v == ' ') v++; char loc[4096]; snprintf(loc, sizeof loc, "%s", v); loc[strcspn(loc, "\r\n")] = 0; if (!loc[0]) return;
  char abs_[4096];
  if (!strncmp(loc, "http://", 7) || !strncmp(loc, "https://", 8)) snprintf(abs_, sizeof abs_, "%s", loc);
  else if (loc[0] == '/') { const char *q = strstr(h->cur, "://"); const char *e = q ? strchr(q + 3, '/') : NULL; int n = e ? (int)(e - h->cur) : (int)strlen(h->cur); snprintf(abs_, sizeof abs_, "%.*s%s", n, h->cur, loc); }
  else { char *sl = strrchr(h->cur, '/'); int n = sl ? (int)(sl - h->cur) + 1 : (int)strlen(h->cur); snprintf(abs_, sizeof abs_, "%.*s%s", n, h->cur, loc); }
  snprintf(h->last, sizeof h->last, "%s", abs_); snprintf(h->cur, sizeof h->cur, "%s", abs_);
}
static int rs_sink(const unsigned char *p, size_t n, void *ud) { (void)p; (void)n; (void)ud; return -1; } /* (the first bytes are enough) */
static void *rs_thread(void *a) {
  RsHop *h = a; Uint32 t0 = SDL_GetTicks(); int r = net_pipe_sync(h->in, "Range: bytes=0-0", 0, rs_hdr, rs_sink, h, 8);
  pthread_mutex_lock(&rsMu);
  if (h->gen == rsGen) {
    snprintf(rsOut, sizeof rsOut, "%s", h->last[0] ? h->last : h->in);
    fprintf(stderr, "[video-tizen] resolved in %u ms (%s, status %d)\n", (unsigned)(SDL_GetTicks() - t0), h->last[0] ? "redirected" : "direct", r);
    rsDone = 1;
  }
  pthread_mutex_unlock(&rsMu);
  free(h); return NULL;
}
static char deferUrl[4096]; static int deferMs, deferOn; /* (a stream waiting for the worker to close the one before) */
static void start_player(const char *u, int startMs);
static void open_url(const char *u, int startMs) {
  int gen = cancel_resolve();
  drop(); deferOn = 0; st = VID_LOADING; err[0] = 0; posSec = startMs > 0 ? startMs / 1000.0 : 0; durSec = 0; liveShown = 0; seeking = 0; pendingSeekMs = startMs > 0 ? startMs : -1; /* (the new stream from its open: while it waits for the one before to close, nothing of that one shows as its: its live picture made the player resume early, and the start wiped that jump) */
  if ((!strncmp(u, "http://", 7) || !strncmp(u, "https://", 8)) && !strstr(u, "://127.0.0.1") && !strstr(u, "://localhost")) {
    RsHop *h = calloc(1, sizeof *h);
    if (h) { h->gen = gen; rsStart = startMs; snprintf(h->in, sizeof h->in, "%s", u); snprintf(h->cur, sizeof h->cur, "%s", u);
      pthread_t t; if (pthread_create(&t, NULL, rs_thread, h) == 0) { pthread_detach(t); return; } free(h); }
  }
  start_player(u, startMs);
}
static void start_player(const char *u, int startMs) {
  drop(); if (!q_idle()) { snprintf(deferUrl, sizeof deferUrl, "%s", u); deferMs = startMs; deferOn = 1; st = VID_LOADING; err[0] = 0; posSec = startMs > 0 ? startMs / 1000.0 : 0; durSec = 0; return; } deferOn = 0;
  tGen++; startWait = 0; audioUnsup = 0; evPrepared = evCompleted = evError = evSeekDone = 0; evBuffering = -1; prepared = 0; infoWait = 0; seeking = 0; liveShown = 0; err[0] = 0; setWin = 0;
  posSec = pendingSeekMs > 0 ? pendingSeekMs / 1000.0 : startMs > 0 ? startMs / 1000.0 : 0; posAt = 0; durSec = 0; if (startMs > 0) pendingSeekMs = startMs; vW = vH = 0; /* (a jump asked before it started is kept) */
  if (!lib || !P.create) { st = VID_ERROR; snprintf(err, sizeof err, "no media player on this TV"); return; }
  /* (created here, the worker idle: nothing of ours ahead of these in the player service) */
  if (P.create(&pl) != 0 || !pl) { st = VID_ERROR; snprintf(err, sizeof err, "the player could not be created"); pl = NULL; return; }
  if (!win) win = find_window();
  TC("set_uri", P.set_uri(pl, u));
  void *g = (void *)(uintptr_t)curGen;
  if (P.set_completed_cb) TC("set_completed_cb", P.set_completed_cb(pl, on_completed, g));
  if (P.set_error_cb) TC("set_error_cb", P.set_error_cb(pl, on_error, g));
  if (P.set_buffering_cb) TC("set_buffering_cb", P.set_buffering_cb(pl, on_buffering, g));
  { int sw, sh; screen(&sw, &sh); if (P.set_ecore_wl_display && win) { int r = TC("set_ecore_wl_display", P.set_ecore_wl_display(pl, DT_OVERLAY, win, 0, 0, sw, sh)); fprintf(stderr, "[video-tizen] display on the window: %d\n", r); setWin = 1; } }
  place_on(pl); /* (its place and crop, before it prepares: the worker idle) */
  if (startSilent) { vol = 0; volFrom = volTo = 0; volMs = 0; } /* (a film held for its previews: its sound never starts before it does) */
  if (P.set_volume) TC("set_volume", P.set_volume(pl, vol / 100.0f, vol / 100.0f)); volSent = vol;
  int r = TC("prepare_async", P.prepare_async(pl, on_prepared, g));
  fprintf(stderr, "[video-tizen] prepare %s -> %d\n", u, r);
  st = r == 0 ? VID_LOADING : VID_ERROR; if (r) snprintf(err, sizeof err, "the player refused the stream (%d)", r);
}

void video_set_live(int on) { isLiveSrc = on; }
void video_live_rejoin(void) { if (curUrl[0]) open_url(curUrl, 0); }
static void load_at(const char *u, int startMs) {
  if (!u)
    return;
  viaApp = 0;
  volFrom = volTo = 100;
  vol = 100;
  volSent = -1;
  snprintf(curUrl, sizeof curUrl, "%s", u);
  timedBase[0] = 0;
  timedFrom = 0;
  wantPause = 0;
  open_url(curUrl, startMs);
}
void video_load(const char *u) { load_at(u, 0); }
int video_load_at(const char *u, double s) {
  if (!u)
    return 0;
  int seek = P.set_play_position && isfinite(s) && s > 0 && s <= INT32_MAX / 1000.0;
  load_at(u, seek ? (int)(s * 1000) : 0);
  return seek;
}
void video_load_timed(const char *base, double durSec_) {
  if (!base) return; viaApp = 0; volFrom = volTo = 100; vol = 100; volSent = -1; snprintf(timedBase, sizeof timedBase, "%s", base); timedDur = durSec_; timedFrom = 0; wantPause = 0;
  snprintf(curUrl, sizeof curUrl, "%s", base); open_url(curUrl, 0);
}
void video_unload(void) { cancel_resolve(); drop(); deferOn = 0; st = VID_IDLE; curUrl[0] = 0; liveShown = 0; }
void video_start_silent(int on) { startSilent = on; } /* the next loads start at volume 0 (previews first: pw_end brings it up) */
void video_play(void) { wantPause = 0; if (pl && prepared) { q_put(Q_START, pl, (int)curGen, 0); st = VID_PLAYING; } }
void video_pause(void) { wantPause = 1; startWait = 0; if (pl && prepared && P.pause) { q_put(Q_PAUSE, pl, 0, 0); st = VID_PAUSED; } }
void video_seek(double s) {
  if (s < 0) s = 0;
  if (timedBase[0]) { char u[4200]; snprintf(u, sizeof u, "%s%st=%.0f", timedBase, strchr(timedBase, '?') ? "&" : "?", s); timedFrom = s; snprintf(curUrl, sizeof curUrl, "%s", u); open_url(u, 0); posSec = 0; return; }
  posSec = s; posAt = now_s();
  if (!pl || !prepared) { pendingSeekMs = (int)(s * 1000); return; }
  if (P.set_play_position) { seeking = 1; seekTo = s; seekAt = now_s(); evSeekDone = 0; q_put(Q_SEEK, pl, (int)(s * 1000), (int)curGen); }
}
void video_seek_now(double s) { video_seek(s); }
double video_reported_time(void) {
  pthread_mutex_lock(&pmu);
  int ms = pl && pub.gen == curGen ? pub.posMs : -1;
  pthread_mutex_unlock(&pmu);
  return ms < 0 ? -1 : ms / 1000.0 + (timedBase[0] ? timedFrom : 0);
}
double video_time(void) { double t = posSec; if (st == VID_PLAYING && posAt > 0 && !seeking) { double d = now_s() - posAt; if (d > 0 && d < 1) t += d; } return t + (timedBase[0] ? timedFrom : 0); }
double video_duration(void) { return timedBase[0] ? timedDur : durSec; }
double video_buffered_end(void) { if (pl && prepared && durSec > 0) { pthread_mutex_lock(&pmu); int b = pub.gen == curGen ? pub.buf : -1; pthread_mutex_unlock(&pmu); if (b > 0) return durSec * b / 100.0; } return video_time(); }
VidState video_state(void) { return st; }
const char *video_error(void) { return err; }
int video_live(void) { return liveShown; }
int video_has_media(void) { return pl != NULL && prepared; }
int video_set_rate(float rate) { (void)rate; return 0; }
int video_rate_supported(void) { return 0; }
int video_settling(void) { return seeking; }
int video_width(void) { return vW; }
int video_height(void) { return vH; }
float video_frame_rate(void) { return 0; }
double video_fps(void) { return 0; }
int video_hdr(char *out, int cap) { if (cap > 0 && out) out[0] = 0; return 0; }

/* the audio tracks as the worker last read them (after the stream prepared, and again at most every 5 s when asked) */
static int curAudPick = -1; static unsigned curAudPickGen;
int video_audio_tracks(VidTrack *out, int max) {
  if (!pl || !prepared) return 0; pthread_mutex_lock(&pmu); int n = pub.trGen == curGen ? pub.nAud : 0; if (n > max) n = max; memcpy(out, pub.aud, (size_t)n * sizeof *out); int stale = pub.trGen != curGen || now_s() - pub.trAt > 5; pthread_mutex_unlock(&pmu);
  if (stale) q_put(Q_TRACKS, pl, (int)curGen, 0); return n; }
int video_audio_current(void) { if (!pl || !prepared) return 0; if (curAudPickGen == curGen && curAudPick >= 0) return curAudPick; pthread_mutex_lock(&pmu); int c = pub.trGen == curGen ? pub.curAud : 0; pthread_mutex_unlock(&pmu); return c; }
void video_select_audio(int idx) { if (pl && prepared && P.select_track) { q_put(Q_TRACK, pl, ST_AUDIO, idx); curAudPick = idx; curAudPickGen = curGen; q_put(Q_TRACKS, pl, (int)curGen, 0); } tGen++; } /* (the pick known at once: the player is told by the worker) */
int video_text_tracks(VidTrack *out, int max) { (void)out; (void)max; return 0; } /* (none the TV draws: its player hands a subtitle's text to a callback and draws nothing itself, so a track handed to it went blank; the app reads and draws the file's own) */
void video_select_text(int idx) { if (!pl || !prepared) return; q_put(idx < 0 ? Q_SUBOFF : Q_SUBON, pl, idx, 0); }
void video_set_window(float x, float y, float w, float h) {
  int sw, sh; screen(&sw, &sh); if (x < 0) x = 0; if (y < 0) y = 0; if (w > sw) w = (float)sw; if (h > sh) h = (float)sh;
  if (x == wx && y == wy && w == ww && h == wh && !cropOn) return; wx = x; wy = y; ww = w; wh = h; cropOn = 0; q_put(Q_PLACE, pl, 0, 0);
}
int video_partial_plane(void) { return P.set_display_roi_area != NULL; }
int video_set_crop(int sx, int sy, int sw, int sh, float dx, float dy, float dw, float dh) {
  if (!P.set_video_roi || !P.set_display_roi_area || sw <= 0 || sh <= 0) return 0; /* (no crop on this TV: the caller keeps the picture whole) */
  cropOn = 1; csx = sx; csy = sy; csw = sw; csh = sh; cdx = dx; cdy = dy; cdw = dw; cdh = dh; q_put(Q_PLACE, pl, 0, 0);
  fprintf(stderr, "[video-tizen] crop %d,%d %dx%d of %dx%d into %.0f,%.0f %.0fx%.0f\n", sx, sy, sw, sh, vW, vH, dx, dy, dw, dh); return 1;
}
void video_set_surface(int w, int h) { (void)w; (void)h; }
int video_audio_unsupported(void) { return audioUnsup; } /* (the TV refused the file's sound: its error 0x..2e) */
int video_edge_colors(uint32_t *out4) { (void)out4; return 0; }

void video_pump(void) {
  if (atomic_exchange(&pauseFromHost, false) && st != VID_IDLE)
    video_pause();
  vol_tick();
  char resolved[sizeof rsOut];
  pthread_mutex_lock(&rsMu);
  int ready = rsDone;
  if (ready) {
    snprintf(resolved, sizeof resolved, "%s", rsOut);
    rsDone = 0;
  }
  pthread_mutex_unlock(&rsMu);
  if (ready)
    start_player(resolved, rsStart);
  if (deferOn && q_idle()) { char u[4096]; snprintf(u, sizeof u, "%s", deferUrl); deferOn = 0; start_player(u, deferMs); } /* (the stream before is closed: this one starts) */
  if (!pl) return;
  if (startFailGen && (unsigned)startFailGen == curGen) { startFailGen = 0; st = VID_ERROR; snprintf(err, sizeof err, "the player would not start"); return; }
  if (evError) { int e = evError; evError = 0; unsigned c = (unsigned)e & 0xff;
    /* a secure stream the TV's player won't open (not a format it can't play: its own TLS and certificates are the
       TV's, years old; a newer certificate chain is refused): once more through the app's own connection, the same
       moment, served on this device */
    if (!viaApp && !timedBase[0] && !strncmp(curUrl, "https://", 8) && c != 0x2e && c != 0x2f && c != 0x24) { char u[4200];
      if (hproxy_url(curUrl, NULL, NULL, u, sizeof u)) { viaApp = 1; double at = posSec; fprintf(stderr, "[video-tizen] error 0x%x on a secure stream: once more through the app's own connection\n", (unsigned)e); snprintf(curUrl, sizeof curUrl, "%s", u); open_url(curUrl, at > 1 ? (int)(at * 1000) : 0); return; } }
    st = VID_ERROR;
    /* in the words the player reads on webOS ("200": the sound this TV can't decode, "203": a picture or a format it can't) */
    audioUnsup = c == 0x2e;
    if (c == 0x2e) snprintf(err, sizeof err, "200 audio codec not supported (0x%x)", (unsigned)e);
    else if (c == 0x2f || c == 0x24) snprintf(err, sizeof err, "203 video codec or format not supported (0x%x)", (unsigned)e);
    else snprintf(err, sizeof err, "the TV's player stopped (0x%x)", (unsigned)e);
    fprintf(stderr, "[video-tizen] error 0x%x: %s\n", (unsigned)e, err); return; }
  if (evPrepared) { evPrepared = 0; infoWait = 1; q_put(Q_INFO, pl, (int)curGen, 0); } /* (its details read by the worker) */
  if (infoWait) { pthread_mutex_lock(&pmu); int ok = pub.infoGen == curGen; double d = pub.dur; int w = pub.w, h = pub.h; char ac[32], vc[32]; snprintf(ac, sizeof ac, "%s", pub.ac); snprintf(vc, sizeof vc, "%s", pub.vc); pthread_mutex_unlock(&pmu);
    if (ok) { infoWait = 0; prepared = 1; tGen++; if (d > 0) durSec = d; vW = w; vH = h;
      fprintf(stderr, "[video-tizen] prepared: %dx%d, %.0f s, audio %s, video %s\n", vW, vH, durSec, ac, vc);
      if (cropOn) q_put(Q_PLACE, pl, 0, 0); /* (the crop needs the frame's size) */
      pthread_mutex_lock(&qmu); polH = pl; polGen = curGen; pthread_mutex_unlock(&qmu);
      if (pendingSeekMs > 0 && P.set_play_position) { seeking = 1; seekTo = pendingSeekMs / 1000.0; seekAt = now_s(); q_put(Q_SEEK, pl, pendingSeekMs, (int)curGen); } pendingSeekMs = -1;
      if (!wantPause) { q_put(Q_START, pl, (int)curGen, 0); st = VID_LOADING; startWait = 1; } else st = VID_PAUSED; /* (playing once the player has really started: previews first holds the film on that) */
      if (vol != volSent && P.set_volume) { volSent = vol; q_put(Q_VOL, pl, vol, 0); } /* (a volume asked while it prepared) */
      posAt = now_s(); } }
  if (startWait && startedGen == curGen) { startWait = 0; if (st == VID_LOADING) st = VID_PLAYING; }
  if (evSeekDone) { evSeekDone = 0; seeking = 0; posAt = now_s(); fprintf(stderr, "[video-tizen] seek done\n"); }
  if (evCompleted) { evCompleted = 0; st = VID_ENDED; return; }
  int b = evBuffering; static int lastB = -2; if (b != lastB && (b < 0 || b % 25 == 0 || b >= 100)) { fprintf(stderr, "[video-tizen] buffering %d%%\n", b); } lastB = b;
  int ps = -1, pms = 0; double pat = 0; { pthread_mutex_lock(&pmu); if (pub.gen == curGen) { ps = pub.pstate; pms = pub.posMs; pat = pub.at; } pthread_mutex_unlock(&pmu); }
  { static double lastLog; static VidState lastSt = VID_IDLE; double tn = now_s(); if (st != lastSt || (seeking && tn - lastLog > 5)) { fprintf(stderr, "[video-tizen] state %d (player %d)%s at %.1f s\n", st, ps, seeking ? ", seeking" : "", posSec); lastSt = st; lastLog = tn; } }
  if (prepared && st != VID_PAUSED && st != VID_ENDED && st != VID_ERROR) { if (b >= 0 && b < 100) st = VID_BUFFERING; else if (b >= 100 || st == VID_BUFFERING) { if (b >= 100) evBuffering = -1; st = VID_PLAYING; } }
  double t = now_s();
  if (prepared && pms >= 0 && pat > lastPollAt) { lastPollAt = pat; double s = pms / 1000.0; /* (the worker's last look: every 0.25 s) */
    if (seeking) { /* done when the position has reached where it was sent (its callback may never come), or after 10 s */
      if (fabs(s - seekTo) < 4 || t - seekAt > 10) { seeking = 0; fprintf(stderr, "[video-tizen] seek reached %.1f s\n", s); } else s = -1; }
    if (s >= 0) { int moved = s > posSec + 0.01;
      /* the player plays and the picture moves: playing, whatever its last buffering word was */
      if (moved && ps == PS_PLAYING && (st == VID_BUFFERING || st == VID_LOADING)) { st = VID_PLAYING; evBuffering = -1; }
      if (st == VID_PLAYING && moved) liveShown = 1; posSec = s; posAt = pat; } }
}

/* (from the host's thread, the app sent to the back: the player is paused by the worker at once, whatever the app's loop is doing) */
void video_pause_now(void) {
  atomic_store(&pauseFromHost, true);
  pthread_mutex_lock(&qmu);
  q_put_locked(Q_PAUSE, polH, 0, 0);
  pthread_mutex_unlock(&qmu);
}
