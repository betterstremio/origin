/* Android: Media3 ExoPlayer (tv.stremioorigin.app.SbPlayer, Java) plays into its own SurfaceView under SDL's
   translucent one, as the TV's video plane sits under the app's window on webOS. Everything ExoPlayer knows
   is read on its own thread; SbPlayer.poll() hands over one snapshot, taken once a frame here (video_pump),
   so every getter below is a plain read. Rectangles go over as fractions of SDL's surface. */
#include "core/video.h"
#include "ui/gfx.h"
#include <SDL2/SDL.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static jclass cls;
static jmethodID mLoad, mUnload, mPlay, mPause, mSeek, mVol, mPoll, mWin, mCrop, mTracks, mSelA, mSelT, mErr, mHdr;
static struct {
  double st, t, dur, buf, w, h, fps, live, media, settling, audioBad, audioCur;
} S;
static char err[256], hdr[32];
static VidTrack aT[24], tT[24];
static int nA, nT;
static unsigned trackGen, seenGen = ~0u;
static double timedDur;
static char timedBase[4096];

static JNIEnv *env(void) {
  return (JNIEnv *)SDL_AndroidGetJNIEnv();
}
static void clear_exc(JNIEnv *e, const char *where) {
  if ((*e)->ExceptionCheck(e)) {
    (*e)->ExceptionDescribe(e);
    (*e)->ExceptionClear(e);
    fprintf(stderr, "[video] java exception in %s\n", where);
  }
}

int video_init(const char *appId) {
  (void)appId;
  JNIEnv *e = env();
  if (!e)
    return 0;
  jclass c = (*e)->FindClass(e, "tv/stremioorigin/app/SbPlayer");
  if (!c) {
    clear_exc(e, "init");
    fprintf(stderr, "[video] no SbPlayer class\n");
    return 0;
  }
  cls = (*e)->NewGlobalRef(e, c);
  (*e)->DeleteLocalRef(e, c);
#define M(v, n, s)                           \
  v = (*e)->GetStaticMethodID(e, cls, n, s); \
  if (!v) {                                  \
    clear_exc(e, n);                         \
    return 0;                                \
  }
  M(mLoad, "load", "(Ljava/lang/String;D)V");
  M(mUnload, "unload", "()V");
  M(mPlay, "play", "()V");
  M(mPause, "pause", "()V");
  M(mSeek, "seek", "(D)V");
  M(mVol, "setVolume", "(II)V");
  M(mPoll, "poll", "([D)I");
  M(mWin, "setWindow", "(FFFF)V");
  M(mCrop, "setCrop", "(FFFFFFFF)Z");
  M(mTracks, "tracks", "(I)Ljava/lang/String;");
  M(mSelA, "selectAudio", "(I)V");
  M(mSelT, "selectText", "(I)V");
  M(mErr, "error", "()Ljava/lang/String;");
  M(mHdr, "hdr", "()Ljava/lang/String;");
#undef M
  fprintf(stderr, "[video] ExoPlayer bridge ready\n");
  return 1;
}
static void call_v(jmethodID m, const char *n) {
  JNIEnv *e = env();
  if (!cls || !e)
    return;
  (*e)->CallStaticVoidMethod(e, cls, m);
  clear_exc(e, n);
}
static void load_at(const char *url, double startSec) {
  JNIEnv *e = env();
  if (!cls || !e)
    return;
  jstring s = (*e)->NewStringUTF(e, url);
  (*e)->CallStaticVoidMethod(e, cls, mLoad, s, startSec);
  (*e)->DeleteLocalRef(e, s);
  clear_exc(e, "load");
  memset(&S, 0, sizeof S);
  S.st = VID_LOADING;
  err[0] = hdr[0] = 0;
  nA = nT = 0;
  seenGen = ~0u;
  fprintf(stderr, "[video] load %.200s%s\n", url, startSec > 0 ? " (timed)" : "");
}
void video_load(const char *url) {
  timedBase[0] = 0;
  timedDur = 0;
  load_at(url, 0);
}
/* one continuous conversion: each seek loads "<base>?t=<s>" again; the times count from the start of the file */
static double timedOff;
void video_load_timed(const char *base, double durSec) {
  snprintf(timedBase, sizeof timedBase, "%s", base);
  timedDur = durSec;
  timedOff = 0;
  char u[4200];
  snprintf(u, sizeof u, "%s?t=0", base);
  load_at(u, 0);
}
void video_unload(void) {
  call_v(mUnload, "unload");
  memset(&S, 0, sizeof S);
  timedBase[0] = 0;
  nA = nT = 0;
}
void video_play(void) {
  call_v(mPlay, "play");
}
void video_pause(void) {
  call_v(mPause, "pause");
}
void video_seek(double sec) {
  if (timedBase[0]) {
    char u[4200];
    snprintf(u, sizeof u, "%s?t=%.0f", timedBase, sec);
    timedOff = (double)(long)sec;
    char b[4096];
    snprintf(b, sizeof b, "%s", timedBase);
    double d = timedDur;
    load_at(u, 0);
    snprintf(timedBase, sizeof timedBase, "%s", b);
    timedDur = d;
    return;
  }
  fprintf(stderr, "[video] seek to %.1f s\n", sec);
  JNIEnv *e = env();
  if (!cls || !e)
    return;
  (*e)->CallStaticVoidMethod(e, cls, mSeek, sec);
  clear_exc(e, "seek");
  S.settling = 1;
}
void video_seek_now(double sec) {
  video_seek(sec);
}
int video_load_at(const char *u, double sec) {
  (void)sec;
  video_load(u);
  return 0;
}
double video_reported_time(void) {
  return -1;
}
double video_time(void) {
  return S.t + (timedBase[0] ? timedOff : 0);
}
double video_duration(void) {
  return timedBase[0] && timedDur > 0 ? timedDur : S.dur;
}
double video_buffered_end(void) {
  return S.buf + (timedBase[0] ? timedOff : 0);
}
VidState video_state(void) {
  return (VidState)(int)S.st;
}
const char *video_error(void) {
  return err;
}
int video_live(void) {
  return S.live != 0;
}
int video_has_media(void) {
  return S.media != 0;
}
void video_set_volume(int pct, int easeMs) {
  JNIEnv *e = env();
  if (!cls || !e)
    return;
  (*e)->CallStaticVoidMethod(e, cls, mVol, pct, easeMs);
  clear_exc(e, "volume");
}
int video_settling(void) {
  return S.settling != 0;
}
int video_width(void) {
  return (int)S.w;
}
int video_height(void) {
  return (int)S.h;
}
float video_frame_rate(void) {
  return 0;
} /* (the screen is not followed on Android yet) */
double video_fps(void) {
  return S.fps;
}
int video_hdr(char *out, int cap) {
  if (cap > 0)
    snprintf(out, (size_t)cap, "%s", hdr);
  return hdr[0] != 0;
}

