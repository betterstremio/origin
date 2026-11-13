#include "core/bgtrailer.h"
#include "core/trailer.h"
#include "core/store.h"
#include "core/net.h"
#include "core/best.h"
#include "core/stremio.h"
#include "platform/mem.h"
#include "core/app.h"
#include "core/i18n.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

enum { S_OFF, S_META, S_FIND, S_LOAD, S_SHOW, S_GONE };
static BgtHooks H;
static int st, idleOn;
static unsigned gen, tok;
static double lastCheck, hiddenAt, memAt;
static char tid[64], ttype[16];
/* a clip: the title itself (Settings > For You > Preview on focus), started past the opening credits, silent, 30 s at
   most; none that suits: the trailer instead. Target (SLO): the first picture within 3 s of settling on a card.
   Measured with the user's addons (so/preview-sli.py): finding a link takes 0.3-1.2 s; the slow part is the debrid
   link being made on its first request (0.1-6 s); once made it answers in ~0.2 s. So:
   - the link is looked for the moment a card is settled on (0.35 s) and, once picked, RESOLVED at once in the
     background (its redirects followed, the debrid link made): the TV then loads the final address, which answers fast;
   - the two best links are resolved side by side and the first one ready is used (a slow resolver no longer decides);
   - the card to the right gets the same, once this one is ready, so moving along a row finds the next one done;
   - found and resolved links are kept an hour per title.
   The pick is the clip's own (not the Play button's best-for-this-TV choice): links as each addon answers, a small SDR
   one (720p first, 1080p next, never 4K or HDR / Dolby Vision: the TV would switch its picture mode for a preview),
   done as soon as a 720p one is in or 0.7 s after the first answer. */
/* quick: a short clip or trailer from IMDb (Preview on focus, the default): no addon is asked, its address is looked up
 * ahead (trailer.c), it plays from its first frame with its sound */
#define QUICK_IDLE 0.40f /* the moment the card has opened */
#define QUICK_SECONDS 75
static int quick;
static char qFocus[64];
static double qLoadAt;
static int clip;
static double clipFrac, clipAbs, showAt, settleAt;
#define CLIP_SECONDS 30
#define CLIP_IDLE 1.0f /* a clip starts this long after the remote rests (a trailer: 3 s) */
#define CLIP_IDLE_READY 0.45f /* ... or this long, when its link is resolved already (made ready ahead, or kept) */
#define PREFETCH_IDLE 0.35f /* its link is looked for this long after */
#define CLIP_GRACE 0.7 /* after the first addon's answer, the others this long to bring a better one */
#define RESOLVE_MAX 5.0 /* no resolved address by then: the TV is given the link itself */
typedef struct {
  char id[64], url[2][SB_URL_MAX], addon[96], label[160], final[SB_URL_MAX];
  int score[2], done, resolving, resolved, failed, seen;
  unsigned tok;
  double firstAt, askedAt, pickAt, resolvedAt, frac, abs;
  unsigned g;
} ClipPick;
static ClipPick SL[2]; /* 0: the card in focus; 1: the one to its right, made ready ahead */
#define CP SL[0]
static unsigned pgen;
static struct {
  char id[64], url[SB_URL_MAX], addon[96], label[160];
  int score;
  double at, frac, abs;
} CC[12];
static int ccNext; /* resolved links, an hour each */
#define LIMIT_PAUSE                                                                                                 \
  (30 * 60) /* an addon said its rate limit is reached: this many seconds of IMDb previews, so what is left goes to \
               playing films */
