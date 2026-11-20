/* macOS: libmpv draws each new video frame into a texture through its render API, on the drawing thread, in the app's
   own GL context (VideoToolbox decoding, its pictures handed to GL without copies). At the end of every frame the
   texture goes under what the app drew (video_frame_under, called by gfx_end): the video shows where the UI is
   transparent, as on the TVs' video plane, Android's SurfaceView and Windows' composition layer, and the holes a card
   cuts for its trailer show it too. Its options follow the Windows player (video_win.c) where they apply.
   libmpv.dylib (Contents/Frameworks in the app, tools/macos/build-deps.sh) is loaded on a background thread 1.2 s after
   the start, sooner if a video is asked for, so the start never waits for it. Every call into mpv here is asynchronous
   except the drawing; what mpv knows arrives as events and observed properties, read once a frame in video_pump. */
#include "core/video.h"
#include "ui/gfx.h"
#include "platform/plat.h"
#include "platform/mem.h"
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <SDL2/SDL.h>
#include <CoreAudio/CoreAudio.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static struct {
  mpv_handle *(*create)(void);
  int (*initialize)(mpv_handle *);
  int (*set_option_string)(mpv_handle *, const char *, const char *);
  int (*command_async)(mpv_handle *, uint64_t, const char **);
  int (*set_property_async)(mpv_handle *, uint64_t, const char *, mpv_format, void *);
  int (*observe_property)(mpv_handle *, uint64_t, const char *, mpv_format);
  mpv_event *(*wait_event)(mpv_handle *, double);
  int (*request_log_messages)(mpv_handle *, const char *);
  const char *(*error_string)(int);
  unsigned long (*client_api_version)(void);
  int (*render_create)(mpv_render_context **, mpv_handle *, mpv_render_param *);
  void (*render_set_update_callback)(mpv_render_context *, mpv_render_update_fn, void *);
  uint64_t (*render_update)(mpv_render_context *);
  int (*render_render)(mpv_render_context *, mpv_render_param *);
} M;
static mpv_handle *H;
static mpv_render_context *RC;
static atomic_int loaderState; /* 0 waiting, 1 loading, 2 ready, -1 failed */
static atomic_int frameWaiting; /* mpv has a new frame to draw (its update callback, any thread) */
static pthread_mutex_t wakeM = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wakeC = PTHREAD_COND_INITIALIZER;
static int woken;
static int hooked;
static GfxTarget tgt; /* the picture mpv draws into: the video's rectangle on the screen, in pixels */
static IOPMAssertionID awakeId;

/* what the app sees, all on the drawing thread */
static struct {
  VidState st;
  int active, pendingStarts, started, loaded, live, paused, cache, eof, seeking, error;
  double t, dur, buf, fps, reported;
  int w, h, dv;
  char gamma[16], hwdec[24], err[200];
} S;
static VidTrack aT[24], tT[24];
static int64_t aId[24], tId[24];
static int nA, nT, curA = -1;
static char url[4200], pendingUrl[4200];
static double pendingAt;
static int pending;
static double seekTarget = -1;
static int seekSent;
static Uint32 loadAt;
static struct {
  int on;
  double off, dur;
  char base[4096];
} timed;
static struct {
  float x, y, w, h;
  int crop, sx, sy, sw, sh;
  int set;
} R;
static struct {
  double from, to, cur;
  Uint32 t0, ms;
  int on;
} V = { 100, 100, 100, 0, 0, 0 };
static int awake;

enum {
  P_TIME = 1,
  P_DUR,
  P_PAUSE,
  P_CACHE,
  P_EOF,
  P_SEEKING,
  P_BUF,
  P_W,
  P_H,
  P_FPS,
  P_GAMMA,
  P_DV,
  P_TRACKS,
  P_HWDEC,
  P_DROP,
  P_AO
};
enum { R_LOAD = 1, R_OTHER };

/* ---- the sound on a stereo device ----
   A 5.1 or 7.1 film on headphones or the Mac's own speakers is folded down to stereo, and mpv's plain fold-down adds
   the channels up: loud scenes go past full scale and clip, heard as crackle. Where the device plays stereo the
   fold-down is normalised so it never clips (this FFmpeg has no limiter filter for Windows' louder way). The device is
   asked at mpv's start and again on a thread at each video start (headphones plugged in, AirPods). */
