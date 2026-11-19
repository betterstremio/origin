#include "core/fillstat.h"
#include "platform/plat.h"
#include "ui/img.h"
#include "ui/jpegscale.h"
#include <SDL2/SDL.h>
#include "ui/gfx.h"
#include "ui/etc.h"
#include "core/net.h"
#include "core/store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <stdint.h>
/* the JPEG decoder's NEON loops on ARM (stb_image never switches them on by itself): Android (arm64 and v7, NDK clang)
   and any 64-bit ARM. Not the webOS ARMv7 build: its zig compiler stops with "Do not know how to split the result of this
   operator" on them (as it does on libwebp's NEON, see tools/targets/webos.sh) */
#if (defined(__ANDROID__) || defined(__aarch64__)) && defined(__ARM_NEON) && !defined(STBI_NO_SIMD)
#define STBI_NEON
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_GIF
#include "stb_image.h"
#include "ui/palette.h"
#include "stb_image_write.h"
#include "src/webp/decode.h"

/* area-average resample of the part (cx, cy, cw, ch) of the source to nw x nh (the pictures only ever
   shrink: posters arrive at 500x750 for a 264x396 card; a smaller source repeats pixels) */
static unsigned char *resample(const unsigned char *src, int w, int h, float cx, float cy, float cw, float ch, int nw,
                               int nh) {
  unsigned char *out = malloc((size_t)nw * nh * 4);
  if (!out)
    return NULL;
  for (int y = 0; y < nh; y++) {
    int y0 = (int)(cy + y * ch / nh), y1 = (int)(cy + (y + 1) * ch / nh);
    if (y1 <= y0)
      y1 = y0 + 1;
    if (y0 >= h)
      y0 = h - 1;
    if (y1 > h)
      y1 = h;
    for (int x = 0; x < nw; x++) {
      int x0 = (int)(cx + x * cw / nw), x1 = (int)(cx + (x + 1) * cw / nw);
      if (x1 <= x0)
        x1 = x0 + 1;
      if (x0 >= w)
        x0 = w - 1;
      if (x1 > w)
        x1 = w;
      unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
      for (int yy = y0; yy < y1; yy++) {
        const unsigned char *p = src + ((size_t)yy * w + x0) * 4;
        for (int xx = x0; xx < x1; xx++, p += 4) {
          r += p[0];
          g += p[1];
          b += p[2];
          a += p[3];
          n++;
        }
      }
      unsigned char *o = out + ((size_t)y * nw + x) * 4;
      if (!n)
        n = 1;
      o[0] = (unsigned char)(r / n);
      o[1] = (unsigned char)(g / n);
      o[2] = (unsigned char)(b / n);
      o[3] = (unsigned char)(a / n);
    }
  }
  return out;
}
static unsigned char *downscale(const unsigned char *src, int w, int h, int nw, int nh) {
  return resample(src, w, h, 0, 0, (float)w, (float)h, nw, nh);
}
/* rounded corners into the alpha (1 px of smoothing, the colour is kept so filtering leaves no dark rim) */
static void round_corners(unsigned char *px, int w, int h, float r) {
  if (r <= 0)
    return;
  if (r > w / 2.0f)
    r = w / 2.0f;
  if (r > h / 2.0f)
    r = h / 2.0f;
  int n = (int)ceilf(r) + 1;
  for (int y = 0; y < n && y < h; y++)
    for (int x = 0; x < n && x < w; x++) {
      float dx = r - (x + 0.5f), dy = r - (y + 0.5f);
      if (dx < 0 || dy < 0)
        continue;
      float d = sqrtf(dx * dx + dy * dy) - r, k = 0.5f - d;
      if (k >= 1)
        continue;
      if (k < 0)
        k = 0;
      int xs[2] = { x, w - 1 - x }, ys[2] = { y, h - 1 - y };
      for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
          unsigned char *p = px + ((size_t)ys[j] * w + xs[i]) * 4;
          p[3] = (unsigned char)(p[3] * k + 0.5f);
        }
    }
}

/* fit 3, a poster card cut from a wide picture (as Netflix's): the card's window of the picture's 16:9 middle, at the
   subject, so that the card opening wide (to 16:9) only uncovers the rest of the same picture */
#define OPEN_ASPECT (16.0f / 9.0f)
#define SUBJ_WIN 0.375f /* the window the subject is looked for in: a 300-wide card of an 800-wide picture */
/* where the subject of a picture is, as a fraction of its width: the window with the most detail and skin, on a
   64x36 copy (the bottom quarter, often ground or words, counts half; the middle is preferred a little) */
static float subject_x(const unsigned char *px, int w, int h, float rx, float ry, float rw, float rh) {
  enum { TW = 64, TH = 36 };
  unsigned char *t = resample(px, w, h, rx, ry, rw, rh, TW, TH);
  if (!t)
    return 0.5f;
  float col[TW] = { 0 };
  for (int y = 1; y < TH; y++)
    for (int x = 1; x < TW; x++) {
      const unsigned char *p = t + ((size_t)y * TW + x) * 4, *l = p - 4, *u = p - TW * 4;
      int r = p[0], g = p[1], b = p[2], mx = r > g ? (r > b ? r : b) : (g > b ? g : b),
          mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
      int lum = (2 * r + 5 * g + b) / 8, ll = (2 * l[0] + 5 * l[1] + l[2]) / 8, lu = (2 * u[0] + 5 * u[1] + u[2]) / 8;
      int edge = abs(lum - ll) + abs(lum - lu);
      int skin = r > 95 && g > 40 && b > 20 && mx - mn > 15 && abs(r - g) > 15 && r > g && r > b;
      col[x] += (y < TH * 3 / 4 ? 1.0f : 0.5f) * (float)(edge + (skin ? 40 : 0));
    }
  free(t);
  int win = (int)(TW * SUBJ_WIN + 0.5f), best = (TW - win) / 2;
  float bestV = -1;
  for (int s0 = 0; s0 + win <= TW; s0++) {
    float v = 0, c = (s0 + win / 2.0f) / TW;
    for (int x = s0; x < s0 + win; x++)
      v += col[x];
    v *= 1 - 0.5f * fabsf(c - 0.5f);
    if (v > bestV) {
      bestV = v;
      best = s0;
    }
  }
  return (best + win / 2.0f) / TW;
}
/* how far into a picture rw wide a window cw wide starts, to sit on the subject at focal (the same sum everywhere) */
float img_crop_left(float focal, float rw, float cw) {
  float l = focal * rw - cw / 2;
  return l < 0 ? 0 : l > rw - cw ? rw - cw : l;
}
/* the subject's place, kept beside the cache (a crop read back from the cache must know where it was cut) */
static void focal_write(const char *src, float f) {
  char k[2300], v[16];
  snprintf(k, sizeof k, "fx:%s", src);
  int n = snprintf(v, sizeof v, "%.4f", f);
  cache_write(k, (const unsigned char *)v, (size_t)n);
}
static float focal_read(const char *src) {
  char k[2300];
  snprintf(k, sizeof k, "fx:%s", src);
  size_t len = 0;
  unsigned char *d = cache_read(k, 14 * 86400, &len);
  if (!d)
    return -1;
  char v[16];
  size_t n = len < sizeof v - 1 ? len : sizeof v - 1;
  memcpy(v, d, n);
  v[n] = 0;
  free(d);
  float f = (float)atof(v);
  return f >= 0 && f <= 1 ? f : -1;
}
typedef struct Img {
  char *url;
  unsigned hash;
  int w, h; /* the cache key (a shaped variant carries its shape in it) and the requested size */
  char *src;
  int fit;
  float radius; /* the picture's own address; fit -1 = as it is, else the shape (img_get_shaped) */
  float focal; /* fit 3: where the subject is, across the picture's 16:9 middle (0..1; -1 not known) */
  int state; /* 0 queued, 1 loading, 2 ready, 3 failed */
  unsigned char *px;
  int pw, ph, pch; /* decoded pixels waiting for upload */
  int fmt; /* what px holds: 0 RGBA, 1 ETC RGB blocks, 2 ETC2 RGBA (EAC) blocks (encoded on the device, ui/etc.c) */
  long pbytes; /* the size of the pending data (set when it is queued for upload) */
  int texFmt; /* what its texture is: 0 RGBA, 1/2 ETC */
  int encTried; /* queued for the background encoder (once; cleared when the queue drops it) */
  unsigned char *eb; /* its encoded blocks, waiting for the main thread to swap the texture */
  int ef;
  float cornerPx; /* an ETC picture without corners of its own: the radius (texture pixels) the shader rounds it with */
  float ec;
  struct Img *enext;
  GLuint tex;
  float aspect;
  int bytes;
  GLuint upTex;
  int upRow; /* a large picture uploading in bands over several frames */
  int urgent; /* the picture the screen waits on (a backdrop): it goes up first, in bigger bands */
  int pin; /* never evicted (title art: small, wanted at once) */
  int inPri; /* asked to go ahead (img_ahead_shaped): class 2 in the queue, like pinned art */
  unsigned seq; /* the order it was queued in (on-screen pictures load in it: draw order) */
  int pfOnly; /* only a look-ahead has asked for it so far (not on screen) */
  int saveLater; /* its small copy is written once the decode slot is free */
  int gone; /* the host said it does not exist (404/410): not asked again */
  double readyAt; /* when its decode was done (an on-screen one may wait a moment for the ones before it) */
  struct Img *dnext;
  int inDone; /* decoded, waiting for upload (inDone: in that list now) */
  int wantPal, palAlpha, palReady;
  Palette pal; /* the colours of the picture, asked for by the accent */
  int wantTex; /* someone draws it (a picture only asked for its colours is never uploaded) */
  unsigned lastUse;
  int row;       /* the Home row that asked for it last (-1: none): eviction goes by distance from the focus */
  unsigned hpin; /* = hgen: kept while the first screen is pinned (see img_pin_begin) */
  int shown;  /* it has had a texture before (a return: no fade, no wait) */
  int lost;   /* why its texture is gone (1 budget, 2 guard, 3 dropped from the queue, 4 upload skipped; 0 none): development counters */
  int smOk;   /* its small copy is known to be on disk */
  int fromSm; /* its pixels came from the small copy */
  double askAt;
  int fails;
  double failAt; /* failed loads so far, and when the last one failed: asked again, it tries once more after a wait */
  struct Img *next;
} Img;
/* development: per fill-test phase (img_phase_*): evictions by cause, uploads, bytes uploaded per frame, picture memory peak */
static unsigned phEv[5], phUps, phUpFrames, phUpHist[130];
static long frUp, phPeak;
static Img *upCur;
static const long upBudget = 320L * 1024; /* per frame upload bytes (1 MB was tried: whole crops in one frame cost the frame 5 ms and the scroll test's p95 and late % rose) */
static unsigned stEvict, stReturn, stRetFade; /* development: pictures evicted, shown again, and of those faded in */
static int ctxRow = -1, focusRow = -1; /* the row being drawn; the focused row (set by the board each frame) */
static unsigned focusAt, hgen = 1;
static long hpinBytes; /* what the first-screen pins hold now */
#define BUCKETS 1024
static Img *table[BUCKETS];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
/* what to load: one list, each pick the one that matters most now: the backdrop the screen waits on (urg, taken first),
   then what is on screen (asked in the last two frames), then title art and pictures asked to go ahead, then what a
   look-ahead asked for. What is on screen goes in the order it was asked for, which is the order it is drawn in (rows
   top to bottom, cards left to right): a row fills in from its start, never from its middle; the others go most
   recently asked first (a fast scroll's passed titles wait behind the ones it reached) */