static double limitUntil;
static double now_s(void);
static const char *preview_mode(void) {
  const char *m = store_get("bgPreview", "");
  if (!strcmp(m, "clip")) {
    store_set("bgPreview", "imdb");
    return "imdb";
  } /* (2.0.0's default, saved by anyone who touched the row: on public addons it used up the requests needed to play.
       Chosen again now, it is stored as "title") */
  if (!strcmp(m, "title") && limitUntil > 0 && now_s() < limitUntil)
    return "imdb";
  if (m[0])
    return m;
  return strcmp(store_get("bgTrailer", "true"), "false") ? "imdb" : "off";
}
void bgt_addons_limited(const char *addon, const char *how) {
  double t = now_s();
  int was = limitUntil > 0 && t < limitUntil;
  limitUntil = t + LIMIT_PAUSE;
  if (was || strcmp(store_get("bgPreview", ""), "title"))
    return;
  fprintf(stderr, "[btr] rate limit from %s (%s): addon previews paused for %d min, IMDb instead\n", addon, how,
          LIMIT_PAUSE / 60);
  toast(T("Your addon reached its rate limit. Previews use IMDb for the next 30 minutes"), 5000, NULL);
}
int bgt_clip(void) {
  return clip;
}
int bgt_fast(void) {
  return clip || quick;
}
double bgt_clip_seek(double dur) {
  if (dur <= 0)
    return 0;
  double t = clipAbs > 0 ? clipAbs : dur * clipFrac;
  if (t > dur * 0.5)
    t = dur * 0.3;
  return t;
}
double bgt_clip_start(void) {
  return clipAbs > 0 ? clipAbs : 1080;
} /* (a series: its fixed place; a film: 18 minutes, 15% of a two-hour one) */
/* The TV's pipeline teardown/creation blocks its compositor for 40-60 ms: it may only land when the screen is still (no
   tween moved for ~2 frames) and no key just came; if things keep moving it goes ahead anyway after STILL_WAIT_MAX more
   seconds. */
#define STILL_MIN 0.04 /* about 2 frames without a moving tween */
#define STILL_WAIT_MAX 0.35
static unsigned seenMoves;
static double lastMove, waitFrom, waitLast, swapAt = -9;
static double now_s(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}

void bgt_init(const BgtHooks *h) {
  if (h)
    H = *h;
}
BgtState bgt_state(void) {
  static const BgtState m[] = { BGT_OFF, BGT_WAIT, BGT_WAIT, BGT_LOAD, BGT_SHOW, BGT_GONE };
  return m[st];
}

/* a soft stop: paused and hidden in the same frame; the decoder waits for a pause in the navigation (unloading
   the pipeline in the key handler made the next frame late). The link searches go on: they are for the cards near the
   focus */
static void stop_soft(void) {
  gen++;
  if (tok) {
    trailer_cancel(tok);
    tok = 0;
  }
  if (st == S_LOAD || st == S_SHOW) {
    int was = st == S_SHOW;
    if (H.hide)
      H.hide(!was, H.ud);
    st = S_GONE;
    hiddenAt = now_s() + (was ? 0.46 : 0); /* it was showing: the picture fades back over it first */
  } else if (st == S_META || st == S_FIND)
    st = S_OFF;
}
void bgt_release_now(void) {
  gen++;
  if (tok) {
    trailer_cancel(tok);
    tok = 0;
  }
  if (st == S_LOAD || st == S_SHOW) {
    if (H.hide)
      H.hide(1, H.ud);
  }
  if (st == S_LOAD || st == S_SHOW || st == S_GONE) {
    if (H.unload)
      H.unload(H.ud);
  }
  st = S_OFF;
}
void bgt_key(void) {
  if (idleOn || st != S_OFF) {
    if (st == S_LOAD || st == S_SHOW)
      fprintf(stderr, "[btr] stop: input\n");
    idleOn = 0;
    stop_soft();
  }
}
void bgt_playing(void) {
  if (st != S_LOAD)
    return;
  st = S_SHOW;
  showAt = now_s();
  if (quick && settleAt > 0)
    fprintf(stderr, "[btr] clip sli %.2f s from settling (link picked %.2f, resolved %.2f)\n", showAt - settleAt,
            qLoadAt - settleAt, qLoadAt - settleAt);
  if (clip && settleAt > 0)
    fprintf(stderr, "[btr] clip sli %.2f s from settling (link picked %.2f, resolved %.2f)\n", showAt - settleAt,
            CP.pickAt > 0 ? CP.pickAt - settleAt : -1, CP.resolvedAt > 0 ? CP.resolvedAt - settleAt : -1);
}
void bgt_ended(void) {
  if (st == S_LOAD || st == S_SHOW) {
    fprintf(stderr, "[btr] stop: the video ended or failed\n");
    stop_soft();
  }
}