static atomic_int mixCh; /* the default output device's channels; 0 unknown */
static int afOn = -1; /* the normalised fold-down as last set: -1 never */
static int device_channels(void) {
  AudioObjectPropertyAddress a = { kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                   kAudioObjectPropertyElementMain };
  AudioDeviceID dev = 0;
  UInt32 sz = sizeof dev;
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, &dev) || !dev)
    return 0;
  a.mSelector = kAudioDevicePropertyStreamConfiguration;
  a.mScope = kAudioDevicePropertyScopeOutput;
  if (AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &sz) || !sz)
    return 0;
  AudioBufferList *l = malloc(sz);
  int n = 0;
  if (l && !AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, l))
    for (UInt32 i = 0; i < l->mNumberBuffers; i++)
      n += (int)l->mBuffers[i].mNumberChannels;
  free(l);
  return n;
}
static void *ask_device(void *a) {
  (void)a;
  plat_thread_background(5);
  int n = device_channels();
  if (n > 0)
    atomic_store(&mixCh, n);
  return NULL;
}
static void set_str(const char *k, const char *v);
static void apply_fold_down(void) { /* (drawing thread) */
  int ch = atomic_load(&mixCh), want = ch == 2 && !getenv("SB_NO_FOLD");
  if (want == afOn || !ch)
    return;
  afOn = want;
  set_str("audio-normalize-downmix", want ? "yes" : "no");
  fprintf(stderr, "[video] sound device plays %d channels: %s\n", ch,
          want ? "surround folded to stereo without clipping" : "mpv's own mapping");
}