static Img *pend[1024];
static int qn;
static unsigned qSeq, frameNo;
/* the bytes of a picture's data, raw or compressed */
static long pic_bytes(const Img *im) {
  return im->fmt ? (long)etc_size(im->pw, im->ph, im->fmt == 2) : (long)im->pw * im->ph * im->pch;
}
static Img *urg; /* the one picture the screen waits on (the backdrop): taken before the list (not counted in qn) */
static void q_push(Img *im, int pri) {
  im->seq = ++qSeq;
  if (pri)
    im->inPri = 1;
  pend[qn++] = im;
} /* under mu; room checked by the caller */
static int q_class(const Img *im) {
  return im->urgent ? 4 : (!im->pfOnly && frameNo - im->lastUse <= 2) ? 3 : (im->pin || im->inPri) ? 2 : 1;
}
static int q_before(const Img *a, int ca, const Img *b) {
  return ca == 3 ? a->seq < b->seq : a->seq > b->seq;
} /* (same class: on screen, the first asked; else the newest) */
static Img *q_pop(void) { /* under mu, qn > 0 */
  int best = -1, bc = -1;
  for (int i = 0; i < qn; i++) {
    int c = q_class(pend[i]);
    if (c > bc || (c == bc && q_before(pend[i], c, pend[best]))) {
      best = i;
      bc = c;
    }
  }
  Img *im = pend[best];
  pend[best] = pend[--qn];
  im->inPri = 0;
  return im;
}
static Img *busy[8]; /* what each worker is loading now (under mu) */
static double mono_s(void);
/* decoded pictures waiting for upload: a list without a limit (a full array dropped them, and a dropped picture held
   its pixels and never showed: a backdrop waiting for one never changed again) */
static Img *doneHead;
static int dn;
static long dnBytes, dnPeak; /* pixels decoded and waiting for upload (a worker pause above a cap was tried: no effect on the memory peaks, dropped) */
static void done_push(Img *im) {
  if (im->inDone)
    return;
  im->inDone = 1;
  im->readyAt = mono_s();
  im->dnext = doneHead;
  doneHead = im;
  dn++;
  im->pbytes = pic_bytes(im);
  dnBytes += im->pbytes;
  if (dnBytes > dnPeak)
    dnPeak = dnBytes;
} /* under mu; never twice (it would point to itself and the list never end) */
static Img *done_pop(void) { /* the order of the loading: a backdrop, what is on screen (in its order), title art, the rest */
  Img **pp = &doneHead;
  int bc = -1;
  int returning = 0;
  for (Img **q = &doneHead; *q; q = &(*q)->dnext) {
    int c = q_class(*q);
    int back = c == 3 && (*q)->shown;
    if (c > bc || (c == bc && (back > returning || (back == returning && q_before(*q, c, *pp))))) {
      bc = c;
      returning = back;
      pp = q;
    }
  }
  if (*pp && bc == 3 && !returning) { /* new art fills in order; a returning texture can go up as soon as it is ready */
    const Img *im = *pp;
    int ahead = 0;
    for (int i = 0; i < qn && !ahead; i++)
      ahead = q_class(pend[i]) == 3 && pend[i]->seq < im->seq;
    for (int i = 0; i < 8 && !ahead; i++)
      ahead = busy[i] && busy[i] != im && q_class(busy[i]) == 3 && busy[i]->seq < im->seq;
    if (ahead && mono_s() - im->readyAt < 0.3)
      return NULL; /* (never longer: a slow one does not hold the rest) */
  }
  Img *im = *pp;
  if (im) {
    *pp = im->dnext;
    im->dnext = NULL;
    im->inDone = 0;
    dn--;
    dnBytes -= im->pbytes;
  }
  return im;
}
static long usedBytes, budgetBytes = SB_IMG_BUDGET_MB * 1024L * 1024;
static char cacheDir[1024];
#define WORKERS_MAX                                                                                                   \
  8 /* mostly waiting on the network; the decoding itself is limited (dec_enter). 6 under 1200 MB, each one in flight \
       holds a picture */

static unsigned fnv(const char *s) {
  unsigned h = 2166136261u;
  while (*s) {
    h ^= (unsigned char)*s++;
    h *= 16777619u;
  }
  return h;
}
static Img *find(const char *url, unsigned h) {
  for (Img *i = table[h % BUCKETS]; i; i = i->next)
    if (i->hash == h && !strcmp(i->url, url))
      return i;
  return NULL;
}

/* ---- worker: fetch (cache first), decode, scale ---- */
/* the picture as it is drawn is kept too, re-encoded small (a poster at its card size is ~15 KB of JPEG, logos a
   small PNG for their transparency): the next visit decodes that in a few ms instead of the full-size original */