/* "idx\tlang\tname\tcodec\tforced\tchannels\n" per track */
static int parse_tracks(const char *s, VidTrack *out, int max) {
  int n = 0;
  while (s && *s && n < max) {
    VidTrack *t = &out[n];
    memset(t, 0, sizeof *t);
    const char *nl = strchr(s, '\n');
    size_t len = nl ? (size_t)(nl - s) : strlen(s);
    char line[400];
    snprintf(line, sizeof line, "%.*s", (int)(len < sizeof line ? len : sizeof line - 1), s);
    char *f[6] = { 0 };
    int k = 0;
    char *p = line;
    f[k++] = p;
    while (*p && k < 6) {
      if (*p == '\t') {
        *p = 0;
        f[k++] = p + 1;
      }
      p++;
    }
    if (k == 6) {
      t->idx = atoi(f[0]);
      snprintf(t->lang, sizeof t->lang, "%s", f[1]);
      snprintf(t->name, sizeof t->name, "%s", f[2]);
      snprintf(t->codec, sizeof t->codec, "%s", f[3]);
      t->forced = atoi(f[4]);
      t->channels = atoi(f[5]);
      n++;
    }
    s = nl ? nl + 1 : NULL;
  }
  return n;
}
static void refresh_tracks(void) {
  if (seenGen == trackGen)
    return;
  seenGen = trackGen;
  JNIEnv *e = env();
  if (!cls || !e)
    return;
  for (int kind = 0; kind < 2; kind++) {
    jstring js = (jstring)(*e)->CallStaticObjectMethod(e, cls, mTracks, kind);
    clear_exc(e, "tracks");
    const char *s = js ? (*e)->GetStringUTFChars(e, js, NULL) : NULL;
    if (kind == 0)
      nA = parse_tracks(s, aT, 24);
    else
      nT = parse_tracks(s, tT, 24);
    if (s)
      (*e)->ReleaseStringUTFChars(e, js, s);
    if (js)
      (*e)->DeleteLocalRef(e, js);
  }
}
int video_audio_tracks(VidTrack *out, int max) {
  refresh_tracks();
  int n = nA < max ? nA : max;
  memcpy(out, aT, (size_t)n * sizeof *out);
  return n;
}
int video_audio_current(void) {
  return (int)S.audioCur;
}
void video_select_audio(int idx) {
  JNIEnv *e = env();
  if (!cls || !e)
    return;
  (*e)->CallStaticVoidMethod(e, cls, mSelA, idx);
  clear_exc(e, "selectAudio");
}
int video_text_tracks(VidTrack *out, int max) {
  refresh_tracks();
  int n = nT < max ? nT : max;
  memcpy(out, tT, (size_t)n * sizeof *out);
  return n;
}
void video_select_text(int idx) {
  JNIEnv *e = env();
  if (!cls || !e)
    return;
  (*e)->CallStaticVoidMethod(e, cls, mSelT, idx);
  clear_exc(e, "selectText");
}