/* ---- starting mpv ---- */
static void opt(mpv_handle *h, const char *k, const char *v) {
  int r = M.set_option_string(h, k, v);
  if (r < 0)
    fprintf(stderr, "[video] mpv option %s=%s: %s\n", k, v, M.error_string(r));
}
static void *open_libmpv(void) {
  char p[1200];
  char *base = SDL_GetBasePath(); /* in the app: Contents/Resources/, the libraries in Contents/Frameworks */
  snprintf(p, sizeof p, "%s../Frameworks/libmpv.dylib", base ? base : "./");
  SDL_free(base);
  void *h = dlopen(p, RTLD_NOW | RTLD_LOCAL);
  if (!h) /* development: a build run from the terminal, Homebrew's mpv */
    h = dlopen(getenv("SB_LIBMPV") ? getenv("SB_LIBMPV") : "/opt/homebrew/lib/libmpv.2.dylib", RTLD_NOW | RTLD_LOCAL);
  if (!h)
    fprintf(stderr, "[video] libmpv: %s\n", dlerror());
  return h;
}
static void *loader(void *a) {
  (void)a;
  pthread_mutex_lock(&wakeM);
  if (!woken) { /* Home first; a video asked for sooner wakes this at once */
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_nsec += 200000000;
    t.tv_sec += 1 + t.tv_nsec / 1000000000;
    t.tv_nsec %= 1000000000;
    pthread_cond_timedwait(&wakeC, &wakeM, &t);
  }
  pthread_mutex_unlock(&wakeM);
  if (getenv("SB_NO_MPV")) { /* development: the app without its player, for comparisons */
    atomic_store(&loaderState, -1);
    return NULL;
  }
  atomic_store(&loaderState, 1);
  Uint32 t0 = SDL_GetTicks();
  void *lib = open_libmpv();
  if (!lib) {
    atomic_store(&loaderState, -1);
    return NULL;
  }
#define L(f, name)                                      \
  if (!(*(void **)&M.f = dlsym(lib, name))) {           \
    fprintf(stderr, "[video] libmpv lacks %s\n", name); \
    atomic_store(&loaderState, -1);                     \
    return NULL;                                        \
  }
  // clang-format off
  L(create, "mpv_create")
  L(initialize, "mpv_initialize")
  L(set_option_string, "mpv_set_option_string")
  L(command_async, "mpv_command_async")
  L(set_property_async, "mpv_set_property_async")
  L(observe_property, "mpv_observe_property")
  L(wait_event, "mpv_wait_event")
  L(request_log_messages, "mpv_request_log_messages")
  L(error_string, "mpv_error_string")
  L(client_api_version, "mpv_client_api_version")
  L(render_create, "mpv_render_context_create")
  L(render_set_update_callback, "mpv_render_context_set_update_callback")
  L(render_update, "mpv_render_context_update")
  L(render_render, "mpv_render_context_render")
  // clang-format on
#undef L
  if ((M.client_api_version() >> 16) != 2) {
    fprintf(stderr, "[video] libmpv API %lu.%lu, 2.x needed\n", M.client_api_version() >> 16,
            M.client_api_version() & 0xffff);
    atomic_store(&loaderState, -1);
    return NULL;
  }
  Uint32 t1 = SDL_GetTicks();
  mpv_handle *h = M.create();
  if (!h) {
    atomic_store(&loaderState, -1);
    return NULL;
  }
  char cache[1200];
  snprintf(cache, sizeof cache, "%s/mpv-shaders", plat_data_dir());
  mkdir(cache, 0755);
  /* nothing of mpv's own: no config files, scripts, keys, on-screen controls or window (the app does all that) */
  opt(h, "config", "no");
  opt(h, "terminal", "no"); /* (this libmpv has no scripts, youtube-dl or on-screen controller to turn off) */
  opt(h, "osd-level", "0");
  opt(h, "input-default-bindings", "no");
  opt(h, "input-vo-keyboard", "no");
  opt(h, "input-media-keys", "no");
  /* one core for the app's whole life: no start-up per video */
  opt(h, "idle", "yes");
  opt(h, "keep-open", "yes");
  opt(h, "audio-display", "no");
  opt(h, "vo", "libmpv"); /* frames go to the render context made on the drawing thread (ready) */
  opt(h, "hwdec", "auto-safe"); /* (VideoToolbox) */
  opt(h, "gpu-shader-cache-dir", cache);
  opt(h, "tone-mapping", "bt.2390"); /* HDR shown on an SDR screen (the app reports no HDR yet) */
  /* subtitles inside the file only when the app picks one; the app draws its own */
  opt(h, "sid", "no");
  opt(h, "sub-auto", "no");
  /* the network: streams from debrid services and addons drop now and then */
  opt(h, "cache", "yes");
  opt(h, "demuxer-max-bytes", "150MiB");
  opt(h, "demuxer-max-back-bytes", "50MiB");
  opt(h, "demuxer-seekable-cache", "yes");
  opt(h, "network-timeout", "30");
  opt(h, "user-agent", "StremioOrigin (macOS; native)");
  opt(h, "stream-lavf-o", "reconnect=1,reconnect_streamed=1,reconnect_on_network_error=1,reconnect_delay_max=7");
  opt(h, "hr-seek", "no"); /* seeks land on a keyframe: at once, not after decoding up to the exact second */
  opt(h, "audio-client-name", "Origin");
  if (getenv("SB_MUTE"))
    opt(h, "mute", "yes"); /* tests: no sound through the speakers */
  if (getenv("SB_MPV_OPTS")) {
    char b[1000];
    snprintf(b, sizeof b, "%s", getenv("SB_MPV_OPTS")); /* development: "key=value;key=value" */
    for (char *t = strtok(b, ";"); t; t = strtok(NULL, ";")) {
      char *eq = strchr(t, '=');
      if (eq) {
        *eq = 0;
        opt(h, t, eq + 1);
      }
    }
  }
  if (getenv("SB_MPV_LOG")) {
    opt(h, "log-file", getenv("SB_MPV_LOG"));
    opt(h, "msg-level", "all=v");
  } /* development */
  int r = M.initialize(h);
  if (r < 0) {
    fprintf(stderr, "[video] mpv did not start: %s\n", M.error_string(r));
    atomic_store(&loaderState, -1);
    return NULL;
  }
  fprintf(stderr, "[video] libmpv ready: library %u ms, start %u ms\n", t1 - t0, SDL_GetTicks() - t1);
  {
    int n = device_channels();
    if (n > 0)
      atomic_store(&mixCh, n);
  }
  H = h;
  atomic_store(&loaderState, 2);
  return NULL;
}
static void wake_loader(void) {
  pthread_mutex_lock(&wakeM);
  woken = 1;
  pthread_cond_signal(&wakeC);
  pthread_mutex_unlock(&wakeM);
}
int video_init(const char *appId) {
  (void)appId;
  static int started;
  if (started)
    return 1;
  started = 1;
  pthread_t th;
  if (pthread_create(&th, NULL, loader, NULL))
    return 0;
  pthread_detach(th);
  return 1;
}
static void on_update(void *ud) { /* (mpv's thread) */
  (void)ud;
  atomic_store(&frameWaiting, 1);
}
static void *gl_proc(void *ctx, const char *name) {
  (void)ctx;
  return SDL_GL_GetProcAddress(name);
}
static int ready(void) {
  if (atomic_load(&loaderState) != 2)
    return 0;
  if (!hooked) {
    hooked = 1;
    mpv_opengl_init_params gp = { gl_proc, NULL };
    mpv_render_param rp[] = { { MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_OPENGL },
                              { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gp },
                              { MPV_RENDER_PARAM_INVALID, NULL } };
    int r = M.render_create(&RC, H, rp);
    gfx_restore_state(); /* (mpv set up its own GL state) */
    if (r < 0) {
      fprintf(stderr, "[video] mpv cannot draw into this GL context: %s\n", M.error_string(r));
      RC = NULL;
      atomic_store(&loaderState, -1);
      return 0;
    }
    M.render_set_update_callback(RC, on_update, NULL);
    static const struct {
      int id;
      const char *n;
      mpv_format f;
    } O[] = { { P_TIME, "time-pos", MPV_FORMAT_DOUBLE },
              { P_DUR, "duration", MPV_FORMAT_DOUBLE },
              { P_PAUSE, "pause", MPV_FORMAT_FLAG },
              { P_CACHE, "paused-for-cache", MPV_FORMAT_FLAG },
              { P_EOF, "eof-reached", MPV_FORMAT_FLAG },
              { P_SEEKING, "seeking", MPV_FORMAT_FLAG },
              { P_BUF, "demuxer-cache-time", MPV_FORMAT_DOUBLE },
              { P_W, "video-params/w", MPV_FORMAT_INT64 },
              { P_H, "video-params/h", MPV_FORMAT_INT64 },
              { P_FPS, "container-fps", MPV_FORMAT_DOUBLE },
              { P_GAMMA, "video-params/gamma", MPV_FORMAT_STRING },
              { P_DV, "current-tracks/video/dolby-vision-profile", MPV_FORMAT_INT64 },
              { P_TRACKS, "track-list", MPV_FORMAT_NODE },
              { P_HWDEC, "hwdec-current", MPV_FORMAT_STRING },
              { P_DROP, "frame-drop-count", MPV_FORMAT_INT64 },
              { P_AO, "current-ao", MPV_FORMAT_STRING } };
    for (size_t i = 0; i < sizeof O / sizeof O[0]; i++)
      M.observe_property(H, (uint64_t)O[i].id, O[i].n, O[i].f);
    M.request_log_messages(H, "warn");
    int rss, sw;
    plat_mem_self(&rss, &sw);
    fprintf(stderr, "[video] mpv in use (app %d MB)\n", rss);
  }
  return 1;
}