typedef struct {
  unsigned char *b;
  size_t n, cap;
} WBuf;
static void wbuf_put(void *ctx, void *data, int size) {
  WBuf *w = ctx;
  if (w->n + size > w->cap) {
    w->cap = (w->n + size) * 2;
    w->b = realloc(w->b, w->cap);
  }
  memcpy(w->b + w->n, data, size);
  w->n += size;
}
static void small_save(const Img *im, const unsigned char *px, int w, int h) {
  if (!strncmp(im->src, "file://", 7) || w * h > 1280 * 720)
    return;
  int opaque = 1;
  for (long i = 3, n = (long)w * h * 4; i < n; i += 4)
    if (px[i] != 255) {
      opaque = 0;
      break;
    }
  WBuf b = { 0 };
  int ok;
  if (opaque) {
    unsigned char *rgb = malloc((size_t)w * h * 3);
    for (long i = 0, n = (long)w * h; i < n; i++) {
      rgb[i * 3] = px[i * 4];
      rgb[i * 3 + 1] = px[i * 4 + 1];
      rgb[i * 3 + 2] = px[i * 4 + 2];
    }
    ok = stbi_write_jpg_to_func(wbuf_put, &b, w, h, 3, rgb, 90);
    free(rgb);
  } else
    ok = stbi_write_png_to_func(wbuf_put, &b, w, h, 4, px, w * 4);
  if (ok && b.n) {
    char key[2400];
    snprintf(key, sizeof key, "sm:%dx%d:%s", im->w, im->h, im->url);
    cache_write(key, b.b, b.n);
  }
  free(b.b);
}
static int small_load(Img *im) {
  char key[2400];
  snprintf(key, sizeof key, "sm:%dx%d:%s", im->w, im->h, im->url);
  size_t len = 0;
  if (im->fit == 3 && (im->focal = focal_read(im->src)) < 0)
    return 0; /* (a crop without its subject's place: made again from the picture) */
  unsigned char *data = cache_read(key, 14 * 86400, &len);
  if (!data)
    return 0;
  int w, h, ch;
  unsigned char *px = stbi_load_from_memory(data, (int)len, &w, &h, &ch, 4);
  free(data);
  if (!px)
    return 0;
  if (im->wantPal && !im->palReady) {
    int st = 1;
    while ((w / st) * (h / st) > 40000)
      st++;
    palette_of(px, w, h, im->palAlpha, st, &im->pal);
    im->palReady = 1;
  }
  if (im->fit >= 0) {
    round_corners(px, w, h, im->radius);
    im->aspect = (float)im->w / im->h;
  } else
    im->aspect = (float)w / h;
  im->px = px;
  im->pw = w;
  im->ph = h;
  im->pch = 4;
  im->fromSm = 1;
  return 1;
}
static pthread_mutex_t decMu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t decCv = PTHREAD_COND_INITIALIZER;
static int decBusy, decMax = 2; /* two; set once in img_init */
static void dec_enter(void) {
  pthread_mutex_lock(&decMu);
  while (decBusy >= decMax)
    pthread_cond_wait(&decCv, &decMu);
  decBusy++;
  pthread_mutex_unlock(&decMu);
}
static void dec_leave(void) {
  pthread_mutex_lock(&decMu);
  decBusy--;
  pthread_cond_signal(&decCv);
  pthread_mutex_unlock(&decMu);
}
static void load_(Img *im);
static void decode_part(Img *im, unsigned char *data, size_t len, int fromNet, const char *key);
static double mono_s(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}
/* ---- compressed pictures: where the GPU takes ETC1 / ETC2 a picture is kept as blocks (a poster is 0.5 byte a pixel, 1
   with alpha, against 4 as RGBA: the same budget holds 4-8 times more of them). The first show never waits for an encode
   (on the TV it is ~1 us a pixel): the picture loads as RGBA as before, and ONE encoder thread, at the lowest priority
   and only while the loader has no on-screen work, turns its small copy into blocks, writes them to disk ("etc:" key:
   the next visit loads them with no decode at all) and hands them to the main thread, which swaps the texture ---- */
