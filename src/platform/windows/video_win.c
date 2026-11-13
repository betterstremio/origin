/* Windows: libmpv plays into a child window (the plane) under the UI, which ANGLE draws as a DirectComposition layer
   above it, so the video shows where the UI is transparent, as on the TVs' video plane and Android's SurfaceView.
   mpv draws with its own Direct3D 11 output (gpu-next, d3d11va decoding without copies). Its options follow
   NuvioDesktop's player (GPL-3.0, github.com/NuvioMedia/NuvioDesktop) where they apply here.
   libmpv-2.dll is loaded on a background thread 1.2 s after the start (sooner if a video is asked for), so the start
   never waits for it. Every call into mpv from here is asynchronous: the drawing thread never waits on mpv. What mpv
   knows arrives as events and observed properties, read once a frame in video_pump. */
#include "core/video.h"
#include "ui/gfx.h"
#include "platform/plat.h"
#include "platform/mem.h"
int win_private_mb(void); /* (mem_win.c) */
#include "plat_win.h"
#include <mpv/client.h>
#include <SDL2/SDL.h>
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <windows.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
  mpv_handle *(*create)(void);
  int (*initialize)(mpv_handle *);
  int (*set_option_string)(mpv_handle *, const char *, const char *);
  int (*set_option)(mpv_handle *, const char *, mpv_format, void *);
  int (*command_async)(mpv_handle *, uint64_t, const char **);
  int (*set_property_async)(mpv_handle *, uint64_t, const char *, mpv_format, void *);
  int (*observe_property)(mpv_handle *, uint64_t, const char *, mpv_format);
  mpv_event *(*wait_event)(mpv_handle *, double);
  int (*request_log_messages)(mpv_handle *, const char *);
  const char *(*error_string)(int);
  unsigned long (*client_api_version)(void);
} M;
static mpv_handle *H;
static volatile LONG loaderState; /* 0 waiting, 1 loading, 2 ready, -1 failed */
static HANDLE wake;
static HWND plane;
static int hooked;

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
static int lastPx[4] = { -1, -1, -1, -1 }, lastCW, lastCH;
static char lastCrop[64];
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
   A 5.1 or 7.1 film on headphones or two speakers is folded down to stereo, and mpv's plain fold-down adds the
   channels up: loud scenes go past full scale and clip, heard as crackle (Inside Out 2's 7.1 TrueHD: +2.4 dBFS,
   about 2,000 clipped samples in 90 s). Where the device mixes in stereo the app folds it down itself, in the filter
   chain, and a limiter catches only the peaks past -0.5 dBFS: normal passages stay as loud as before. The device is
   asked once at mpv's start and again on a thread at each video start (headset unplugged, speakers chosen). */
int win_audio_channels(void); /* (audiodev_win.c) */
static volatile LONG mixCh; /* the channels the default output device mixes in; 0 unknown */
static int afOn = -1; /* the fold-down chain as last set: -1 never */
#define FOLD_DOWN "lavfi=[aformat=channel_layouts=stereo,alimiter=limit=0.944:attack=5:release=50:level=0:latency=1]"
static void *ask_device(void *a) {
  (void)a;
  plat_thread_background(5);
  int n = win_audio_channels();
  if (n > 0)
    InterlockedExchange(&mixCh, n);
  return NULL;
}
static void set_str(const char *k, const char *v);
static void apply_fold_down(void) { /* (drawing thread) */
  int want = mixCh == 2 && !getenv("SB_NO_FOLD");
  if (want == afOn || !mixCh)
    return;
  afOn = want;
  set_str("af", want ? FOLD_DOWN : "");
  fprintf(stderr, "[video] sound device mixes %ld channels: %s\n", (long)mixCh,
          want ? "surround folded to stereo here, peaks limited" : "mpv's own mapping");
}