/* ---- commands ---- */
static void cmd(uint64_t tag, const char **a) {
  if (H)
    M.command_async(H, tag, a);
}
static void set_str(const char *k, const char *v) {
  if (H)
    M.set_property_async(H, R_OTHER, k, MPV_FORMAT_STRING, &v);
}
static void set_dbl(const char *k, double v) {
  if (H)
    M.set_property_async(H, R_OTHER, k, MPV_FORMAT_DOUBLE, &v);
}
static void reset_state(void) {
  memset(&S, 0, sizeof S);
  S.reported = -1;
  nA = nT = 0;
  curA = -1;
  seekTarget = -1;
  seekSent = 0;
}
static void start_load(const char *u, double at) {
  reset_state();
  S.active = 1;
  S.st = VID_LOADING;
  loadAt = SDL_GetTicks();
  snprintf(url, sizeof url, "%s", u);
  fprintf(stderr, "[video] load %.200s%s\n", u, at > 0 ? " (from a position)" : "");
  if (!ready()) {
    snprintf(pendingUrl, sizeof pendingUrl, "%s", u);
    pendingAt = at;
    pending = 1;
    wake_loader();
    return;
  }
  pending = 0;
  char o[120];
  snprintf(o, sizeof o, "start=%.3f", at > 0 ? at : 0.0);
  apply_fold_down();
  {
    pthread_t th;
    if (!pthread_create(&th, NULL, ask_device, NULL))
      pthread_detach(th);
  } /* (a changed device: the next video, or this one in video_pump) */
  set_str("pause", "no");
  V.on = 0;
  V.cur = V.to = 100;
  set_dbl("volume", 100);
  R.crop = 0;
  /* http and https through FFmpeg's own reader, which reconnects at the same byte when a connection drops (a debrid
     link cutting out; stream-lavf-o above) */
  char via[4300];
  const char *lu = u;
  if ((!strncmp(u, "http://", 7) || !strncmp(u, "https://", 8)) && !getenv("SB_CURLHTTP")) {
    snprintf(via, sizeof via, "ffmpeg://%s", u);
    lu = via;
  }
  /* (loadfile's index argument, before the options, came in mpv 0.38: client API 2.3) */
  const char *a3[] = { "loadfile", lu, "replace", "-1", o, NULL }, *a2[] = { "loadfile", lu, "replace", o, NULL };
  S.pendingStarts++;
  cmd(R_LOAD, M.client_api_version() >= 0x20003 ? a3 : a2);
}
void video_load(const char *u) {
  timed.on = 0;
  start_load(u, 0);
}
int video_load_at(const char *u, double sec) {
  timed.on = 0;
  start_load(u, sec);
  return 1;
}
double video_reported_time(void) {
  return S.reported;
}
/* one continuous conversion: each seek loads "<base>?t=<s>" again; times count from the start of the file */
void video_load_timed(const char *base, double durSec) {
  snprintf(timed.base, sizeof timed.base, "%s", base);
  timed.dur = durSec;
  timed.off = 0;
  timed.on = 1;
  char u[4200];
  snprintf(u, sizeof u, "%s?t=0", base);
  start_load(u, 0);
}
void video_unload(void) {
  pending = 0;
  timed.on = 0;
  url[0] = 0;
  reset_state();
  V.on = 0;
  if (H) {
    const char *a[] = { "stop", NULL };
    cmd(R_OTHER, a);
  }
}
void video_play(void) {
  S.paused = 0;
  set_str("pause", "no");
}
void video_pause(void) {
  S.paused = 1;
  set_str("pause", "yes");
}
void video_seek(double sec) {
  if (sec < 0)
    sec = 0;
  if (timed.on) {
    char u[4200];
    snprintf(u, sizeof u, "%s?t=%.0f", timed.base, sec);
    double d = timed.dur;
    char b[4096];
    snprintf(b, sizeof b, "%s", timed.base);
    start_load(u, 0);
    timed.on = 1;
    timed.dur = d;
    timed.off = (double)(long)sec;
    snprintf(timed.base, sizeof timed.base, "%s", b);
    return;
  }
  seekTarget = sec;
  seekSent = 0; /* sent in video_pump: presses within one frame make one seek */
}
void video_seek_now(double sec) {
  video_seek(sec);
}