#define ETC_MAGIC "SBETC2"
#define ETC_HDR 28 /* magic(6) fmt(1) 0(1) pw(4) ph(4) aspect(4) focal(4) corner(4), then the blocks */
static unsigned etcN;
static long etcUs; /* pictures encoded so far and the time it took (microseconds) */
static int etcLog = -1; /* SB_UPLOG: each encode logged */
static int etc_alpha(const unsigned char *px, int w, int h) {
  for (long i = 3, n = (long)w * h * 4; i < n; i += 4)
    if (px[i] != 255)
      return 1;
  return 0;
}
static void etc_write(const Img *im, int fmt, int w, int h, const unsigned char *blocks, float corner) {
  size_t sz = etc_size(w, h, fmt == 2);
  unsigned char *b = malloc(ETC_HDR + sz);
  if (!b)
    return;
  memcpy(b, ETC_MAGIC, 6);
  b[6] = (unsigned char)fmt;
  b[7] = 0;
  memcpy(b + 8, &w, 4);
  memcpy(b + 12, &h, 4);
  memcpy(b + 16, &im->aspect, 4);
  memcpy(b + 20, &im->focal, 4);
  memcpy(b + 24, &corner, 4);
  memcpy(b + ETC_HDR, blocks, sz);
  char key[2400];
  snprintf(key, sizeof key, "etc:%dx%d:%s", im->w, im->h, im->url);
  cache_write(key, b, ETC_HDR + sz);
  free(b);
}
static int etc_load(Img *im) {
  int caps = gfx_etc_caps();
  if (!caps || (im->wantPal && !im->palReady))
    return 0; /* (the colours are made from RGBA: the old way) */
  char key[2400];
  snprintf(key, sizeof key, "etc:%dx%d:%s", im->w, im->h, im->url);
  size_t len = 0;
  unsigned char *d = cache_read(key, 14 * 86400, &len);
  if (!d)
    return 0;
  int fmt = len > ETC_HDR ? d[6] : 0, pw = 0, ph = 0;
  float aspect = 0, hf = -1, corner = 0;
  if (len > ETC_HDR) {
    memcpy(&pw, d + 8, 4);
    memcpy(&ph, d + 12, 4);
    memcpy(&aspect, d + 16, 4);
    memcpy(&hf, d + 20, 4);
    memcpy(&corner, d + 24, 4);
  }
  if (len <= ETC_HDR || memcmp(d, ETC_MAGIC, 6) || (fmt != 1 && fmt != 2) || (fmt == 2 && !(caps & 1)) || pw < 4 ||
      ph < 4 || pw > 4096 || ph > 4096 || len != ETC_HDR + etc_size(pw, ph, fmt == 2) || !(aspect > 0) ||
      !(corner >= 0 && corner < 2000) || (im->fit == 3 && !(hf >= 0 && hf <= 1))) {
    free(d);
    return 0;
  }
  memmove(d, d + ETC_HDR, len - ETC_HDR); /* (the blocks are the picture's pixels now) */
  im->px = d;
  im->fmt = fmt;
  im->pw = pw;
  im->ph = ph;
  im->pch = fmt == 2 ? 4 : 3;
  im->aspect = aspect;
  im->focal = hf; /* already stored in the compressed copy, including older ETC files */
  im->cornerPx = corner; /* (the picture has no corners of its own: the shader rounds them) */
  im->fromSm = 1;
  return 1;
}
static void load(Img *im) {
  im->fromSm = 0;
  im->fmt = 0;
  im->cornerPx = 0;
  dec_enter();
  int ok = etc_load(im) || small_load(im);
  dec_leave();
  if (!ok)
    load_(im);
}
/* the encoder: a bounded list of pictures (the Img, not pixels; the oldest dropped when full), taken newest first */
#define ENC_JOBS 256
static Img *ejob[ENC_JOBS];
static int ejn;
static Img *edone; /* encoded, waiting for the main thread to swap the texture */
static pthread_cond_t ecv = PTHREAD_COND_INITIALIZER;
static int etc_eligible(const Img *im) { /* a raw picture up to 1280x720 that has a small copy */
  return gfx_etc_caps() && !im->fmt && !im->encTried && (!im->urgent || im->fit == 1) && im->pw >= 4 && im->ph >= 4 &&
         im->pw * im->ph >= 64 * 64 && im->pw * im->ph <= 1280 * 720 && strncmp(im->src, "file://", 7);
}
static void enc_queue(Img *im) { /* main thread */
  pthread_mutex_lock(&mu);
  im->encTried = 1;
  if (ejn == ENC_JOBS) {
    ejob[0]->encTried = 0;
    memmove(ejob, ejob + 1, (ENC_JOBS - 1) * sizeof *ejob);
    ejn--;
  }
  ejob[ejn++] = im;
  pthread_cond_signal(&ecv);
  pthread_mutex_unlock(&mu);
}
static int loader_busy(void) { /* under mu: something on screen is loading or waits to go up */
  if (urg)
    return 1;
  for (int i = 0; i < qn; i++)
    if (q_class(pend[i]) >= 3)
      return 1;
  for (int i = 0; i < 8; i++)
    if (busy[i] && q_class(busy[i]) >= 3)
      return 1;
  for (const Img *d = doneHead; d; d = d->dnext)
    if (q_class(d) >= 3)
      return 1;
  return 0;
}
static void enc_run(Img *im) {
  int w = im->pw, h = im->ph, caps = gfx_etc_caps();
  char key[2400];
  snprintf(key, sizeof key, "sm:%dx%d:%s", im->w, im->h, im->url);
  size_t len = 0;
  unsigned char *data = cache_read(key, 14 * 86400, &len);
  if (!data)
    return; /* (no small copy: left as it is) */
  int sw, sh, ch;
  unsigned char *px = stbi_load_from_memory(data, (int)len, &sw, &sh, &ch, 4);
  free(data);
  if (!px || sw != w || sh != h) {
    free(px);
    return;
  }
  /* a picture with no alpha of its own is RGB (half the bytes) and its corners are the shader's; one with alpha (a logo)
     keeps them in its alpha, baked */
  int fmt = etc_alpha(px, w, h) ? 2 : 1;
  float corner = fmt == 1 && im->fit >= 0 ? im->radius : 0;
  if (fmt == 2 && !(caps & 1)) {
    free(px);
    return;
  }
  if (fmt == 2 && im->fit >= 0)
    round_corners(px, w, h, im->radius);
  size_t sz = etc_size(w, h, fmt == 2);
  unsigned char *b = malloc(sz);
  if (!b) {
    free(px);
    return;
  }
  double t0 = mono_s();
  if (fmt == 2)
    etc2_eac_encode(px, w, h, b);
  else
    etc1_encode(px, w, h, b);
  double ms = (mono_s() - t0) * 1000;
  free(px);
  __atomic_fetch_add(&etcN, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&etcUs, (long)(ms * 1000), __ATOMIC_RELAXED);
  if (etcLog < 0)
    etcLog = getenv("SB_UPLOG") != NULL;
  if (etcLog)
    fprintf(stderr, "[etc] %dx%d fmt%d %.1f ms\n", w, h, fmt, ms);
  etc_write(im, fmt, w, h, b, corner);
  pthread_mutex_lock(&mu); /* still shown (as RGBA)? then the main thread swaps it; else the disk copy is all that is kept */
  if (im->tex && !im->texFmt && !im->eb) {
    im->eb = b;
    im->ef = fmt;
    im->ec = corner;
    im->enext = edone;
    edone = im;
    b = NULL;
  }
  pthread_mutex_unlock(&mu);
  free(b);
}
static void *encoder(void *a) {
  (void)a;
  plat_thread_background(15); /* the lowest the app uses */
  for (;;) {
    pthread_mutex_lock(&mu);
    while (!ejn)
      pthread_cond_wait(&ecv, &mu);
    if (loader_busy()) {
      pthread_mutex_unlock(&mu);
      usleep(60000);
      continue;
    }
    Img *im = ejob[--ejn];
    pthread_mutex_unlock(&mu);
    enc_run(im);
  }
  return NULL;
}
static void load_(Img *im) {
  char key[2048];
  snprintf(key, sizeof key, "img:%s", im->src);
  size_t len = 0;
  unsigned char *data = cache_read(key, 14 * 86400, &len);
  int fromNet = 0;
  if (!data && !strncmp(im->src, "file://", 7)) {
    FILE *f = fopen(im->src + 7, "rb");
    if (f) {
      fseek(f, 0, SEEK_END);
      long n = ftell(f);
      fseek(f, 0, SEEK_SET);
      if (n > 0) {
        data = malloc(n);
        len = fread(data, 1, n, f);
      }
      fclose(f);
    }
  } /* a bundled picture */
  else if (!data) {
    char nk[2100], np[1200];
    snprintf(nk, sizeof nk, "neg:%s", im->src);
    if (cache_file(nk, 86400, np, sizeof np)) { /* missing yesterday: not asked again for a day */
      im->gone = 1;
      im->fails++;
      im->failAt = mono_s();
      im->state = 3;
      return;
    }
    int st = 0;
    data = net_fetch_sync(im->src, &len, &st, 20000);
    fromNet = 1;
    if (!data && (st == 404 || st == 410)) {
      cache_write(nk, (const unsigned char *)"1", 1);
      im->gone = 1;
    }
  }
  if (!data || len < 64) {
    fprintf(stderr, "[img] failed %s\n", im->src);
    free(data);
    im->fails++;
    im->failAt = mono_s();
    im->state = 3;
    return;
  }
  dec_enter();
  decode_part(im, data, len, fromNet, key);
  dec_leave();
  if (im->saveLater) { /* the small copy's encoding (tens of ms on the TV) no longer holds a decode slot */
    im->saveLater = 0;
    if (im->px) {
      small_save(im, im->px, im->pw, im->ph);
      if (im->fit >= 0)
        round_corners(im->px, im->pw, im->ph, im->radius);
    }
  }
}
/* the scale a w0 x h0 picture must keep to still cover what it is cut to (1 or more: decode it whole). A crop (fit 3) is
   cut from the picture's 16:9 middle, so it is that window's height that must reach the box's height */
static float cover_scale(const Img *im, int w0, int h0) {
  if (im->w <= 0 || im->h <= 0 || w0 <= 0 || h0 <= 0)
    return 1;
  if (im->fit == 3) {
    float rh = (float)w0 / OPEN_ASPECT < (float)h0 ? (float)w0 / OPEN_ASPECT : (float)h0;
    return (float)im->h / rh;
  }
  float sx = (float)im->w / w0, sy = (float)im->h / h0;
  return sx > sy ? sx : sy;
}
static void decode_part(Img *im, unsigned char *data, size_t len, int fromNet, const char *key) {
  int w, h, ch = 4;
  unsigned char *px = NULL;
  fill_dev_decode_delay();
  if (len > 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WEBP", 4)) { /* metahub serves WebP: decoded straight at the size it is shown */
    int w0 = 0, h0 = 0;
    if (WebPGetInfo(data, len, &w0, &h0) && w0 > 0 && h0 > 0) {
      float sc = cover_scale(im, w0, h0);
      if (sc < 0.9f) {
        WebPDecoderConfig cf;
        if (WebPInitDecoderConfig(&cf)) {
          cf.options.use_scaling = 1;
          cf.options.scaled_width = (int)(w0 * sc + 0.999f);
          cf.options.scaled_height = (int)(h0 * sc + 0.999f);
          cf.output.colorspace = MODE_RGBA;
          if (WebPDecode(data, len, &cf) == VP8_STATUS_OK) {
            px = cf.output.u.RGBA.rgba;
            w = cf.output.width;
            h = cf.output.height;
            cf.output.is_external_memory = 1; /* (ours now: freed with free()) */
          } else
            WebPFreeDecBuffer(&cf.output);
        }
      }
    }
    if (!px)
      px = WebPDecodeRGBA(data, len, &w, &h);
  } else {
    int w0 = 0, h0 = 0, c0;
    if (stbi_info_from_memory(data, (int)len, &w0, &h0, &c0))
      px = jpeg_scaled_rgba(data, len, cover_scale(im, w0, h0), &w, &h); /* (the system's libjpeg at 1/2, 1/4, 1/8 when that covers it) */
    if (!px)
      px = stbi_load_from_memory(data, (int)len, &w, &h, &ch, 4);
  }
  if (px && fromNet && im->fit != 3 && !(im->fit < 0 && im->w * im->h <= 260 * 88))
    cache_write(key, data, len); /* (not the big original behind a card's crop or a card's logo: only what they keep,
                                    saved small below; a full original each would push the cards out of a TV's cache) */
  free(data);
  if (!px) {
    fprintf(stderr, "[img] undecodable %s\n", im->src);
    im->fails++;
    im->failAt = mono_s();
    im->state = 3;
    return;
  }
  if (im->wantPal && !im->palReady) {
    int st = 1;
    while ((w / st) * (h / st) > 40000)
      st++;
    palette_of(px, w, h, im->palAlpha, st, &im->pal);
    im->palReady = 1;
  }
  if (im->fit >= 0) { /* the exact box: crop (or stretch), scale, round the corners */
    float cx = 0, cy = 0, cw = (float)w, ch = (float)h, ba = (float)im->w / im->h, ta = (float)w / h;
    if (im->fit == 3) { /* the card's window of the picture's 16:9 middle, where its subject is */
      float rx = 0, ry = 0, rw = (float)w, rh = (float)h;
      if (ta > OPEN_ASPECT) {
        rw = h * OPEN_ASPECT;
        rx = (w - rw) / 2;
      } else if (ta < OPEN_ASPECT) {
        rh = w / OPEN_ASPECT;
        ry = (h - rh) / 2;
      }
      im->focal = subject_x(px, w, h, rx, ry, rw, rh);
      focal_write(im->src, im->focal);
      cw = rh * ba;
      ch = rh;
      cy = ry;
      cx = rx + img_crop_left(im->focal, rw, cw);
    } else if (im->fit > 0) {
      if (ta > ba) {
        cw = h * ba;
        cx = (w - cw) / 2;
      } else if (ta < ba) {
        ch = w / ba;
        cy = im->fit == 2 ? 0 : (h - ch) / 2;
      }
    }
    unsigned char *out = resample(px, w, h, cx, cy, cw, ch, im->w, im->h);
    free(px);
    if (!out) {
      im->state = 3;
      return;
    }
    im->saveLater = 1; /* its small copy (before the corners: they are put back on load) is made out of the decode slot */
    im->px = out;
    im->pw = im->w;
    im->ph = im->h;
    im->pch = 4;
    im->aspect = ba;
    return;
  }
  /* scale down to the shown size (cover: the smaller of the two scale factors keeps it covering) */
  int tw = im->w, th = im->h;
  float sx = (float)tw / w, sy = (float)th / h, s = sx > sy ? sx : sy;
  if (s < 1.0f) {
    int nw = (int)(w * s + 0.5f), nh = (int)(h * s + 0.5f);
    if (nw < 1)
      nw = 1;
    if (nh < 1)
      nh = 1;
    unsigned char *out = downscale(px, w, h, nw, nh);
    if (out) {
      free(px);
      px = out;
      w = nw;
      h = nh;
    }
  }
  im->saveLater = 1;
  im->px = px;
  im->pw = w;
  im->ph = h;
  im->pch = 4;
  im->aspect = (float)w / h;
}
/* a picture of a Home row the focus reaches in under a second (the eviction's own test): loaded though nobody asked for it
   a while, so the row is there when the focus gets to it. The focus is read atomically (the board sets it each frame) */