static void on_found(const Trailer *t, const char *err, void *ud) {
  if ((unsigned)(uintptr_t)ud != gen || st != S_FIND)
    return;
  tok = 0;
  if (!t || !idleOn) {
    st = S_OFF;
    return;
  }
  /* the setting decides, not the free memory (the TV makes room for its own player); the free memory is only logged */
  st = S_LOAD;
  memAt = now_s() + 10;
  fprintf(stderr, "[btr] load %s (%s), %d MB free\n", tid, t->mime, plat_mem_free_mb());
  if (H.load)
    H.load(t->url, t->mime, tid, H.ud);
}
static void on_meta(cJSON *m, void *ud) {
  if ((unsigned)(uintptr_t)ud != gen || st != S_META)
    return;
  char imdb[24], yt[128];
  trailer_imdb_id(m, tid, imdb, sizeof imdb);
  trailer_yt_ids(m, yt, sizeof yt);
  if (!m || !idleOn || (!imdb[0] && !yt[0])) {
    st = S_OFF;
    return;
  }
  st = S_FIND;
  tok = trailer_find_meta(m, tid, 1, on_found, (void *)(uintptr_t)gen);
}
/* a link's worth for a preview: -1 unusable; else higher is better (720p SDR, cached, small) */
static int clip_score(const Stream *s) {
  if (sb_stream_is_limit(s))
    return -1; /* (its link plays the message) */
  if (s->p2p || strncmp(s->url, "http", 4) || s->hdrs[0] || s->ext[0])
    return -1;
  char lb[1100];
  snprintf(lb, sizeof lb, "%s %s %s", s->name, s->title, s->filename);
  char hdr[64];
  best_hdr_of(lb, hdr, sizeof hdr);
  if (hdr[0])
    return -1; /* HDR / Dolby Vision: the TV changes mode */
  const char *r = best_res_of(lb);
  int sc;
  if (!strcmp(r, "4K") || !strcmp(r, "1440p"))
    return -1;
  sc = !strcmp(r, "720p") ? 100 : !strcmp(r, "1080p") ? 80 : !strcmp(r, "SD") ? 55 : 50;
  if (strstr(lb, "\xe2\x8f\xb3") || strstr(lb, "\xe2\x9d\x8c") || strstr(lb, "ncached") || strstr(lb, "ownload"))
    return -1;
  /* (also a cross: not on the debrid service yet, its link plays a "downloading" placeholder) */ /* hourglass,
                                                                                                     "Uncached",
                                                                                                     "Download":
                                                                                                     not ready */
  if (strstr(lb, "\xe2\x9a\xa1") || strstr(lb, "ached"))
    sc += 40; /* (ready at once: above any resolution step) */ /* lightning, "Cached" */
  if (s->size > 0) {
    int gb = (int)(s->size / 1e9);
    sc -= gb > 20 ? 20 : gb;
  }
  return sc;
}
/* a slot's callbacks carry its generation and its number */
static void *slot_ud(int k) {
  return (void *)(uintptr_t)((SL[k].g << 1) | (unsigned)k);
}
static ClipPick *slot_of(void *ud) {
  uintptr_t u = (uintptr_t)ud;
  int k = (int)(u & 1);
  return SL[k].g == (unsigned)(u >> 1) ? &SL[k] : NULL;
}
static void keep_resolved(const ClipPick *P) {
  int k = ccNext++ % 12;
  snprintf(CC[k].id, sizeof CC[k].id, "%s", P->id);
  snprintf(CC[k].url, sizeof CC[k].url, "%s", P->final);
  snprintf(CC[k].addon, sizeof CC[k].addon, "%s", P->addon);
  snprintf(CC[k].label, sizeof CC[k].label, "%s", P->label);
  CC[k].score = P->score[0];
  CC[k].at = now_s();
  CC[k].frac = P->frac;
  CC[k].abs = P->abs;
}
static void on_resolved(int status, const unsigned char *body, size_t len, void *ud) {
  ClipPick *P = slot_of(ud);
  if (!P || P->resolved)
    return;
  if ((status == 200 || status == 206) && body && len > 8 && !strncmp((const char *)body, "http", 4) &&
      len < SB_URL_MAX) {
    snprintf(P->final, sizeof P->final, "%.*s", (int)len, (const char *)body);
    P->resolved = 1;
    P->resolvedAt = now_s();
    keep_resolved(P);
    fprintf(stderr, "[btr] clip link for %s ready in %.2f s (resolved %.2f s after the pick)\n", P->id,
            P->resolvedAt - P->askedAt, P->resolvedAt - P->pickAt);
  } else if (++P->failed >= P->resolving)
    fprintf(stderr, "[btr] clip link for %s: resolving failed (HTTP %d)\n", P->id, status);
}
/* the pick made: its two best links resolved side by side */
static void pick_resolve(ClipPick *P, int k) {
  if (P->resolving || P->score[0] <= 0)
    return;
  P->pickAt = now_s();
  for (int i = 0; i < 2; i++)
    if (P->score[i] > 0 && P->url[i][0]) {
      P->resolving++;
      net_resolve(P->url[i], on_resolved, slot_ud(k));
    }
}
static int pick_ready(const ClipPick *P, double t) {
  return P->done || P->score[0] >= 100 || (P->firstAt && P->score[0] > 0 && t - P->firstAt > CLIP_GRACE) ||
         t - P->askedAt > 6;
}
static void on_clip_part(StreamGroup *g, int gi, void *ud) {
  (void)gi;
  ClipPick *P = slot_of(ud);
  if (!P || P->done || !g)
    return;
  if (!P->firstAt)
    P->firstAt = now_s();
  P->seen += g->n;
  for (int i = 0; i < g->n; i++) {
    const Stream *s = &g->items[i];
    int sc = clip_score(s);
    if (sc <= 0)
      continue;
    if (sc > P->score[0]) {
      P->score[1] = P->score[0];
      memcpy(P->url[1], P->url[0], sizeof P->url[0]);
      P->score[0] = sc;
      snprintf(P->url[0], sizeof P->url[0], "%s", s->url);
      snprintf(P->addon, sizeof P->addon, "%s", s->addon);
      snprintf(P->label, sizeof P->label, "%.150s", s->name);
    } else if (sc > P->score[1] && strcmp(s->url, P->url[0])) {
      P->score[1] = sc;
      snprintf(P->url[1], sizeof P->url[1], "%s", s->url);
    }
  }
}
static void on_clip_done(int total, void *ud) {
  (void)total;
  ClipPick *P = slot_of(ud);
  if (P)
    P->done = 1;
}
/* a card's link: a resolved one kept from before, else its addons asked now (k: which slot) */
static void clip_search(const Item *it, int k) {
  ClipPick *P = &SL[k];
  if (!strcmp(P->id, it->id))
    return;
  if (k == 0 && SL[0].tok && !SL[0].done && strcmp(SL[1].id, it->id))
    sb_streams_cancel(SL[0].tok); /* the card left behind: what it still waits for stops */
  if (k == 1 && SL[1].tok && !SL[1].done)
    sb_streams_cancel(SL[1].tok);
  if (k == 0 && !strcmp(SL[1].id, it->id)) {
    SL[0] = SL[1];
    SL[0].g = ++pgen;
    memset(&SL[1], 0, sizeof SL[1]); /* the one made ready ahead: now in focus */
    if (SL[0].resolving && !SL[0].resolved) {
      SL[0].resolving = 0;
      SL[0].failed = 0;
      pick_resolve(&SL[0], 0);
    }
    return;
  } /* (its resolve answers went to the old slot: asked again, they come back at once) */
  memset(P, 0, sizeof *P);
  snprintf(P->id, sizeof P->id, "%s", it->id);
  P->askedAt = now_s();
  P->g = ++pgen;
  int series = !strcmp(it->type, "series");
  P->abs = series ? 240 : 0;
  P->frac = series ? 0 : 0.15;
  for (int c = 0; c < 12; c++)
    if (!strcmp(CC[c].id, it->id) && P->askedAt - CC[c].at < 3600) {
      snprintf(P->final, sizeof P->final, "%s", CC[c].url);
      snprintf(P->addon, sizeof P->addon, "%s", CC[c].addon);
      snprintf(P->label, sizeof P->label, "%s", CC[c].label);
      P->score[0] = CC[c].score;
      P->done = 1;
      P->resolving = 1;
      P->resolved = 1;
      P->pickAt = P->resolvedAt = P->askedAt;
      return;
    }
  char vid[96];
  CwInfo ci;
  if (series) {
    if (lib_cw_info(it, &ci) && ci.vid[0] && strcmp(ci.vid, it->id))
      snprintf(vid, sizeof vid, "%s", ci.vid);
    else
      snprintf(vid, sizeof vid, "%s:1:1", it->id);
  } else
    snprintf(vid, sizeof vid, "%s", it->id);
  P->tok = sb_streams_bg(it->type, vid, on_clip_part, on_clip_done, slot_ud(k));
  if (!P->tok && !P->done)
    P->done = 1; /* (no addon to ask) */
}
/* plays what the search has, or the trailer when it has nothing */
static void clip_go(double t) {
  if (!idleOn) {
    st = S_OFF;
    return;
  }
  clipAbs = CP.abs;
  clipFrac = CP.frac;
  int resolveDead = CP.resolving && !CP.resolved && CP.failed >= CP.resolving;
  if (CP.score[0] <= 0 || (resolveDead && !CP.url[0][0])) {
    fprintf(stderr, "[btr] clip: no small SDR link for %s (%.1f s, %d links seen), the trailer instead\n", tid,
            t - CP.askedAt, CP.seen);
    clip = 0;
    st = S_META;
    sb_meta(ttype, tid, on_meta, (void *)(uintptr_t)gen);
    return;
  }
  const char *u = CP.resolved ? CP.final : CP.url[0];
  st = S_LOAD;
  memAt = t + 10;
  fprintf(stderr, "[btr] clip %s, %s link from %s: %s, %d MB free\n", tid, CP.resolved ? "resolved" : "unresolved",
          CP.addon, CP.label, plat_mem_free_mb());
  if (H.load)
    H.load(u, "", tid, H.ud);
}
static int imdb_for(const Item *it) {
  return it && !strcmp(preview_mode(), "imdb") && !strncmp(it->id, "tt", 2) &&
         (!strcmp(it->type, "movie") || !strcmp(it->type, "series"));
}
static int clip_for(const Item *it) {
  return it && !strcmp(preview_mode(), "title") && !strncmp(it->id, "tt", 2) &&
         (!strcmp(it->type, "movie") || !strcmp(it->type, "series"));
}
static void try_start(const BgtView *v) {
  if (v->shedding)
    return; /* the TV is struggling: no trailer for now */
  if (v->spotlight)
    return; /* the spotlight moves on by itself */
  if (st != S_OFF || v->blocked || !v->focused || !v->focused->id[0] || !v->focused->type[0])
    return;
  const char *mode = preview_mode();
  if (!strcmp(mode, "off") || getenv("SB_NOBTR"))
    return; /* (SB_NOBTR: development, measuring without it) */
  snprintf(tid, sizeof tid, "%s", v->focused->id);
  snprintf(ttype, sizeof ttype, "%s", v->focused->type);
  quick = 0;
  if (clip_for(v->focused)) {
    clip_search(v->focused, 0);
    clip = 1;
    st = S_FIND;
    gen++;
    return;
  } /* (the link may be ready already: the tick plays it) */
  clip = 0;
  st = S_META;
  sb_meta(ttype, tid, on_meta, (void *)(uintptr_t)++gen);
}
/* the usual moment for a swap has come (what: "unload" / "load"): true when the screen is still, or the cap is hit; the
 * next call within 0.25 s is the same swap */