/* ---- what the app reads ---- */
double video_time(void) {
  if (seekTarget >= 0)
    return seekTarget;
  return S.t + (timed.on ? timed.off : 0);
}
double video_duration(void) {
  return timed.on && timed.dur > 0 ? timed.dur : S.dur;
}
double video_buffered_end(void) {
  return S.buf + (timed.on ? timed.off : 0);
}
static VidState state_now(void) {
  return !S.active  ? VID_IDLE
         : S.error  ? VID_ERROR
         : !S.live  ? VID_LOADING
         : S.eof    ? VID_ENDED
         : S.cache  ? VID_BUFFERING
         : S.paused ? VID_PAUSED
                    : VID_PLAYING;
}
VidState video_state(void) {
  return state_now();
} /* (a pause asked for a moment ago counts already) */
const char *video_error(void) {
  return S.err;
}
int video_live(void) {
  return S.live;
}
int video_has_media(void) {
  return S.active && (S.started || S.pendingStarts > 0);
}
int video_settling(void) {
  return seekTarget >= 0 || S.seeking;
}
int video_width(void) {
  return S.w;
}
int video_height(void) {
  return S.h;
}
float video_frame_rate(void) {
  return 0;
} /* (the screen's rate does not follow the video on a desktop) */
double video_fps(void) {
  return S.fps;
}
int video_hdr(char *out, int cap) {
  const char *h = !S.gamma[0]               ? ""
                  : !strcmp(S.gamma, "pq")  ? (S.dv > 0 ? "DolbyVision" : "HDR10")
                  : !strcmp(S.gamma, "hlg") ? "HLG"
                                            : "none";
  if (cap > 0)
    snprintf(out, (size_t)cap, "%s", h);
  return h[0] != 0;
}
int video_audio_unsupported(void) {
  return 0;
} /* mpv decodes every audio format itself */
int video_audio_tracks(VidTrack *out, int max) {
  int n = nA < max ? nA : max;
  memcpy(out, aT, (size_t)n * sizeof *out);
  return n;
}
int video_audio_current(void) {
  return curA;
}
void video_select_audio(int idx) {
  if (idx < 0 || idx >= nA)
    return;
  curA = idx;
  char v[24];
  snprintf(v, sizeof v, "%lld", (long long)aId[idx]);
  set_str("aid", v);
}
int video_text_tracks(VidTrack *out, int max) {
  int n = nT < max ? nT : max;
  memcpy(out, tT, (size_t)n * sizeof *out);
  return n;
}
void video_select_text(int idx) {
  fprintf(stderr, "[video] subtitles: %s%d\n", idx < 0 ? "off" : "track ", idx < 0 ? 0 : idx);
  if (idx < 0 || idx >= nT) {
    set_str("sid", "no");
    return;
  }
  char v[24];
  snprintf(v, sizeof v, "%lld", (long long)tId[idx]);
  set_str("sid", v);
}
/* the stream's own volume, reached over easeMs in steps each frame */
void video_set_volume(int pct, int easeMs) {
  if (pct < 0)
    pct = 0;
  if (pct > 100)
    pct = 100;
  if (easeMs <= 0) {
    V.on = 0;
    V.cur = V.to = pct;
    set_dbl("volume", pct);
    return;
  }
  V.from = V.cur;
  V.to = pct;
  V.t0 = SDL_GetTicks();
  V.ms = (Uint32)easeMs;
  V.on = 1;
}