static int near_focus(const Img *im) {
  int fr = __atomic_load_n(&focusRow, __ATOMIC_RELAXED);
  unsigned fa = __atomic_load_n(&focusAt, __ATOMIC_RELAXED);
  return fr >= 0 && frameNo - fa < 4 && im->row >= 0 && abs(im->row - fr) <= 3;
}
static void *worker(void *a) {
  plat_thread_background(10);
  int wid = (int)(intptr_t)a;
  net_keep_this_thread();
  for (;;) {
    pthread_mutex_lock(&mu);
    while (!qn && !urg)
      pthread_cond_wait(&cv, &mu);
    Img *im;
    if (urg) {
      im = urg;
      urg = NULL;
    } else
      im = q_pop();
    if (im->state == 1 || im->inDone) {
      pthread_mutex_unlock(&mu);
      continue;
    } /* queued twice (urgent and in a queue): another worker has it already */
    /* asked for, then nobody asked again for 1.5 s (scrolled past): not loaded now; the next ask queues it again */
    if (!im->pin && !im->wantPal && frameNo - im->lastUse > 90 && !near_focus(im)) {
      if (im->shown) {
        im->lost = 3;
        phEv[3]++;
      }
      im->state = 2;
      im->wantTex = 0;
      pthread_mutex_unlock(&mu);
      continue;
    }
    im->state = 1;
    if (wid < 8)
      busy[wid] = im;
    pthread_mutex_unlock(&mu);
    load(im);
    pthread_mutex_lock(&mu);
    if (wid < 8)
      busy[wid] = NULL;
    if (im->state != 3)
      im->state = 2;
    done_push(im);
    pthread_mutex_unlock(&mu);
  }
  return NULL;
}
int img_init(const char *dir) {
  snprintf(cacheDir, sizeof cacheDir, "%s", dir);
  /* 4 cores say little on their own (a 1 GB Firestick has them too): every core but the drawing's only with the memory
   * of a better device */
  int mb = plat_mem_total_mb();
  long nc = sysconf(_SC_NPROCESSORS_ONLN);
  if (nc >= 4 && (mb <= 0 || mb >= 1500))
    decMax = nc > 5 ? 4 : (int)nc - 1;
  int n = mb > 0 && mb < 1200 ? 6 : WORKERS_MAX;
  fprintf(stderr, "[img] %d workers, %d decoding at once\n", n, decMax);
  for (int i = 0; i < n; i++) {
    pthread_t t;
    pthread_create(&t, NULL, worker, (void *)(intptr_t)i);
    pthread_detach(t);
  }
  pthread_t et; /* the one encoder (compressed copies of what was shown) */
  if (!pthread_create(&et, NULL, encoder, NULL))
    pthread_detach(et);
  return 1;
}
static float drawScale = 1.0f;
void img_set_draw_scale(float sc) {
  drawScale = sc > 0 && sc < 1 ? sc : 1.0f;
}
/* a phone (layout units are dp, about 2.6 pixels each): pictures decoded at the pixels they cover, up to 3 per unit */
void img_set_draw_scale_up(float sc) {
  drawScale = sc > 1 ? (sc > 3 ? 3 : sc) : sc > 0 ? sc : 1.0f;
}
void img_set_budget_mb(int mb) {
  budgetBytes = (long)mb * 1024 * 1024;
}
int img_budget_mb(void) {
  return (int)(budgetBytes / (1024 * 1024));
}
void img_queue_stats(int *queued, int *waiting) {
  pthread_mutex_lock(&mu);
  *queued = qn + (urg != NULL);
  *waiting = dn;
  pthread_mutex_unlock(&mu);
}
void img_stats(char *o, size_t cap) {
  unsigned n = __atomic_load_n(&etcN, __ATOMIC_RELAXED);
  long us = __atomic_load_n(&etcUs, __ATOMIC_RELAXED);
  snprintf(o, cap, "evicted %u · shown again %u (faded %u) · etc %u pictures, avg %.1f ms", stEvict, stReturn, stRetFade, n,
           n ? us / 1000.0 / n : 0.0);
}
int img_used_mb(void) {
  return (int)(usedBytes / 1048576);
}
void img_frame(void) {
  frameNo++;
  if (frUp) {
    long kb = frUp / 1024;
    phUpHist[kb / 16 > 128 ? 128 : kb / 16]++;
    phUpFrames++;
    frUp = 0;
  }
}
void img_phase_reset(void) {
  memset(phEv, 0, sizeof phEv);
  memset(phUpHist, 0, sizeof phUpHist);
  phUps = phUpFrames = 0;
  phPeak = usedBytes;
  dnPeak = dnBytes;
}
void img_phase_get(ImgPhase *o) {
  for (int i = 0; i < 5; i++)
    o->ev[i] = phEv[i];
  o->ups = phUps;
  o->upFrames = phUpFrames;
  unsigned want = (unsigned)(phUpFrames * 0.95), acc = 0;
  o->upP95KB = 0;
  for (int i = 0; i < 130; i++) {
    acc += phUpHist[i];
    if (acc > want) {
      o->upP95KB = i * 16;
      break;
    }
  }
  o->picPeakMB = (int)(phPeak / 1048576);
  o->dnPeakMB = (int)(dnPeak / 1048576);
}