/* ---- starting mpv ---- */
static void opt(mpv_handle *h, const char *k, const char *v) {
  int r = M.set_option_string(h, k, v);
  if (r < 0)
    fprintf(stderr, "[video] mpv option %s=%s: %s\n", k, v, M.error_string(r));
}
static void *loader(void *a) {
  (void)a;
  WaitForSingleObject(wake, 1200); /* Home first; a video asked for sooner wakes this at once */
  if (getenv("SB_NO_MPV")) {
    InterlockedExchange(&loaderState, -1);
    return NULL;
  } /* development: the app without its player, for comparisons */
  InterlockedExchange(&loaderState, 1);
  Uint32 t0 = SDL_GetTicks();
  wchar_t p[MAX_PATH];
  DWORD n = GetModuleFileNameW(NULL, p, MAX_PATH);
  wchar_t *s = n ? wcsrchr(p, L'\\') : NULL;
  if (s)
    wcscpy(s + 1, L"libmpv-2.dll");
  HMODULE dll = LoadLibraryExW(s ? p : L"libmpv-2.dll", NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!dll && GetLastError() == ERROR_MOD_NOT_FOUND &&
      s) { /* a library libmpv needs is not on this PC: the Vulkan loader, which GPU drivers
install (not Windows). Ours, from fallback\, only then: a PC with its own keeps that one */
    wchar_t fb[MAX_PATH];
    wcscpy(fb, p);
    wchar_t *e = wcsrchr(fb, L'\\');
    if (e) {
      wcscpy(e + 1, L"fallback");
      SetDllDirectoryW(fb);
    }
    dll = LoadLibraryExW(p, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    SetDllDirectoryW(NULL);
    fprintf(stderr, "[video] libmpv needed a library this PC lacks: %s with the bundled Vulkan loader\n",
            dll ? "loaded" : "still not loaded");
  }
  if (!dll) {
    fprintf(stderr, "[video] libmpv-2.dll missing beside the exe (error %lu)\n", GetLastError());
    InterlockedExchange(&loaderState, -1);
    return NULL;
  }
#define L(f, name)                                       \
  if (!(*(FARPROC *)&M.f = GetProcAddress(dll, name))) { \
    fprintf(stderr, "[video] libmpv lacks %s\n", name);  \
    InterlockedExchange(&loaderState, -1);               \
    return NULL;                                         \
  }
  // clang-format off
  L(create, "mpv_create")
  L(initialize, "mpv_initialize")
  L(set_option_string, "mpv_set_option_string")
  L(set_option, "mpv_set_option")
  L(command_async, "mpv_command_async")
  L(set_property_async, "mpv_set_property_async")
  L(observe_property, "mpv_observe_property")
  L(wait_event, "mpv_wait_event")
  L(request_log_messages, "mpv_request_log_messages")
  L(error_string, "mpv_error_string")
  L(client_api_version, "mpv_client_api_version")
  // clang-format on
#undef L
  if ((M.client_api_version() >> 16) != 2) {
    fprintf(stderr, "[video] libmpv API %lu.%lu, 2.x needed\n", M.client_api_version() >> 16,
            M.client_api_version() & 0xffff);
    InterlockedExchange(&loaderState, -1);
    return NULL;
  }
  Uint32 t1 = SDL_GetTicks();
  mpv_handle *h = M.create();
  if (!h) {
    InterlockedExchange(&loaderState, -1);
    return NULL;
  }
  int64_t wid = (int64_t)(intptr_t)plane;
  M.set_option(h, "wid", MPV_FORMAT_INT64, &wid);
  char cache[1200];
  snprintf(cache, sizeof cache, "%s/mpv-shaders", plat_data_dir());
  mkdir(cache, 0755);
  /* nothing of mpv's own: no config files, scripts, keys, on-screen controls or cursor handling (the app does all that)
   */
  opt(h, "config", "no");
  opt(h, "load-scripts", "no");
  opt(h, "ytdl", "no");
  opt(h, "terminal", "no");
  opt(h, "osc", "no");
  opt(h, "osd-level", "0");
  opt(h, "input-default-bindings", "no");
  opt(h, "input-vo-keyboard", "no");
  opt(h, "input-cursor", "no");
  opt(h, "cursor-autohide", "no");
  /* one core for the app's whole life, its window kept: no start-up per video */
  opt(h, "idle", "yes");
  opt(h, "force-window", "yes");
  opt(h, "keep-open", "yes");
  opt(h, "audio-display", "no");
  opt(h, "vo", "gpu-next,gpu,");
  opt(h, "gpu-api", "d3d11");
  opt(h, "hwdec", "auto-safe");
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
  opt(h, "user-agent", "StremioOrigin (Windows; native)");
  opt(h, "stream-lavf-o", "reconnect=1,reconnect_streamed=1,reconnect_on_network_error=1,reconnect_delay_max=7");
  opt(h, "hr-seek", "no"); /* seeks land on a keyframe: at once, not after decoding up to the exact second */
  opt(h, "audio-client-name", "Origin");
  if (getenv("SB_WARP")) {
    opt(h, "d3d11-warp", "yes");
    opt(h, "hwdec", "no");
  } /* development: no GPU (plat_win.c) */
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
    InterlockedExchange(&loaderState, -1);
    return NULL;
  }
  fprintf(stderr, "[video] libmpv ready: library %u ms, start %u ms\n", t1 - t0, SDL_GetTicks() - t1);
  {
    int n = win_audio_channels();
    if (n > 0)
      InterlockedExchange(&mixCh, n);
  }
  H = h;
  MemoryBarrier();
  InterlockedExchange(&loaderState, 2);
  return NULL;
}
static LRESULT CALLBACK plane_proc(HWND w, UINT m, WPARAM a, LPARAM b) {
  if (m == WM_NCHITTEST)
    return HTTRANSPARENT;
  return DefWindowProcW(w, m, a, b);
}
int video_init(const char *appId) {
  (void)appId;
  if (plane)
    return 1;
  HWND top = plat_win_hwnd();
  if (!top) {
    fprintf(stderr, "[video] no window to put the video in\n");
    return 0;
  }
  WNDCLASSW wc = { 0 };
  wc.lpfnWndProc = plane_proc;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.lpszClassName = L"OriginVideo"; /* (no background: mpv's window fills it) */
  RegisterClassW(&wc);
  /* disabled: the mouse goes to the app's window, never to the video's */
  plane = CreateWindowExW(0, L"OriginVideo", L"", WS_CHILD | WS_CLIPSIBLINGS | WS_DISABLED, 0, 0, 16, 16, top, NULL,
                          wc.hInstance, NULL);
  if (!plane) {
    fprintf(stderr, "[video] no video window (error %lu)\n", GetLastError());
    return 0;
  }
  wake = CreateEventW(NULL, TRUE, FALSE, NULL);
  pthread_t th;
  if (pthread_create(&th, NULL, loader, NULL))
    return 0;
  pthread_detach(th);
  return 1;
}
/* mpv's own window inside the plane: disabled too, so a click on the picture reaches the app */
static BOOL CALLBACK disable_child(HWND w, LPARAM l) {
  (void)l;
  if (IsWindowEnabled(w))
    EnableWindow(w, FALSE);
  return TRUE;
}
static int ready(void) {
  if (loaderState != 2)
    return 0;
  if (!hooked) {
    hooked = 1;
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
    EnumChildWindows(plane, disable_child, 0);
    int rss, sw;
    plat_mem_self(&rss, &sw);
    fprintf(stderr, "[video] mpv in use (app %d MB, %d MB private)\n", rss, win_private_mb());
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
static void show(int on) {
  if (plane && (IsWindowVisible(plane) != 0) != on)
    ShowWindow(plane, on ? SW_SHOWNA : SW_HIDE);
}
static void reset_state(void) {
  memset(&S, 0, sizeof S);
  S.reported = -1;
  nA = nT = 0;
  curA = -1;
  seekTarget = -1;
  seekSent = 0;
  lastCrop[0] = 0;
  show(0);
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
    if (wake)
      SetEvent(wake);
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
  set_str("video-crop", "0x0+0+0");
  lastCrop[0] = 0; /* (mpv keeps a crop across files: the last video's would not fit this one) */
  /* http and https through FFmpeg's own reader: this libmpv's built-in one stops for good when a connection drops
     (a debrid link cutting out: "buffering" for ever), FFmpeg's reconnects at the same byte (stream-lavf-o above) */
  char via[4300];
  const char *lu = u;
  if ((!strncmp(u, "http://", 7) || !strncmp(u, "https://", 8)) &&
      !getenv("SB_CURLHTTP")) { /* (SB_CURLHTTP: the old reader, for comparisons) */
    snprintf(via, sizeof via, "ffmpeg://%s", u);
    lu = via;
  }
  const char *a[] = { "loadfile", lu, "replace", "-1", o, NULL };
  S.pendingStarts++;
  cmd(R_LOAD, a);
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

/* ---- where the picture goes ---- */
/* layout units (SB_W x SB_H) to the window's pixels */
static void place(void) {
  if (!plane || !R.set)
    return;
  int vx, vy, W, Hh;
  gfx_viewport(&vx, &vy, &W, &Hh);
  if (W <= 0 || Hh <= 0)
    return;
  int px[4] = { vx + (int)(R.x * W / SB_W + 0.5f), vy + (int)(R.y * Hh / SB_H + 0.5f), (int)(R.w * W / SB_W + 0.5f),
                (int)(R.h * Hh / SB_H + 0.5f) };
  if (memcmp(px, lastPx, sizeof px) || W != lastCW || Hh != lastCH) {
    memcpy(lastPx, px, sizeof px);
    lastCW = W;
    lastCH = Hh;
    SetWindowPos(plane, NULL, px[0], px[1], px[2] > 1 ? px[2] : 1, px[3] > 1 ? px[3] : 1,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
  }
  char c[64] = "";
  if (R.crop)
    snprintf(c, sizeof c, "%dx%d+%d+%d", R.sw, R.sh, R.sx, R.sy);
  if (H && strcmp(c, lastCrop)) {
    snprintf(lastCrop, sizeof lastCrop, "%s", c);
    set_str("video-crop", c[0] ? c : "0x0+0+0");
  }
}
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
  place();
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
  place();
  return 1;
}
void video_set_surface(int w, int h) {
  (void)w;
  (void)h;
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
    VidTrack *t = au ? &aT[nA] : &tT[nT];
    if ((au ? nA : nT) >= 24)
      continue;
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
      static double pauseAt = -2;
      if (pauseAt == -2)
        pauseAt = getenv("SB_PAUSE_AT") ? atof(getenv("SB_PAUSE_AT"))
                                        : -1; /* development: hold the picture there, for comparisons */
      if (pauseAt >= 0 && d >= pauseAt) {
        pauseAt = -1;
        video_pause();
        fprintf(stderr, "[video] paused at %.3f s (SB_PAUSE_AT)\n", d);
      }
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
static void keep_awake(int on) { /* a film playing keeps the screen on; a preview in a card does not */
  if (on == awake)
    return;
  awake = on;
  SetThreadExecutionState(ES_CONTINUOUS | (on ? ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED : 0));
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
  if (loaderState < 0 && S.active && !S.error) {
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
        EnumChildWindows(plane, disable_child, 0);
        int rss, sw;
        plat_mem_self(&rss, &sw);
        fprintf(stderr, "[video] first picture %u ms after the load (app %d MB, %d MB private)\n",
                SDL_GetTicks() - loadAt, rss, win_private_mb());
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
  if (R.set)
    place(); /* (the window may have changed size) */
  show(S.active && S.live && R.set);
  {
    static int said;
    if (!said && S.live && IsWindowVisible(plane)) {
      said = 1;
      RECT r;
      GetWindowRect(plane, &r);
      POINT c = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
      HWND w = WindowFromPoint(c);
      fprintf(stderr, "[video] a click on the picture goes to %s\n",
              w == (HWND)plat_win_hwnd() ? "the app" : "another window (mpv's): clicks lost");
    }
  }
  keep_awake(st == VID_PLAYING && R.w >= SB_W * 0.9f);
}
