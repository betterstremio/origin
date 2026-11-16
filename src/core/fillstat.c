/* Per-card fill metrics for the fast-scroll test (see fillstat.h). Measured on what the draw code actually bound:
   a card counts as final only when its at-rest picture is drawn at full fade. Totals are kept per phase and printed as
   one line (the TV log keeps only 400 lines). Development only. */
#include "core/fillstat.h"
#include "ui/gfx.h"
#include "ui/img.h"
#include "ui/layout.h"
#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define NCARD 8192
#define NBIN 1600 /* 0.25 ms bins, up to 400 ms */
typedef struct {
  unsigned long long key;
  int used, everSeen, inPh, fin, prior, ready;
  unsigned long long seenFrame;
  double firstVis, lastVis, visMs, firstFin;
} Card;
static Card tab[NCARD];
static int on = -1, ph, phActive;
static char phName[48];
static int expRow, expCol;
static double fnow, prevNow, lastKeyT = -1;
static unsigned long long frameNo = 1;
/* the screen's corners in surface pixels (board_draw start) */
static float sx0, sy0, sx1, sy1;
static int refSet;
/* the card in hand */
static int pendArt, pendLogo;
/* the frame in progress */
static int fVis, fFin;
static double vfrLast = 1;
/* global frame periods: the reference period */
static unsigned gh[NBIN + 1];
static unsigned gn;
static double refPer = 16.7;
/* the phase */
static unsigned hist[NBIN + 1];
static unsigned fr, lateN, run, run5, ckbN;
static double sumV, minV, worst;
static double rel300, settleAt;
static int relDone, boxN, boxR;
static float keyLat[4096];
static int nKey;
static int hwmMB;