/* a full queue leaves the picture waiting (state 5), asked again on its next request, never failed for good */
static void enqueue_at(Img *im, int front) {
  pthread_mutex_lock(&mu);
  im->askAt = mono_s();
  if (qn < 1024) {
    q_push(im, front);
    im->state = 0;
    pthread_cond_signal(&cv);
  } else
    im->state = 5;
  pthread_mutex_unlock(&mu);
}
static void enqueue(Img *im) {
  enqueue_at(im, 0);
}
static int pfMode; /* the caller is looking ahead (img_prefetch_mode): what it asks for is not on screen */
void img_prefetch_mode(int on) {
  pfMode = on;
}
static Img *request_as(const char *key, const char *src, int w, int h, int fit, float radius) {
  unsigned hs = fnv(key);
  Img *im = find(key, hs);
  if (im) {
    im->lastUse = frameNo;
    im->row = ctxRow;
    if (!pfMode)
      im->pfOnly = 0;
    if (im->state == 5)
      enqueue(im);
    if (im->state == 3 && !im->gone && im->fails > 0 && im->fails < 6 && mono_s() - im->failAt > 1.5 * (1 << (im->fails - 1))) {
      fprintf(stderr, "[img] again (%d) %s\n", im->fails, im->src);
      enqueue(im);
    } /* failed (a network blip): asked for again, it tries again after 1.5, 3, 6, 12, 24 s */
    if (!im->wantTex) {
      im->wantTex = 1;
      if (im->state == 2 && !im->tex && !im->px) {
        im->state = 0;
        enqueue(im);
      }
    } /* it was loaded for its colours only: now load it to draw */
    return im;
  }
  if (drawScale != 1) {
    w = (int)(w * drawScale + 0.999f);
    h = (int)(h * drawScale + 0.999f);
    radius *= drawScale;
  }
  im = calloc(1, sizeof *im);
  im->url = strdup(key);
  im->src = key == src ? im->url : strdup(src);
  im->hash = hs;
  im->w = w;
  im->h = h;
  im->fit = fit;
  im->focal = -1;
  im->radius = radius;
  im->lastUse = frameNo;
  im->row = ctxRow;
  im->wantTex = 1;
  im->pfOnly = pfMode;
  im->next = table[hs % BUCKETS];
  table[hs % BUCKETS] = im;
  enqueue(im);
  return im;
}
static Img *request(const char *url, int w, int h) {
  return request_as(url, url, w, h, -1, 0);
}
/* cache keys are built for every label / picture in every frame: printf's float formatting was a tenth of the
   drawing thread's time on the TV */
static char *key_int(char *p, long v) {
  char t[24];
  int n = 0;
  unsigned long u = v < 0 ? (unsigned long)(-v) : (unsigned long)v;
  do {
    t[n++] = (char)('0' + u % 10);
    u /= 10;
  } while (u);
  if (v < 0)
    *p++ = '-';
  while (n)
    *p++ = t[--n];
  return p;
}
static long key_round(float f) {
  return (long)(f >= 0 ? f + 0.5f : f - 0.5f);
}
static const char *shaped_key(char *b, size_t cap, const char *url, int w, int h, float radius, int fit) {
  char *q = b;
  q = key_int(q, w);
  *q++ = 'x';
  q = key_int(q, h);
  *q++ = ':';
  q = key_int(q, fit);
  *q++ = ':';
  q = key_int(q, key_round(radius * 10));
  *q++ = '|';
  size_t n = strlen(url), room = cap - (size_t)(q - b) - 1;
  if (n > room)
    n = room;
  memcpy(q, url, n);
  q[n] = 0;
  return b;
}
GLuint img_get_shaped(const char *url, int w, int h, float radius, int fit) {
  if (!url || !*url || w <= 0 || h <= 0)
    return 0;
  char k[2300];
  Img *im = request_as(shaped_key(k, sizeof k, url, w, h, radius, fit), url, w, h, fit, radius);
  return im->tex;
}
/* a picture that comes back in one step: it was shown before, or its small copy (and a crop's subject place) is on
   disk, a few ms of decode. A "no" is remembered for a second (a stat per card per frame otherwise) */
int img_back_shaped(const char *url, int w, int h, float radius, int fit) {
  if (!url || !*url || w <= 0 || h <= 0)
    return 0;
  char k[2300];
  shaped_key(k, sizeof k, url, w, h, radius, fit);
  unsigned hs = fnv(k);
  Img *im = find(k, hs);
  if (im && (im->shown || im->smOk))
    return 1;
  static struct {
    unsigned h, until;
  } no[128];
  if (no[hs % 128].h == hs && frameNo < no[hs % 128].until)
    return 0;
  int sw = w, sh = h;
  if (drawScale != 1) {
    sw = (int)(w * drawScale + 0.999f);
    sh = (int)(h * drawScale + 0.999f);
  } /* (as request_as sizes it: the small copy's name has the size) */
  char sk[2400];
  int ok = 0; /* (cache_known: the index in memory, never the disk: this runs while drawing, and a stat of a missing file waited seconds on the TV) */
  if (gfx_etc_caps()) { /* (the compressed copy first: it is what is written when ETC is in use) */
    snprintf(sk, sizeof sk, "etc:%dx%d:%s", sw, sh, k);
    ok = cache_known(sk, 14 * 86400);
  }
  if (!ok) {
    snprintf(sk, sizeof sk, "sm:%dx%d:%s", sw, sh, k);
    ok = cache_known(sk, 14 * 86400);
    if (ok && fit == 3) {
      snprintf(sk, sizeof sk, "fx:%s", url);
      ok = cache_known(sk, 14 * 86400);
    }
  }
  if (!ok) {
    no[hs % 128].h = hs;
    no[hs % 128].until = frameNo + 60;
    return 0;
  }
  if (im)
    im->smOk = 1;
  return 1;
}
/* shown before in this session (no disk look-up: a hash find, cheap enough for a row of cards every few frames) */
int img_seen_shaped(const char *url, int w, int h, float radius, int fit) {
  if (!url || !*url)
    return 0;
  char k[2300];
  shaped_key(k, sizeof k, url, w, h, radius, fit);
  Img *im = find(k, fnv(k));
  return im && im->shown;
}
int img_shaped_loaded(const char *url, int w, int h, float radius, int fit) {
  char k[2300];
  shaped_key(k, sizeof k, url, w, h, radius, fit);
  Img *im = find(k, fnv(k));
  return im && im->tex;
}
float img_shaped_focal(const char *url, int w, int h, float radius, int fit) {
  char k[2300];
  shaped_key(k, sizeof k, url, w, h, radius, fit);
  Img *im = find(k, fnv(k));
  return im && im->tex ? im->focal : -1;
}
int img_shaped_failed(const char *url, int w, int h, float radius, int fit) {
  char k[2300];
  shaped_key(k, sizeof k, url, w, h, radius, fit);
  Img *im = find(k, fnv(k));
  return im && im->state == 3;
}
/* why a picture is not on screen now, for the fill test: low 4 bits the stage (0 never asked, 1 queued or decoding, 2 decoded and
   waiting to upload, 3 not asked now, 4 failed, 5 texture there), the rest the cause of a lost texture */
int img_why_shaped(const char *url, int w, int h, float radius, int fit) {
  if (!url || !*url)
    return 0;
  char k[2300];
  shaped_key(k, sizeof k, url, w, h, radius, fit);
  Img *im = find(k, fnv(k));
  if (!im)
    return 0;
  int st = im->tex ? 5 : im->state == 3 ? 4 : (im->px || im->inDone || im == upCur || im->upTex) ? 2 : (im->state == 2 && !im->wantTex) ? 3 : 1;
  return st | (im->lost << 4);
}
GLuint img_get(const char *url, int w, int h, float *aspect) {
  if (!url || !*url)
    return 0;
  Img *im = request(url, w, h);
  if (aspect)
    *aspect = im->aspect > 0 ? im->aspect : (float)w / h;
  return im->tex;
}
GLuint img_get_sized(const char *url, int w, int h, float *aspect) {
  if (!url || !*url)
    return 0;
  char k[2300];
  Img *im = request_as(shaped_key(k, sizeof k, url, w, h, 0, -1), url, w, h, -1, 0);
  if (aspect)
    *aspect = im->aspect > 0 ? im->aspect : (float)w / h;
  return im->tex;
} /* a picture at a size of its own (img_get keeps one size per address: the first asked) */
int img_idle(void) {
  return qn == 0 && !urg;
} /* nothing waits to load: room for what only makes things nicer (a sharp card, its logo) */
int img_has_room(void) {
  return usedBytes < budgetBytes / 100 * 85;
} /* (divided first: times 85 overflows a 32-bit long, and the answer was always no) */
void img_prefetch(const char *url, int w, int h) {
  if (url && *url && img_has_room())
    request(url, w, h);
} /* looking ahead only while there is room: it never pushes out what is on screen */
/* the address a plain picture on the screen was loaded from (the player shows the very logo the page showed) */
const char *img_src_of(GLuint tex) {
  if (!tex)
    return NULL;
  for (int b = 0; b < BUCKETS; b++)
    for (Img *i = table[b]; i; i = i->next)
      if (i->tex == tex && i->fit < 0)
        return i->src;
  return NULL;
}
/* the picture the screen is waiting on (the backdrop of the focused title): out of the stacks into the slot taken
   next, so a fast scroll's posters, asked after it, never hold it back; the one it replaces goes back on top */