static int swap_ok(const BgtView *v, double t, const char *what) {
  if (t - swapAt < 0.25)
    return 1;
  if (t - waitLast > 0.1)
    waitFrom = t;
  waitLast = t;
  double still = t - lastMove, waited = t - waitFrom;
  if (still < STILL_MIN && waited < STILL_WAIT_MAX)
    return 0;
  fprintf(stderr, "[btr] swap (%s) +%.2f s after the key, still for %.2f s, waited %.2f s%s\n", what, v->idle, still,
          waited, still < STILL_MIN ? " (cap hit)" : "");
  swapAt = t;
  waitFrom = waitLast = 0;
  return 1;
}
void bgt_tick(const BgtView *v) {
  double t = now_s();
  if (anim_moves != seenMoves) {
    seenMoves = anim_moves;
    lastMove = t;
  }
  if (st == S_GONE && t >= hiddenAt && (v->idle >= 0.42f || t - hiddenAt >= 4) &&
      swap_ok(v, t, "unload")) { /* (0.42: just after a card has finished opening) */
    st = S_OFF;
    if (H.unload)
      H.unload(H.ud);
  }
  int can = v->browsing && v->focused && !v->blocked && !v->spotlight && !v->shedding && !getenv("SB_NOBTR"),
      isClip = can && clip_for(v->focused), isQuick = can && imdb_for(v->focused);
  if (isQuick && v->idle > 0.1f &&
      strcmp(qFocus, v->focused->id)) { /* settled on a new card: its clip and the ones around it, in one request */
    snprintf(qFocus, sizeof qFocus, "%s", v->focused->id);
    settleAt = v->idle > 5 ? t : t - v->idle;
    trailer_clips_want(v->focused->id);
    if (v->next)
      trailer_clips_want(v->next->id);
    for (int i = 0; i < v->nnear; i++)
      if (v->near[i])
        trailer_clips_want(v->near[i]->id);
  }
  if (isQuick && !idleOn && st == S_OFF && v->idle > QUICK_IDLE) {
    char qu[SB_URL_MAX], qn[64];
    int qc = 0;
    if (trailer_clip_ready(v->focused->id, v->hero, qu, sizeof qu, &qc, qn, sizeof qn) > 0 &&
        swap_ok(v, t, "load")) { /* nothing to wait for: it plays now */
      idleOn = 1;
      snprintf(tid, sizeof tid, "%s", v->focused->id);
      snprintf(ttype, sizeof ttype, "%s", v->focused->type);
      clip = 0;
      quick = 1;
      gen++;
      st = S_LOAD;
      memAt = t + 10;
      qLoadAt = t;
      fprintf(stderr, "[btr] %s %s: %s, %d MB free\n", qc ? "imdb clip" : "imdb trailer", tid, qn, plat_mem_free_mb());
      if (H.load)
        H.load(qu, "video/mp4", tid, H.ud);
      return;
    }
  }
  if (!v->browsing)
    for (int k = 0; k < 2; k++)
      if (SL[k].tok && !SL[k].done) {
        sb_streams_cancel(SL[k].tok);
        SL[k].tok = 0;
        SL[k].id[0] = 0;
      } /* a page open: its own requests come first */
  if (idleOn && st == S_OFF && v->focused && tid[0] && strcmp(tid, v->focused->id))
    idleOn = 0; /* the billboard turned by itself: a new title with no key pressed */
  if (isClip && v->idle > PREFETCH_IDLE) {
    if (strcmp(CP.id, v->focused->id))
      settleAt = v->idle > 5 ? t : t - v->idle;
    clip_search(v->focused, 0);
  } /* the link, looked for while the card opens */
  for (int k = 0; k < 2; k++)
    if (SL[k].id[0] && !SL[k].resolving && pick_ready(&SL[k], t))
      pick_resolve(&SL[k], k); /* picked: resolved now */
  if (isClip && CP.resolved && v->next && clip_for(v->next) && v->idle > PREFETCH_IDLE + 0.3f)
    clip_search(v->next, 1); /* then the card to its right */
  /* the clip goes once its link is resolved (or resolving gave up, or took too long) */
  if (clip && st == S_FIND && !strcmp(CP.id, tid) && CP.pickAt > 0 &&
      (CP.resolved || (CP.resolving && CP.failed >= CP.resolving) || t - CP.pickAt > RESOLVE_MAX) &&
      swap_ok(v, t, "load"))
    clip_go(t);
  else if (clip && st == S_FIND && !strcmp(CP.id, tid) && CP.score[0] <= 0 &&
           (CP.done || t - CP.askedAt > 6 || (CP.firstAt && t - CP.firstAt > 1.5)))
    clip_go(t); /* nothing suits: the trailer (1.5 s after the first answer: the addons that answer have answered by
                   then, the rest are the ones that never do) */
  if (clip && st == S_SHOW && showAt && t - showAt > CLIP_SECONDS) {
    fprintf(stderr, "[btr] clip: %d s shown, the card goes back\n", CLIP_SECONDS);
    stop_soft();
  }
  if (quick && st == S_SHOW && showAt && t - showAt > QUICK_SECONDS) {
    fprintf(stderr, "[btr] %d s shown, the card goes back\n", QUICK_SECONDS);
    stop_soft();
  }
  if (isClip && !idleOn && st == S_OFF &&
      (v->idle > CLIP_IDLE || (v->idle > CLIP_IDLE_READY && CP.resolved && !strcmp(CP.id, v->focused->id)))) {
    idleOn = 1;
    try_start(v);
    return;
  } /* a clip: sooner than a trailer, checked every frame (not while the last one's decoder is still going: tried then,
       it would never be tried again until a key) */
  if (t - lastCheck < 1)
    return;
  lastCheck = t;
  int idle = v->idle > 3 && v->browsing && v->focused && !v->blocked;
  if (idle && !idleOn && st == S_OFF) {
    idleOn = 1;
    try_start(v);
  } else if (!idle && idleOn && !(isClip && v->idle > CLIP_IDLE_READY) && !(isQuick && v->idle > QUICK_IDLE)) {
    if (st == S_LOAD || st == S_SHOW)
      fprintf(stderr, "[btr] stop: idle %.1f s, board %d, focused %d, blocked %d\n", v->idle, v->browsing,
              v->focused != NULL, v->blocked);
    idleOn = 0;
    stop_soft();
  }
}