int fill_on(void) {
  if (on < 0)
    on = (getenv("SB_SCROLL") || getenv("SB_INPUT") || getenv("SB_FILL_ALWAYS")) ? 1 : 0;
  return on;
}
void fill_screen_ref(void) {
  if (!fill_on())
    return;
  sx0 = sy0 = 0;
  sx1 = SB_W;
  sy1 = SB_H;
  gfx_xform_point(&sx0, &sy0);
  gfx_xform_point(&sx1, &sy1);
  refSet = 1;
}
void fill_begin(void) {
  pendArt = 0;
  pendLogo = -1;
}
void fill_art(int final) {
  pendArt = final;
}
void fill_logo(int final) {
  pendLogo = final;
}
static Card *find(unsigned long long key) {
  unsigned h = (unsigned)((key ^ (key >> 29)) * 2654435761u) % NCARD;
  for (int i = 0; i < NCARD; i++) {
    Card *c = &tab[(h + i) % NCARD];
    if (!c->used) {
      memset(c, 0, sizeof *c);
      c->used = 1;
      c->key = key;
      return c;
    }
    if (c->key == key)
      return c;
  }
  return NULL;
}
void fill_card(const void *row, int idx, float x, float y, float w, float h, int needLogo, int skip) {
  if (!fill_on() || !refSet || skip)
    return;
  if (gfx_opacity_now() < 0.95f)
    return;
  float x0 = x, y0 = y, x1 = x + w, y1 = y + h;
  gfx_xform_point(&x0, &y0);
  gfx_xform_point(&x1, &y1);
  if (x0 > x1) {
    float t = x0;
    x0 = x1;
    x1 = t;
  }
  float ix = (x1 < sx1 ? x1 : sx1) - (x0 > sx0 ? x0 : sx0), iy = (y1 < sy1 ? y1 : sy1) - (y0 > sy0 ? y0 : sy0);
  float area = (x1 - x0) * (y1 - y0);
  if (ix <= 0 || iy <= 0 || area <= 0 || ix * iy < 0.5f * area)
    return;
  int fin = pendArt && (!needLogo || pendLogo == 1);
  Card *c = find((unsigned long long)(uintptr_t)row * 1000003ull + (unsigned)idx + 1);
  if (!c || c->seenFrame == frameNo)
    return; /* (drawn twice in a frame: once) */
  c->seenFrame = frameNo;
  fVis++;
  fFin += fin;
  if (!ph) {
    c->everSeen = 1;
    return;
  }
  if (!c->inPh) {
    c->inPh = 1;
    c->prior = c->everSeen;
    c->everSeen = 1;
    c->firstVis = fnow;
    c->lastVis = fnow;
    c->visMs = 0;
    c->fin = 0;
    c->ready = fin;
  }
  c->visMs += fnow - c->lastVis < 100 ? fnow - c->lastVis : 0;
  c->lastVis = fnow;
  if (fin && !c->fin) {
    c->fin = 1;
    c->firstFin = fnow;
  }
}
void fill_box(int ready) {
  if (!fill_on() || !ph)
    return;
  boxN++;
  boxR += ready != 0;
}
void fill_key(double lateMs) {
  if (nKey < 4096)
    keyLat[nKey++] = (float)lateMs;
}
double fill_vfr_now(void) {
  return vfrLast;
}
static double pctl(const unsigned *h, unsigned n, double q) {
  if (!n)
    return 0;
  unsigned want = (unsigned)(n * q), acc = 0;
  for (int i = 0; i <= NBIN; i++) {
    acc += h[i];
    if (acc > want)
      return i * 0.25;
  }
  return NBIN * 0.25;
}
static void add_bin(unsigned *h, double d) {
  int b = (int)(d * 4 + 0.5);
  h[b < 0 ? 0 : b > NBIN ? NBIN : b]++;
}
void fill_tick(void) {
  if (!fill_on())
    return;
  double now = (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency();
  if (prevNow > 0) {
    double d = now - prevNow;
    double vfr = fVis ? (double)fFin / fVis : 1;
    vfrLast = vfr;
    if (gn > 120) /* (the first seconds are the start up) */
      add_bin(gh, d);
    gn++;
    if (gn % 30 == 0 && gn > 150)
      refPer = pctl(gh, gn - 120, 0.5);
    if (ph) {
      if (phActive) {
        fr++;
        add_bin(hist, d);
        if (d > worst)
          worst = d;
        if (d > 1.5 * refPer) {
          lateN++;
          run++;
        } else {
          if (run >= 5)
            run5++;
          run = 0;
        }
        if (fVis) {
          sumV += vfr;
          if (vfr < minV)
            minV = vfr;
          if (fFin < fVis)
            ckbN++;
        } else
          sumV += 1;
      } else {
        if (run >= 5)
          run5++;
        run = 0;
        if (lastKeyT > 0) {
          if (!relDone && now - lastKeyT >= 300) {
            rel300 = vfr;
            relDone = 1;
          }
          if (vfr < 1)
            settleAt = -1;
          else if (settleAt < 0)
            settleAt = now - lastKeyT;
        }
      }
    }
  }
  prevNow = now;
  fnow = now;
  frameNo++;
  fVis = fFin = 0;
}
void fill_phase_begin(const char *name, int er, int ec) {
  if (!fill_on())
    return;
  snprintf(phName, sizeof phName, "%s", name);
  expRow = er;
  expCol = ec;
  for (int i = 0; i < NCARD; i++) {
    tab[i].inPh = 0;
    tab[i].fin = 0;
  }
  memset(hist, 0, sizeof hist);
  fr = lateN = run = run5 = ckbN = 0;
  sumV = 0;
  minV = 1;
  worst = 0;
  rel300 = 1;
  relDone = 0;
  settleAt = -1;
  boxN = boxR = 0;
  nKey = 0;
  lastKeyT = -1;
  ph = 1;
  phActive = 1;
}
void fill_phase_expect(int r, int c) {
  expRow = r;
  expCol = c;
}
void fill_phase_active(int a) {
  if (!ph)
    return;
  phActive = a;
  if (!a) {
    lastKeyT = fnow;
    settleAt = -1;
    relDone = 0;
  }
}
static int hwm_mb(void) {
#ifdef _WIN32
  typedef struct {
    DWORD cb, PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, a, b, c, d, PagefileUsage, PeakPagefileUsage;
  } PMC;
  typedef BOOL(WINAPI * Fn)(HANDLE, PMC *, DWORD);
  static Fn fn;
  if (!fn)
    fn = (Fn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo");
  PMC m;
  memset(&m, 0, sizeof m);
  m.cb = sizeof m;
  if (fn && fn(GetCurrentProcess(), &m, sizeof m))
    return (int)(m.PeakWorkingSetSize >> 20);
  return 0;
#else
  FILE *f = fopen("/proc/self/status", "r");
  char l[200];
  int kb = 0;
  while (f && fgets(l, sizeof l, f))
    if (!strncmp(l, "VmHWM:", 6))
      kb = atoi(l + 6);
  if (f)
    fclose(f);
  return kb / 1024;
#endif
}
static int cmpi(const void *a, const void *b) {
  return *(const int *)a - *(const int *)b;
}
static int cmpf(const void *a, const void *b) {
  float d = *(const float *)a - *(const float *)b;
  return d < 0 ? -1 : d > 0;
}
void fill_phase_end(int row, int col) {
  if (!fill_on() || !ph)
    return;
  static int *tt;
  if (!tt)
    tt = malloc(NCARD * sizeof *tt);
  int n = 0, nt = 0, never = 0, ready = 0, prior = 0, priorHit = 0, mx = 0;
  for (int i = 0; i < NCARD; i++) {
    Card *c = &tab[i];
    if (!c->used || !c->inPh)
      continue;
    n++;
    int t = c->fin ? (int)(c->firstFin - c->firstVis + 0.5) : (int)(c->lastVis - c->firstVis + 0.5);
    if (c->fin && t < 2)
      t = 0;
    if (!c->fin && c->visMs >= 600)
      never++;
    ready += c->ready;
    if (c->prior) {
      prior++;
      priorHit += c->ready;
    }
    tt[nt++] = t;
    if (t > mx)
      mx = t;
  }
  qsort(tt, nt, sizeof *tt, cmpi);
  int p50 = nt ? tt[nt / 2] : 0, i95 = (int)(nt * 0.95), p95 = nt ? tt[i95 < nt ? i95 : nt - 1] : 0;
  qsort(keyLat, nKey, sizeof *keyLat, cmpf);
  int k95 = (int)(nKey * 0.95), kp95 = nKey ? (int)(keyLat[k95 < nKey ? k95 : nKey - 1] + 0.5) : 0;
  char keys[64] = "ok", rt[16] = "n/a";
  if (expRow != -999 && (row != expRow || (expCol != -999 && col != expCol)))
    snprintf(keys, sizeof keys, "MISS:exp=%d,%d:got=%d,%d", expRow, expCol, row, col);
  if (prior)
    snprintf(rt, sizeof rt, "%d%%", (int)(100.0 * priorHit / prior + 0.5));
  int h = hwm_mb();
  if (h > hwmMB)
    hwmMB = h;
  fprintf(stderr,
          "[fill] phase=%s cards=%d vfr=%.2f vfrmin=%.2f ckb=%.1f%% ttf p50=%d p95=%d max=%d ready=%d%% never=%d "
          "box=%d/%d rt=%s rel300=%.2f settle=%d frames n=%u per=%.1f p50=%.1f p95=%.1f p99=%.1f worst=%.0f "
          "late=%.1f%% run5=%u key p95=%d keys=%s hwm=%dMB pic=%dMB\n",
          phName, n, fr ? sumV / fr : 1.0, minV, fr ? 100.0 * ckbN / fr : 0.0, p50, p95, mx,
          n ? (int)(100.0 * ready / n + 0.5) : 100, never, boxR, boxN, rt, rel300, (int)settleAt, fr, refPer,
          pctl(hist, fr, 0.5), pctl(hist, fr, 0.95), pctl(hist, fr, 0.99), worst, fr ? 100.0 * lateN / fr : 0.0, run5,
          kp95, keys, h, img_used_mb());
  ph = 0;
  phActive = 0;
}
/* ---- development switches that make the build slow on purpose (they prove the gate fails) ---- */
void fill_dev_decode_delay(void) {
  static int ms = -1;
  if (ms < 0)
    ms = getenv("SB_DEV_SLOWDEC") ? atoi(getenv("SB_DEV_SLOWDEC")) : 0;
  if (ms > 0)
    SDL_Delay((Uint32)ms);
}
int fill_dev_no_ahead(void) {
  static int v = -1;
  if (v < 0)
    v = getenv("SB_DEV_NOAHEAD") != NULL;
  return v;
}
int fill_dev_budget_pct(void) {
  static int v = -1;
  if (v < 0)
    v = getenv("SB_DEV_BUDGET_PCT") ? atoi(getenv("SB_DEV_BUDGET_PCT")) : 100;
  return v;
}