static float lw[4] = { -1, -1, -1, -1 };
void video_set_window(float x, float y, float w, float h) {
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > SB_W)
    w = SB_W - x;
  if (y + h > SB_H)
    h = SB_H - y;
  if (lw[0] == x && lw[1] == y && lw[2] == w && lw[3] == h)
    return;
  lw[0] = x;
  lw[1] = y;
  lw[2] = w;
  lw[3] = h;
  JNIEnv *e = env();
  if (!cls || !e)
    return;
  (*e)->CallStaticVoidMethod(e, cls, mWin, x / SB_W, y / SB_H, w / SB_W, h / SB_H);
  clear_exc(e, "window");
}
int video_partial_plane(void) {
  return 1;
}
int video_set_crop(int sx, int sy, int sw, int sh, float dx, float dy, float dw, float dh) {
  int vw = (int)S.w, vh = (int)S.h;
  if (vw <= 0 || vh <= 0 || sw <= 0 || sh <= 0)
    return 0;
  lw[0] = -1;
  JNIEnv *e = env();
  if (!cls || !e)
    return 0;
  jboolean ok = (*e)->CallStaticBooleanMethod(e, cls, mCrop, (float)sx / vw, (float)sy / vh, (float)sw / vw,
                                              (float)sh / vh, dx / SB_W, dy / SB_H, dw / SB_W, dh / SB_H);
  clear_exc(e, "crop");
  return ok != 0;
}
void video_set_surface(int w, int h) {
  (void)w;
  (void)h;
}
int video_audio_unsupported(void) {
  return S.audioBad != 0;
}
void video_pump(void) {
  JNIEnv *e = env();
  if (!cls || !e)
    return;
  static jdoubleArray arr;
  if (!arr) {
    jdoubleArray a = (*e)->NewDoubleArray(e, 12);
    arr = (*e)->NewGlobalRef(e, a);
    (*e)->DeleteLocalRef(e, a);
  }
  jint flags = (*e)->CallStaticIntMethod(e, cls, mPoll, arr);
  clear_exc(e, "poll");
  (*e)->GetDoubleArrayRegion(e, arr, 0, 12, (jdouble *)&S);
  {
    static int lastSt = -1;
    static const char *nm[] = { "idle", "loading", "playing", "paused", "buffering", "ended", "error" };
    int st = (int)S.st;
    if (st != lastSt && st >= 0 && st <= 6) {
      fprintf(stderr, "[video] state %s at %.1f s of %.1f (%dx%d)\n", nm[st], S.t, S.dur, (int)S.w, (int)S.h);
      lastSt = st;
    }
  }
  if (flags & 1) {
    trackGen++;
    refresh_tracks();
    fprintf(stderr, "[video] tracks: %d audio, %d text\n", nA, nT);
  }
  if (flags & 2) {
    jstring js = (jstring)(*e)->CallStaticObjectMethod(e, cls, mErr);
    clear_exc(e, "error");
    const char *s = js ? (*e)->GetStringUTFChars(e, js, NULL) : NULL;
    snprintf(err, sizeof err, "%s", s ? s : "");
    if (s)
      (*e)->ReleaseStringUTFChars(e, js, s);
    if (js)
      (*e)->DeleteLocalRef(e, js);
  }
  if (flags & 4) {
    jstring js = (jstring)(*e)->CallStaticObjectMethod(e, cls, mHdr);
    clear_exc(e, "hdr");
    const char *s = js ? (*e)->GetStringUTFChars(e, js, NULL) : NULL;
    snprintf(hdr, sizeof hdr, "%s", s ? s : "");
    if (s)
      (*e)->ReleaseStringUTFChars(e, js, s);
    if (js)
      (*e)->DeleteLocalRef(e, js);
  }
}