/* ---- where the picture goes ----
   Fill and Zoom need no help from mpv: the whole frame is drawn into a texture the shape of the video, large enough
   that the cropped part covers the rectangle pixel for pixel, and only that part of it is shown */
static void clamp(float *x, float *y, float *w, float *h) {
  if (*x < 0) {
    *w += *x;
    *x = 0;
  }
  if (*y < 0) {
    *h += *y;
    *y = 0;
  }
  if (*x + *w > SB_W)
    *w = SB_W - *x;
  if (*y + *h > SB_H)
    *h = SB_H - *y;
}
void video_set_window(float x, float y, float w, float h) {
  clamp(&x, &y, &w, &h);
  if (w < 1 || h < 1)
    return;
  R.x = x;
  R.y = y;
  R.w = w;
  R.h = h;
  R.crop = 0;
  R.set = 1;
}
int video_partial_plane(void) {
  return 1;
}
int video_set_crop(int sx, int sy, int sw, int sh, float dx, float dy, float dw, float dh) {
  if (sw < 2 || sh < 2 || S.w <= 0 || S.h <= 0)
    return 0;
  clamp(&dx, &dy, &dw, &dh);
  if (dw < 1 || dh < 1)
    return 0;
  R.x = dx;
  R.y = dy;
  R.w = dw;
  R.h = dh;
  R.crop = 1;
  R.sx = sx;
  R.sy = sy;
  R.sw = sw;
  R.sh = sh;
  R.set = 1;
  return 1;
}
void video_set_surface(int w, int h) {
  (void)w;
  (void)h;
}
/* the frame's end (gfx_end): mpv's newest picture drawn into the texture the size of the video's rectangle, then the
   texture shown under everything the app drew this frame. Frames are taken as they come, before the first picture
   counts as shown too: mpv's output waits for each one to be drawn */
void video_frame_under(void) {
  if (!RC || !S.active || !R.set)
    return;
  int vx, vy, W, Hh;
  gfx_viewport(&vx, &vy, &W, &Hh);
  int pw = (int)(R.w * W / SB_W + 0.5f), ph = (int)(R.h * Hh / SB_H + 0.5f);
  float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
  if (R.crop && S.w > 0 && S.h > 0) { /* the frame at the scale the cropped part needs, its own shape (no bars) */
    pw = (int)((float)pw * S.w / R.sw + 0.5f);
    ph = (int)((float)pw * S.h / S.w + 0.5f);
    if (pw > 8192 || ph > 8192)
      return;
    u0 = (float)R.sx / S.w;
    u1 = (float)(R.sx + R.sw) / S.w;
    v0 = (float)R.sy / S.h;
    v1 = (float)(R.sy + R.sh) / S.h;
  }
  if (pw < 2 || ph < 2)
    return;
  int fresh = 0;
  if (tgt.w != pw || tgt.h != ph) {
    gfx_target_free(&tgt);
    if (!gfx_target_create(&tgt, pw, ph))
      return;
    fresh = 1;
  }
  if (atomic_exchange(&frameWaiting, 0) || fresh) {
    uint64_t f = M.render_update(RC);
    if ((f & MPV_RENDER_UPDATE_FRAME) || fresh) {
      GLint fbo = 0;
      glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
      mpv_opengl_fbo mf = { (int)tgt.fbo, pw, ph, 0 };
      int flip = 0, block = 0;
      mpv_render_param rp[] = { { MPV_RENDER_PARAM_OPENGL_FBO, &mf },
                                { MPV_RENDER_PARAM_FLIP_Y, &flip },
                                { MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block },
                                { MPV_RENDER_PARAM_INVALID, NULL } };
      M.render_render(RC, rp);
      glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
      gfx_restore_state();
    }
  }
  if (S.live)
    gfx_under(tgt.tex, R.x, R.y, R.w, R.h, u0, v0, u1, v1);
}