static void urgent_move(Img *im) {
  if (im->tex)
    return;
  im->urgent = 1;
  if (im->state != 0 || im == urg)
    return;
  pthread_mutex_lock(&mu);
  for (int i = qn - 1; i >= 0; i--)
    if (pend[i] == im) {
      pend[i] = pend[--qn];
      if (urg)
        pend[qn++] = urg; /* the one it replaces goes back to the list */
      urg = im;
      break;
    } /* (a picture already taken by a worker is not in the list: nothing to do) */
  pthread_mutex_unlock(&mu);
}
void img_urgent(const char *url, int w, int h) {
  if (url && *url)
    urgent_move(request(url, w, h));
}
void img_urgent_shaped(const char *url, int w, int h, float radius, int fit) {
  if (!url || !*url || w <= 0 || h <= 0)
    return;
  char k[2300];
  urgent_move(request_as(shaped_key(k, sizeof k, url, w, h, radius, fit), url, w, h, fit, radius));
}
/* the cards about to reach the open box: out of the plain stack into the first one (once; the last asked goes first),
   and asked every frame, so a scroll never leaves them behind. Not kept like title art: they age out as usual */
void img_ahead_shaped(const char *url, int w, int h, float radius, int fit) {
  if (!url || !*url || w <= 0 || h <= 0)
    return;
  char k[2300];
  Img *im = request_as(shaped_key(k, sizeof k, url, w, h, radius, fit), url, w, h, fit, radius);
  if (im->state == 0)
    im->inPri = 1; /* (class 2 in the queue: after what is on screen, before the plain look-ahead) */
}
/* title art: decoded before the posters waiting in the queue, and kept */
void img_prefetch_pri(const char *url, int w, int h) {
  if (!url || !*url)
    return;
  Img *im = find(url, fnv(url));
  if (!im)
    im = request(url, w, h);
  else
    im->lastUse = frameNo;
  if (im->state == 0)
    im->inPri = 1;
  /* the last 32 kept (a logo at its drawn size is ~360 KB: about 12 MB of the picture budget at most) */
  static Img *pinned[32];
  static int pn;
  if (!im->pin) {
    if (pinned[pn])
      pinned[pn]->pin = 0;
    pinned[pn] = im;
    pn = (pn + 1) % 32;
    im->pin = 1;
  }
}
/* the colours of a picture: 1 when known (out filled), 0 while it loads, -1 when it cannot be loaded */
int img_palette(const char *url, int w, int h, int alpha, Palette *out) {
  if (!url || !*url)
    return -1;
  Img *im = find(url, fnv(url));
  if (!im) {
    im = request(url, w, h);
    im->wantTex = 0;
  } /* only its colours: decoded, never uploaded */
  if (im->state == 3)
    return -1;
  if (im->palReady) {
    *out = im->pal;
    return 1;
  }
  if (!im->wantPal) {
    im->wantPal = 1;
    im->palAlpha = alpha;
    if ((im->state == 2 || im->tex) &&
        !im->px) { /* already decoded without it: load again for the colours (not while its pixels wait to go up) */
      pthread_mutex_lock(&mu);
      if (qn < 1024) {
        q_push(im, 0);
        pthread_cond_signal(&cv);
      }
      pthread_mutex_unlock(&mu);
    }
  }
  return 0;
}
int img_failed(const char *url) {
  Img *im = find(url, fnv(url));
  return im && im->state == 3;
}
int img_loaded(const char *url) {
  Img *im = find(url, fnv(url));
  return im && im->tex;
}

/* uploads a few finished decodes per frame (each upload is a copy into GPU memory) */
/* uploads take about 3/4 MB a frame: the TV's driver rearranges every texel on the CPU as it uploads (a 1280x720
   backdrop in one go was a 25-30 ms frame), so a large picture goes up in bands of rows over a few frames and is
   only shown once complete */
#define UP_BUDGET upBudget /* this TV converts ~100 KB a millisecond for its GPU: ~3 ms a frame (more on a device over 2 GB: see img_init) */
#define UP_URGENT (768L * 1024) /* a backdrop: in a few frames, as before (its fade waits for the whole picture) */
static int upLog = -1; /* SB_UPLOG: each upload's size and time (development) */
static void up_log(const char *what, long bytes, Uint64 t0) {
  if (upLog < 0)
    upLog = getenv("SB_UPLOG") != NULL;
  if (!upLog)
    return;
  double ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
  fprintf(stderr, "[up] %s %ld KB %.1f ms\n", what, bytes / 1024, ms);
}
static void up_done(Img *im) {
  im->tex = im->upTex;
  im->upTex = 0;
  int compress = etc_eligible(im); /* a focused landscape card can be urgent and still compress after upload */
  im->urgent = 0; /* urgency belongs to this upload, not the next visit to the title */
  /* a picture shown before, or one that came from its small copy within 100 ms of the ask, appears at once: a fade
     over the grey placeholder made every return look like a reload */
  int fade = !(im->shown || (im->fromSm && mono_s() - im->askAt < 0.1));
  if (im->shown) {
    stReturn++;
    if (fade)
      stRetFade++;
  }
  im->shown = 1;
  im->lost = 0;
  if (fade)
    gfx_tex_fade_in(im->tex);
  im->bytes = (int)pic_bytes(im);
  im->texFmt = im->fmt;
  if (im->fmt && im->cornerPx > 0)
    gfx_tex_set_corner(im->tex, im->cornerPx);
  usedBytes += im->bytes;
  phUps++;
  if (usedBytes > phPeak)
    phPeak = usedBytes;
  free(im->px);
  im->px = NULL;
  im->lastUse = frameNo;
  if (compress) /* (shown as RGBA: compressed in the background, swapped in when ready) */
    enc_queue(im);
}
static long up_band(Img *im, long budget) { /* rows up to the budget, in whole 16-row blocks; what was sent */
  long row = im->fmt ? (long)((im->pw + 3) / 4) * (im->fmt == 2 ? 16 : 8) : (long)im->pw * im->pch; /* (compressed: a row of blocks, 4 pixel rows) */
  int step = im->fmt ? 4 : 1, rows = (int)(budget / row) * step;
  rows = rows < 16 ? 16 : rows & ~15;
  if (im->upRow + rows > im->ph)
    rows = im->ph - im->upRow;
  long sent = im->fmt ? ((rows + 3) / 4) * row : (long)rows * row;
  Uint64 u0 = SDL_GetPerformanceCounter();
  if (im->fmt)
    gfx_ctex_update(im->upTex, im->fmt, im->px + (size_t)(im->upRow / 4) * row, im->upRow, im->pw, rows);
  else
    gfx_tex_update(im->upTex, im->px + (size_t)im->upRow * row, 0, im->upRow, im->pw, rows, im->pch);
  up_log("band", sent, u0);
  frUp += sent;
  im->upRow += rows;
  if (im->upRow >= im->ph) {
    up_done(im);
    upCur = NULL;
  }
  return sent;
}
/* an encoded picture still shown as RGBA: its texture is swapped for the compressed one (the same picture: no fade), within
   what this frame's uploads have left */