/* ---- mpv's news ---- */
static const char *node_str(const mpv_node *m, const char *k) {
  for (int i = 0; m->format == MPV_FORMAT_NODE_MAP && i < m->u.list->num; i++)
    if (!strcmp(m->u.list->keys[i], k)) {
      const mpv_node *v = &m->u.list->values[i];
      return v->format == MPV_FORMAT_STRING ? v->u.string : NULL;
    }
  return NULL;
}
static int64_t node_int(const mpv_node *m, const char *k, int64_t def) {
  for (int i = 0; m->format == MPV_FORMAT_NODE_MAP && i < m->u.list->num; i++)
    if (!strcmp(m->u.list->keys[i], k)) {
      const mpv_node *v = &m->u.list->values[i];
      return v->format == MPV_FORMAT_INT64 ? v->u.int64 : v->format == MPV_FORMAT_FLAG ? v->u.flag : def;
    }
  return def;
}
static const char *codec_name(const char *c) {
  if (!c)
    return "";
  if (!strcmp(c, "subrip"))
    return "srt";
  if (!strcmp(c, "hdmv_pgs_subtitle"))
    return "pgs";
  if (!strcmp(c, "dvd_subtitle"))
    return "vobsub";
  if (!strncmp(c, "pcm", 3))
    return "pcm";
  return c;
}
static void take_tracks(const mpv_node *n) {
  nA = nT = 0;
  curA = -1;
  if (!n || n->format != MPV_FORMAT_NODE_ARRAY)
    return;
  for (int i = 0; i < n->u.list->num; i++) {
    const mpv_node *m = &n->u.list->values[i];
    const char *ty = node_str(m, "type");
    if (!ty)
      continue;
    int au = !strcmp(ty, "audio");
    if (!au && strcmp(ty, "sub"))
      continue;
    if (node_int(m, "external", 0))
      continue;
    if ((au ? nA : nT) >= 24)
      continue;
    VidTrack *t = au ? &aT[nA] : &tT[nT];
    memset(t, 0, sizeof *t);
    const char *lang = node_str(m, "lang"), *title = node_str(m, "title");
    snprintf(t->lang, sizeof t->lang, "%s", lang ? lang : "");
    snprintf(t->name, sizeof t->name, "%s", title ? title : lang ? lang : "");
    snprintf(t->codec, sizeof t->codec, "%s", codec_name(node_str(m, "codec")));
    t->forced = (int)node_int(m, "forced", 0);
    t->channels = (int)node_int(m, "demux-channel-count", 0);
    if (au) {
      t->idx = nA;
      aId[nA] = node_int(m, "id", 0);
      if (node_int(m, "selected", 0))
        curA = nA;
      nA++;
    } else {
      t->idx = nT;
      tId[nT] = node_int(m, "id", 0);
      nT++;
    }
  }
}
static void on_prop(mpv_event_property *p, int id) {
  int none = p->format == MPV_FORMAT_NONE || !p->data;
  double d = !none && p->format == MPV_FORMAT_DOUBLE ? *(double *)p->data : 0;
  int f = !none && p->format == MPV_FORMAT_FLAG ? *(int *)p->data : 0;
  int64_t i = !none && p->format == MPV_FORMAT_INT64 ? *(int64_t *)p->data : 0;
  const char *s = !none && p->format == MPV_FORMAT_STRING ? *(char **)p->data : "";
  switch (id) {
  case P_TIME:
    if (!none) {
      S.t = d;
      S.reported = d;
    }
    break;
  case P_DUR:
    S.dur = d;
    break;
  case P_PAUSE:
    S.paused = f;
    break;
  case P_CACHE:
    S.cache = f;
    break;
  case P_EOF:
    S.eof = f;
    break;
  case P_SEEKING:
    S.seeking = f;
    break;
  case P_BUF:
    S.buf = d;
    break;
  case P_W:
    S.w = (int)i;
    break;
  case P_H:
    S.h = (int)i;
    break;
  case P_FPS:
    S.fps = d;
    break;
  case P_GAMMA:
    snprintf(S.gamma, sizeof S.gamma, "%s", s);
    break;
  case P_DV:
    S.dv = (int)i;
    break;
  case P_TRACKS:
    take_tracks(none ? NULL : (mpv_node *)p->data);
    fprintf(stderr, "[video] tracks: %d audio, %d text\n", nA, nT);
    break;
  case P_DROP: {
    static Uint32 at;
    if (i > 0 && SDL_GetTicks() - at > 2000) {
      at = SDL_GetTicks();
      fprintf(stderr, "[video] %lld frames dropped so far (at %.1f s)\n", (long long)i, S.t);
    }
    break;
  } /* (at most one line in 2 s) */
  case P_AO: {
    static char ao[24];
    if (s[0] && strcmp(s, ao)) {
      snprintf(ao, sizeof ao, "%s", s);
      fprintf(stderr, "[video] sound through %s\n", s);
    }
    break;
  }
  case P_HWDEC:
    if (s[0] && strcmp(s, S.hwdec)) {
      snprintf(S.hwdec, sizeof S.hwdec, "%s", s);
      fprintf(stderr, "[video] decoding: %s\n", s);
    }
    break;
  }
}
static void keep_awake(int on) { /* a film playing keeps the display on; a preview in a card does not */
  if (on == awake)
    return;
  awake = on;
  if (on)
    IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep, kIOPMAssertionLevelOn,
                                CFSTR("Origin is playing a video"), &awakeId);
  else if (awakeId) {
    IOPMAssertionRelease(awakeId);
    awakeId = 0;
  }
}
void video_pump(void) {
  if (pending && ready()) {
    char u[4200];
    snprintf(u, sizeof u, "%s", pendingUrl);
    Uint32 la = loadAt;
    start_load(u, pendingAt);
    loadAt = la;
  } /* (timed from the app's ask) */
  if (H && S.active)
    apply_fold_down(); /* (only acts when the device changed) */
  if (atomic_load(&loaderState) < 0 && S.active && !S.error) {
    S.error = 1;
    S.st = VID_ERROR;
    snprintf(S.err, sizeof S.err, "the video player could not start");
  }
  if (!ready())
    return;
  for (;;) {
    mpv_event *e = M.wait_event(H, 0);
    if (e->event_id == MPV_EVENT_NONE)
      break;
    switch (e->event_id) {
    case MPV_EVENT_START_FILE:
      if (S.pendingStarts > 0 && --S.pendingStarts == 0 && S.active)
        S.started = 1;
      break;
    case MPV_EVENT_FILE_LOADED:
      if (S.started) {
        S.loaded = 1;
        fprintf(stderr, "[video] opened in %u ms\n", SDL_GetTicks() - loadAt);
      }
      break;
    case MPV_EVENT_PLAYBACK_RESTART:
      if (S.started && !S.live) {
        S.live = 1;
        int rss, sw;
        plat_mem_self(&rss, &sw);
        fprintf(stderr, "[video] first picture %u ms after the load (app %d MB)\n", SDL_GetTicks() - loadAt, rss);
      }
      break;
    case MPV_EVENT_END_FILE: {
      mpv_event_end_file *ef = e->data;
      if (S.started && S.pendingStarts == 0 && S.active && ef->reason == MPV_END_FILE_REASON_ERROR) {
        S.error = 1;
        snprintf(S.err, sizeof S.err, "%s", M.error_string(ef->error));
        fprintf(stderr, "[video] failed: %s\n", S.err);
      } else if (S.started && S.pendingStarts == 0 && S.active && ef->reason == MPV_END_FILE_REASON_EOF)
        S.eof = 1;
      break;
    }
    case MPV_EVENT_COMMAND_REPLY:
      if (e->reply_userdata == R_LOAD && e->error < 0) {
        if (S.pendingStarts > 0)
          S.pendingStarts--;
        S.error = 1;
        snprintf(S.err, sizeof S.err, "%s", M.error_string(e->error));
      }
      break;
    case MPV_EVENT_PROPERTY_CHANGE:
      if (S.pendingStarts == 0 || e->reply_userdata == P_PAUSE)
        on_prop(e->data, (int)e->reply_userdata);
      break;
    case MPV_EVENT_LOG_MESSAGE: {
      mpv_event_log_message *l = e->data;
      fprintf(stderr, "[mpv] %s: %s", l->prefix, l->text);
      break;
    }
    default:
      break;
    }
  }
  if (seekTarget >= 0 && !seekSent && S.loaded) {
    char t[32];
    snprintf(t, sizeof t, "%.3f", seekTarget);
    const char *a[] = { "seek", t, "absolute", NULL };
    cmd(R_OTHER, a);
    seekSent = 1;
    S.seeking = 1;
    fprintf(stderr, "[video] seek to %.1f s\n", seekTarget);
  }
  if (seekSent && !S.seeking) {
    seekTarget = -1;
    seekSent = 0;
  }
  if (V.on) {
    Uint32 el = SDL_GetTicks() - V.t0;
    double k = el >= V.ms ? 1 : (double)el / V.ms, v = V.from + (V.to - V.from) * k;
    if (k >= 1)
      V.on = 0;
    if (v - V.cur > 0.5 || V.cur - v > 0.5 || !V.on) {
      V.cur = v;
      set_dbl("volume", v);
    }
  }
  VidState st = state_now();
  if (st != S.st) {
    static const char *nm[] = { "idle", "loading", "playing", "paused", "buffering", "ended", "error" };
    fprintf(stderr, "[video] state %s at %.1f s of %.1f (%dx%d)\n", nm[st], S.t, S.dur, S.w, S.h);
    S.st = st;
  }
  if (!S.active && tgt.tex)
    gfx_target_free(&tgt); /* (no video: the texture's memory goes back) */
  keep_awake(st == VID_PLAYING && R.w >= SB_W * 0.9f);
}
/* playback speed: only phones offer it (the Mac's player keeps 1) */
int video_set_rate(float rate) {
  (void)rate;
  return 0;
}
int video_rate_supported(void) {
  return 0;
}