static void enc_apply(void) {
  if (frUp >= upBudget || upCur)
    return;
  pthread_mutex_lock(&mu);
  Img *im = edone;
  if (im)
    edone = im->enext;
  int ok = im && im->tex && !im->texFmt && im->state == 2 && !im->px && !im->inDone && !im->upTex;
  unsigned char *b = im ? im->eb : NULL;
  if (im)
    im->eb = NULL;
  pthread_mutex_unlock(&mu);
  if (!im)
    return;
  GLuint t = ok ? gfx_ctex_create(im->ef, b, im->pw, im->ph) : 0;
  free(b);
  if (!t)
    return;
  long nb = (long)etc_size(im->pw, im->ph, im->ef == 2);
  gfx_tex_recycle(im->tex, im->pw, im->ph, im->pch, 1);
  im->tex = t;
  if (im->ec > 0)
    gfx_tex_set_corner(t, im->ec);
  usedBytes += nb - im->bytes;
  im->bytes = (int)nb;
  im->texFmt = im->ef;
  frUp += nb;
}
static void pump_loads(void);
void img_pump(void) {
  pump_loads();
  enc_apply();
}
static void pump_loads(void) {
  long sent = 0;
  Uint64 t0 = SDL_GetPerformanceCounter();
  if (upCur) {
    if (!upCur->wantTex) {
      if (upCur->shown) {
        upCur->lost = 4;
        phEv[4]++;
      }
      gfx_tex_recycle(upCur->upTex, upCur->pw, upCur->ph, upCur->pch, 1);
      upCur->upTex = 0;
      free(upCur->px);
      upCur->px = NULL;
      upCur->state = 0;
      upCur = NULL;
    } else {
      sent += up_band(upCur, upCur->urgent ? UP_URGENT : UP_BUDGET);
      if (upCur)
        return;
    }
  }
  t0 = SDL_GetPerformanceCounter();
  double hz = (double)SDL_GetPerformanceFrequency();
  for (int n = 0; n < 6 && sent < UP_BUDGET; n++) {
    if (sent && (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / hz > 3.0)
      return; /* first uploads also allocate: time counts too */
    pthread_mutex_lock(&mu);
    Img *im = done_pop();
    pthread_mutex_unlock(&mu);
    if (!im)
      return;
    if (im->px && (im->tex || !im->wantTex)) {
      if (!im->tex && im->shown) {
        im->lost = 4;
        phEv[4]++;
      }
      free(im->px);
      im->px = NULL;
      continue;
    } /* a reload for the colours only, or nobody draws it */
    if (!im->px)
      continue;
    long bytes = pic_bytes(im);
    long cap = im->urgent ? UP_URGENT : UP_BUDGET;
    int wholeOnly = im->fmt == 1 && !(gfx_etc_caps() & 1); /* (ETC1 by the extension has no part upload) */
    if (bytes <= cap - sent || (!sent && (bytes <= cap + cap / 4 || wholeOnly))) {
      sent += bytes;
      Uint64 u0 = SDL_GetPerformanceCounter();
      im->upTex = im->fmt ? gfx_ctex_create(im->fmt, im->px, im->pw, im->ph) : gfx_tex_create(im->px, im->pw, im->ph, im->pch, 1);
      if (!im->upTex) { /* (cannot be made: asked for again when drawn) */
        free(im->px);
        im->px = NULL;
        im->state = 2;
        im->wantTex = 0;
        continue;
      }
      up_log("whole", bytes, u0);
      frUp += bytes;
      up_done(im);
      continue;
    }
    if (sent) {
      pthread_mutex_lock(&mu);
      done_push(im);
      pthread_mutex_unlock(&mu);
      return;
    } /* next frame, first */
    {
      Uint64 u0 = SDL_GetPerformanceCounter();
      im->upTex = im->fmt ? gfx_ctex_create(im->fmt, NULL, im->pw, im->ph) : gfx_tex_create(NULL, im->pw, im->ph, im->pch, 1);
      up_log("alloc", bytes, u0);
    }
    if (!im->upTex) {
      free(im->px);
      im->px = NULL;
      im->state = 2;
      im->wantTex = 0;
      return;
    }
    im->upRow = 0;
    upCur = im;
    up_band(im, cap);
    return;
  }
}
void img_drop_shaped(const char *url, int w, int h, float radius, int fit) {
  if (!url || !*url)
    return;
  char k[2300];
  shaped_key(k, sizeof k, url, w, h, radius, fit);
  Img *i = find(k, fnv(k));
  if (!i || !i->tex || i->pin)
    return;
  gfx_tex_recycle(i->tex, i->pw, i->ph, i->pch, 1);
  i->tex = 0;
  usedBytes -= i->bytes;
  i->bytes = 0;
  i->state = 2;
  i->wantTex = 0; /* (asked again, it comes back from its small copy) */
}
/* the board tells which Home row is being drawn and which has the focus; pictures asked meanwhile belong to that row */
void img_ctx_row(int row) {
  ctxRow = row;
}
void img_focus_row(int row) {
  __atomic_store_n(&focusRow, row, __ATOMIC_RELAXED);
  __atomic_store_n(&focusAt, frameNo, __ATOMIC_RELAXED);
}
/* the first screen kept (rows 0-1 of Home, the billboard): begin, then pin what is loaded; whatever is not pinned again
   in a pass is let go. Never more than a third of the budget, and nothing is kept when the memory guard asks */
void img_pin_begin(void) {
  hgen++;
  hpinBytes = 0;
}
void img_pin_shaped(const char *url, int w, int h, float radius, int fit) {
  if (!url || !*url || w <= 0 || h <= 0)
    return;
  char k[2300];
  shaped_key(k, sizeof k, url, w, h, radius, fit);
  Img *im = find(k, fnv(k));
  if (!im || !im->tex || hpinBytes + im->bytes > budgetBytes / 3)
    return;
  if (im->hpin != hgen)
    hpinBytes += im->bytes;
  im->hpin = hgen;
}
/* over the budget: the picture farthest from the focus goes first (rows 0-1 and the first screen's pins last), then the
   oldest. keepMb > 0 is the memory guard or a film: then nothing is kept */
/* the app is sent to the back or hidden: every picture texture (the title art kept for the next screen excepted) and the
   pool of spare ones go, so the system finds the memory free (they come back as they are drawn, from the disk copies).
   Main thread, once per leaving */
void img_background(void) {
  long before = usedBytes;
  for (int b = 0; b < BUCKETS; b++)
    for (Img *i = table[b]; i; i = i->next) {
      if (!i->tex || i->pin)
        continue;
      gfx_tex_recycle(i->tex, i->pw, i->ph, i->pch, 1);
      i->tex = 0;
      usedBytes -= i->bytes;
      i->bytes = 0;
      i->lost = 2;
      i->state = 2;
      i->wantTex = 0;
    }
  gfx_pool_clear();
  fprintf(stderr, "[img] background: freed %ld MB\n", (before - usedBytes) / 1048576);
}
void img_release_unused(int keepMb) {
  long keep = keepMb > 0 ? (long)keepMb * 1024 * 1024 : budgetBytes;
  if (usedBytes <= keep)
    return;
  int useFocus = focusRow >= 0 && frameNo - focusAt < 4; /* (the board was drawn just now) */
  /* what was on screen in the last 2 s stays (a step back never reloads it); only a real overflow takes more */
  unsigned minAge = usedBytes > keep + keep / 8 ? 3u : 120u;
  for (int n = 0; n < (keepMb > 0 ? 64 : 6) && usedBytes > keep; n++) { /* (a few a frame: a burst of deletions made the TV's memory spike) */
    Img *vic = NULL;
    unsigned best = 0;
    for (int b = 0; b < BUCKETS; b++)
      for (Img *i = table[b]; i; i = i->next) {
        if (!i->tex || i->pin || (!keepMb && i->hpin == hgen) || frameNo - i->lastUse < minAge)
          continue;
        if (!keepMb && useFocus && i->row >= 0 && abs(i->row - focusRow) <= 3)
          continue; /* the rows the focus reaches in under a second stay (the guard still takes what it must) */
        unsigned d = 0;
        if (useFocus)
          d = i->row < 0 ? 2u : i->row <= 1 ? 0u : (unsigned)abs(i->row - focusRow);
        unsigned age = frameNo - i->lastUse; /* (32 bits: the distance above 20 bits of age; 64 broke the ARM compiler) */
        unsigned sc = (d << 20) | (age < 0xfffffu ? age : 0xfffffu);
        if (!vic || sc > best) {
          vic = i;
          best = sc;
        }
      }
    if (!vic)
      return;
    gfx_tex_recycle(vic->tex, vic->pw, vic->ph, vic->pch, 1);
    vic->tex = 0;
    usedBytes -= vic->bytes;
    vic->bytes = 0;
    stEvict++;
    vic->lost = keepMb > 0 ? 2 : 1;
    phEv[vic->lost]++;
    /* loaded again (from its small copy) only when it is asked for again: queueing it at once made a loop under memory
       pressure (decode, upload, evict, decode...) that filled the queue, and a new backdrop could not get in */
    vic->state = 2;
    vic->wantTex = 0;
  }
}
void img_cover_uv(float ta, float ba, int top, float *u0, float *v0, float *u1, float *v1) {
  *u0 = 0;
  *v0 = 0;
  *u1 = 1;
  *v1 = 1;
  if (ta > ba) {
    float k = ba / ta;
    *u0 = (1 - k) / 2;
    *u1 = *u0 + k;
  } /* texture wider than the box: crop the sides */
  else if (ta < ba) {
    float k = ta / ba;
    if (top) {
      *v0 = 0;
      *v1 = k;
    } else {
      *v0 = (1 - k) / 2;
      *v1 = *v0 + k;
    }
  }
}
