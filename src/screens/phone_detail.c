/* A title's page on a phone: its picture across the top, the name (its logo when there is one), the year and length,
   Play (the best stream when that setting is on, else the list), Streams, My List, Trailer, Watched, the story, the
   cast's names, and for a series its seasons and episodes. The page scrolls; a tap on an episode plays it, a long
   press lists its streams. The data is the TV page's (the addons' details, the library, the best pick). */
#include "screens/phone.h"
#include "screens/player.h"
#include "screens/detail.h"
#include "core/form.h"
#include "core/i18n.h"
#include "core/hit.h"
#include "core/json.h"
#include "core/best.h"
#include "core/stremio.h"
#include "ui/gfx.h"
#include "ui/text.h"
#include "ui/icon.h"
#include "ui/img.h"
#include "ui/widgets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAXS 40
typedef struct {
  int n;
  const cJSON **eps;
  int neps;
} Season;
static struct {
  DetailParams p;
  cJSON *meta;
  unsigned gen;
  Item it;
  Season s[MAXS];
  int ns, si;
  int *watched;
  unsigned watchedVer;
  const cJSON *const *vids;
  int nvids;
  Scroller page, seasons;
  int descOpen;
  char cast[400];
} D;
static int is_series(void) {
  if (!strcmp(D.p.it.type, "series"))
    return 1;
  if (!D.meta || !strcmp(D.p.it.type, "movie") || js_str(js_obj(D.meta, "behaviorHints"), "defaultVideoId", NULL))
    return 0;
  const cJSON *v;
  cJSON_ArrayForEach(v, js_arr(D.meta, "videos")) {
    const char *id = js_str(v, "id", "");
    if (*id && strcmp(id, D.p.it.id))
      return 1;
  }
  return 0;
}
static const char *ID(void) {
  return D.p.it.id;
}
static const char *film_vid(void) {
  const char *dv = js_str(js_obj(D.meta, "behaviorHints"), "defaultVideoId", NULL);
  return dv && *dv ? dv : ID();
}
static int cmp_ep(const void *a, const void *b) {
  const cJSON *x = *(const cJSON **)a, *y = *(const cJSON **)b;
  double ex = js_num(x, "episode", js_num(x, "number", 0)), ey = js_num(y, "episode", js_num(y, "number", 0));
  return ex < ey ? -1 : ex > ey;
}
static void seasons_free(void) {
  for (int i = 0; i < D.ns; i++)
    free(D.s[i].eps);
  D.ns = 0;
  D.si = 0;
}
static void build_seasons(void) {
  seasons_free();
  const cJSON *v;
  cJSON_ArrayForEach(v, js_arr(D.meta, "videos")) {
    int sn = (int)js_num(v, "season", 0), k = -1;
    for (int i = 0; i < D.ns; i++)
      if (D.s[i].n == sn) {
        k = i;
        break;
      }
    if (k < 0) {
      if (D.ns >= MAXS)
        continue;
      k = D.ns++;
      D.s[k].n = sn;
      D.s[k].eps = NULL;
      D.s[k].neps = 0;
    }
    Season *s = &D.s[k];
    const cJSON **g = realloc(s->eps, (size_t)(s->neps + 1) * sizeof *g);
    if (!g)
      continue;
    s->eps = g;
    s->eps[s->neps++] = v;
  }
  for (int i = 1; i < D.ns; i++)
    for (int j = i; j > 0; j--) {
      int a = D.s[j - 1].n, b = D.s[j].n;
      if ((a == 0 && b != 0) || (a != 0 && b != 0 && a > b)) {
        Season t = D.s[j];
        D.s[j] = D.s[j - 1];
        D.s[j - 1] = t;
      } else
        break;
    }
  for (int i = 0; i < D.ns; i++)
    qsort(D.s[i].eps, (size_t)D.s[i].neps, sizeof *D.s[i].eps, cmp_ep);
  /* the season being watched */
  const char *cur = js_str(js_obj(lib_get(ID()), "state"), "video_id", "");
  for (int i = 0; i < D.ns && *cur; i++)
    for (int e = 0; e < D.s[i].neps; e++)
      if (!strcmp(js_str(D.s[i].eps[e], "id", ""), cur))
        D.si = i;
}
static int unreleased(const cJSON *v) {
  const char *r = js_str(v, "released", NULL);
  if (!r)
    r = js_str(v, "firstAired", NULL);
  if (!r || strlen(r) < 10)
    return 0;
  struct tm tm;
  memset(&tm, 0, sizeof tm);
  if (sscanf(r, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3)
    return 0;
  tm.tm_year -= 1900;
  tm.tm_mon -= 1;
  return mktime(&tm) > time(NULL);
}
static void watched_check(void) { /* one flag per video, made again when the library moves */
  if (!D.meta || D.watchedVer == lib_version() + 1)
    return;
  D.watchedVer = lib_version() + 1;
  const cJSON *videos = js_arr(D.meta, "videos");
  int n = cJSON_GetArraySize((cJSON *)videos);
  free(D.watched);
  D.watched = n ? calloc((size_t)n, sizeof(int)) : NULL;
  cJSON *lit = lib_get(ID());
  if (lit && D.watched)
    lib_ep_watched_map(lit, videos, D.watched);
}
static int ep_watched(const cJSON *v) {
  if (!D.watched)
    return 0;
  int i = 0;
  const cJSON *x;
  cJSON_ArrayForEach(x, js_arr(D.meta, "videos")) {
    if (x == v)
      return D.watched[i];
    i++;
  }
  return 0;
}
static void make_cast(void) {
  D.cast[0] = 0;
  const cJSON *c = js_arr(D.meta, "cast"), *x;
  int n = 0;
  size_t k = 0;
  if (c)
    cJSON_ArrayForEach(x, c) {
      if (!cJSON_IsString(x) || n >= 5)
        continue;
      k += (size_t)snprintf(D.cast + k, sizeof D.cast - k, "%s%s", n ? ", " : "", x->valuestring);
      n++;
      if (k >= sizeof D.cast)
        break;
    }
}
static void on_meta(cJSON *m, void *ud) {
  if ((unsigned)(uintptr_t)ud != D.gen || SCR != SCR_DETAIL || !m)
    return;
  if (D.meta)
    cJSON_Delete(D.meta);
  D.meta = cJSON_Duplicate(m, 1);
  Item it = D.p.it;
  sb_fill_item(&it, D.meta, D.p.it.type);
  snprintf(it.id, sizeof it.id, "%s", ID());
  D.it = it;
  if (is_series())
    build_seasons();
  D.watchedVer = 0;
  make_cast();
}

/* ---- playing ---- */
static void ep_label(const cJSON *v, char *out, size_t cap) {
  if (!v) {
    snprintf(out, cap, "%s", js_str(D.meta, "releaseInfo", ""));
    return;
  }
  int s = (int)js_num(v, "season", 0), e = (int)js_num(v, "episode", js_num(v, "number", 0));
  const char *n = js_str(v, "name", js_str(v, "title", ""));
  snprintf(out, cap, "S%d · E%d%s%s", s, e, *n ? "  ·  " : "", n);
}
static const cJSON *video_of(const char *vid) {
  const cJSON *v;
  cJSON_ArrayForEach(v, js_arr(D.meta, "videos"))
    if (!strcmp(js_str(v, "id", ""), vid))
      return v;
  return NULL;
}
static void open_list(const char *vid) {
  char lb[300];
  ep_label(is_series() ? video_of(vid) : NULL, lb, sizeof lb);
  phone_streams_show(D.p.it.type, ID(), vid, D.it.name[0] ? D.it.name : D.p.it.name, lb);
}
static void play_vid(const char *vid) {
  const cJSON *v = is_series() ? video_of(vid) : NULL;
  if (v && unreleased(v)) {
    toast(T("This episode has not aired yet"), 2200, NULL);
    return;
  }
  const cJSON *st = js_obj(lib_get(ID()), "state");
  Stream r;
  if (st && js_num(st, "timeOffset", 0) > 60000 && (!is_series() || !strcmp(js_str(st, "video_id", ""), vid)) &&
      sb_resume_stream(ID(), vid, &r)) {
    app_play(&r, D.p.it.type, ID(), vid, 0);
    return;
  }
  if (best_on())
    player_choose_best(D.p.it.type, ID(), vid);
  else
    open_list(vid);
}
/* what Play plays: the film, or the episode being watched, else the first not yet seen */
static int play_target(char *vid, size_t cap) {
  if (!is_series()) {
    snprintf(vid, cap, "%s", film_vid());
    return 1;
  }
  const char *c = js_str(js_obj(lib_get(ID()), "state"), "video_id", NULL);
  if (c && *c && video_of(c)) {
    snprintf(vid, cap, "%s", c);
    return 1;
  }
  watched_check();
  for (int i = 0; i < D.ns; i++) {
    if (!D.s[i].n && D.ns > 1)
      continue;
    for (int e = 0; e < D.s[i].neps; e++)
      if (!ep_watched(D.s[i].eps[e]) && !unreleased(D.s[i].eps[e])) {
        snprintf(vid, cap, "%s", js_str(D.s[i].eps[e], "id", ""));
        return 1;
      }
  }
  if (D.ns && D.s[0].neps) {
    snprintf(vid, cap, "%s", js_str(D.s[0].eps[0], "id", ""));
    return 1;
  }
  return 0;
}
static void play_act(int a, int b) {
  (void)a;
  (void)b;
  char vid[80];
  if (D.meta && play_target(vid, sizeof vid))
    play_vid(vid);
}
static void list_streams_act(int a, int b) {
  (void)a;
  (void)b;
  char vid[80];
  if (D.meta && play_target(vid, sizeof vid))
    open_list(vid);
}
static void mylist_act(int a, int b) {
  (void)a;
  (void)b;
  int had = lib_has(ID());
  lib_toggle(&D.it);
  toast(T(had ? "Removed from your Library" : "Added to your Library"), 1800, NULL);
}
static void trailer_act(int a, int b) {
  (void)a;
  (void)b;
  detail_trailer(D.p.it.type, ID(), D.it.name);
}
static void watched_act(int a, int b) {
  (void)a;
  (void)b;
  if (!D.meta)
    return;
  lib_toggle_watched(&D.it, js_arr(D.meta, "videos"));
  D.watchedVer = 0;
}
static void back_act(int a, int b) {
  (void)a;
  (void)b;
  app_back();
}
static void more_act(int a, int b) {
  (void)a;
  (void)b;
  D.descOpen = !D.descOpen;
}
static void season_act(int i, int b) {
  (void)b;
  if (i >= 0 && i < D.ns)
    D.si = i;
}
static void ep_tap(int s, int e) {
  if (s < D.ns && e < D.s[s].neps)
    play_vid(js_str(D.s[s].eps[e], "id", ""));
}
static void ep_hold(int s, int e) {
  if (s < D.ns && e < D.s[s].neps)
    open_list(js_str(D.s[s].eps[e], "id", ""));
}

static void enter(const void *pp) {
  if (pp)
    D.p = *(const DetailParams *)pp;
  D.gen++;
  if (D.meta)
    cJSON_Delete(D.meta);
  D.meta = NULL;
  seasons_free();
  free(D.watched);
  D.watched = NULL;
  D.watchedVer = 0;
  D.it = D.p.it;
  D.cast[0] = 0;
  D.descOpen = 0;
  scroller_set(&D.page, 0);
  scroller_set(&D.seasons, 0);
  cJSON *mm = sb_meta_cached(D.p.it.type, ID());
  if (mm)
    on_meta(mm, (void *)(uintptr_t)D.gen);
  sb_meta(D.p.it.type, ID(), on_meta, (void *)(uintptr_t)D.gen);
  if (D.p.openStreams) {
    D.p.openStreams = 0;
    if (D.p.it.videoId[0])
      open_list(D.p.it.videoId);
    else {
      D.p.openStreams = 2;
    }
  } /* (Play from a card: once the details say what to play) */
}
static void leave(void) {
  phone_streams_close();
}
static void tick(float dt) {
  scroller_tick(&D.page, dt);
  scroller_tick(&D.seasons, dt);
  watched_check();
  if (D.p.openStreams == 2 && D.meta) {
    D.p.openStreams = 0;
    play_act(0, 0);
  } /* the hero's Play: as Play here */
}

/* ---- drawing ---- */
static float icon_btn(const char *icon, const char *label, float x, float y, float w, HitFn act) {
  int pr = touch_pressed(NULL, act, 0, 0);
  uint32_t c = pr ? WHITE_A(150) : PH_TXT;
  icon_draw(icon, x + w / 2 - 12, y + 4, 24, c);
  text_draw(label, x + w / 2, y + 34, PH_TX_SMALL - 1, 600, pr ? WHITE_A(150) : PH_TXT2, 0, w - 4, ALIGN_CENTER);
  hit_act(x, y - 4, w, 58, act, 0, 0);
  return w;
}
static float episodes_draw(float y, float bottom) {
  if (!D.ns)
    return y;
  /* the seasons: a strip of pills */
  if (D.ns > 1) {
    float x = PH_M - D.seasons.off, x0 = x;
    touch_region(0, y - 4, SB_W, 44, &D.seasons, NULL);
    for (int i = 0; i < D.ns; i++) {
      char t[48];
      if (D.s[i].n)
        snprintf(t, sizeof t, "%s %d", T("Season"), D.s[i].n);
      else
        snprintf(t, sizeof t, "%s", T("Specials"));
      float w = text_width(t, PH_TX_BODY, 700, 0) + 28;
      int on = i == D.si, pr = touch_pressed(NULL, season_act, i, 0);
      if (x + w > 0 && x < SB_W) {
        gfx_rrect(x, y, w, 36, 18, on ? WHITE_A(255) : pr ? WHITE_A(50) : WHITE_A(24));
        text_draw(t, x + w / 2, y + 18 - text_line_height(PH_TX_BODY) / 2.0f, PH_TX_BODY, 700,
                  on ? RGB(12, 11, 17) : PH_TXT, 0, 0, ALIGN_CENTER);
        hit_act(x, y, w, 36, season_act, i, 0);
      }
      x += w + 8;
    }
    scroller_limits(&D.seasons, 0, x - x0 - 8 + 2 * PH_M - SB_W);
    y += 52;
  } else {
    char t[48];
    snprintf(t, sizeof t, "%s", D.s[0].n ? T("Episodes") : T("Specials"));
    text_draw(t, PH_M, y, PH_TX_ROW, 700, PH_TXT, 0, 0, ALIGN_LEFT);
    y += 34;
  }
  Season *s = &D.s[D.si];
  float tw = SB_W >= 600 ? 200 : 136, th = tw * 9 / 16, rowH = th + 16 + 44;
  const char *cur = js_str(js_obj(lib_get(ID()), "state"), "video_id", "");
  for (int e = 0; e < s->neps; e++) {
    const cJSON *v = s->eps[e];
    if (y + rowH > 0 && y < bottom) {
      int pr = touch_pressed(NULL, ep_tap, D.si, e);
      if (pr)
        gfx_rrect(PH_M - 6, y - 6, SB_W - 2 * PH_M + 12, rowH - 4, 10, WHITE_A(14));
      const char *thumb = js_str(v, "thumbnail", "");
      char bb[512];
      if (!*thumb)
        thumb = sb_bg_mid(&D.it, bb, sizeof bb);
      phone_art(thumb, PH_M, y, tw, th, 6);
      if (unreleased(v)) {
        gfx_rrect(PH_M, y, tw, th, 6, BLACK_A(140));
        text_draw(T("Soon"), PH_M + tw / 2, y + th / 2 - 8, PH_TX_SMALL, 700, PH_TXT, 0, 0, ALIGN_CENTER);
      } else {
        gfx_circle(PH_M + tw / 2, y + th / 2, 16, BLACK_A(120));
        icon_draw("play", PH_M + tw / 2 - 10, y + th / 2 - 10, 20, WHITE_A(235));
      }
      if (!strcmp(cur, js_str(v, "id", ""))) {
        double off = js_num(js_obj(lib_get(ID()), "state"), "timeOffset", 0),
               dur = js_num(js_obj(lib_get(ID()), "state"), "duration", 0);
        if (dur > 0) {
          float k = (float)(off / dur);
          if (k > 1)
            k = 1;
          gfx_rrect(PH_M + 6, y + th - 7, tw - 12, 3, 1.5f, WHITE_A(80));
          gfx_rrect(PH_M + 6, y + th - 7, (tw - 12) * k, 3, 1.5f, ACC | 0xff000000u);
        }
      }
      if (ep_watched(v)) {
        gfx_circle(PH_M + tw - 12, y + 12, 9, BLACK_A(170));
        icon_draw("check", PH_M + tw - 19, y + 5, 14, WHITE_A(240));
      }
      char t[300];
      int num = (int)js_num(v, "episode", js_num(v, "number", e + 1));
      snprintf(t, sizeof t, "%d. %s", num, js_str(v, "name", js_str(v, "title", "")));
      float tx = PH_M + tw + 12, tww = SB_W - tx - PH_M;
      text_draw_wrapped(t, tx, y + 2, tww, 2, PH_TX_BODY, 700, PH_TXT, 0, ALIGN_LEFT, 19);
      char rel[32] = "";
      const char *r = js_str(v, "released", js_str(v, "firstAired", ""));
      if (strlen(r) >= 10)
        snprintf(rel, sizeof rel, "%.10s", r);
      if (rel[0])
        text_draw(rel, tx, y + 44, PH_TX_SMALL, 500, PH_TXT2, 0, tww, ALIGN_LEFT);
      const char *ov = js_str(v, "overview", js_str(v, "description", ""));
      if (*ov)
        text_draw_wrapped(ov, PH_M, y + th + 8, SB_W - 2 * PH_M, 2, PH_TX_SMALL + 1, 400, PH_TXT2, 0, ALIGN_LEFT, 18);
      hit_add_hold(PH_M - 6, y - 6, SB_W - 2 * PH_M + 12, rowH - 4, NULL, ep_tap, ep_hold, D.si, e);
    }
    y += *js_str(v, "overview", js_str(v, "description", "")) ? rowH : th + 18;
  }
  return y;
}
static void draw(void) {
  float bottom = SB_H;
  gfx_rect(0, 0, SB_W, SB_H, PAGE);
  touch_region(0, 0, SB_W, bottom, NULL, &D.page);
  float y0 = -D.page.off, y = y0, W = SB_W;
  /* the picture, its lower half fading into the page; it moves at half speed as the page scrolls */
  float ph = W * 9 / 16;
  if (ph > SB_H * 0.45f)
    ph = SB_H * 0.45f;
  float par = D.page.off > 0 ? D.page.off * 0.5f : D.page.off;
  char bb[512];
  const char *bu = sb_bg_large(&D.it, bb, sizeof bb);
  if (!*bu)
    bu = sb_poster_of(&D.it, bb, sizeof bb);
  gfx_clip_push(0, 0, W, y0 + ph > 0 ? y0 + ph : 0);
  phone_art(bu, 0, -par, W, ph, 0);
  gfx_clip_pop();
  gfx_rrect_v(0, y0 + ph * 0.45f, W, ph * 0.55f + 1, 0, PAGE & 0x00ffffffu, PAGE);
  y += ph - 30;
  /* the name: its logo, else its words */
  char lb[512];
  const char *lu = sb_logo_of(&D.it, lb, sizeof lb);
  float asp;
  GLuint lt = lu && *lu ? img_get(lu, 400, 140, &asp) : 0;
  if (lt && asp > 0) {
    float lh = 64, lw = lh * asp;
    if (lw > W * 0.7f) {
      lw = W * 0.7f;
      lh = lw / asp;
    }
    gfx_image(lt, PH_M, y + 64 - lh, lw, lh, 0, WHITE_A(255), 0, 0, 1, 1);
    y += 76;
  } else {
    int n = text_wrap_count(D.it.name, W - 2 * PH_M, 2, PH_TX_H1, 800, -0.4f);
    text_draw_wrapped(D.it.name, PH_M, y, W - 2 * PH_M, 2, PH_TX_H1, 800, PH_TXT, -0.4f, ALIGN_LEFT, 32);
    y += n * 32 + 8;
  }
  if (D.it.metaLine[0]) {
    text_draw(D.it.metaLine, PH_M, y, PH_TX_BODY, 600, PH_TXT2, 0, W - 2 * PH_M, ALIGN_LEFT);
    y += 26;
  }
  /* Play, and the list of streams */
  char vid[80] = "";
  int has = D.meta && play_target(vid, sizeof vid);
  char pl[160];
  const cJSON *st = js_obj(lib_get(ID()), "state");
  int resume = st && js_num(st, "timeOffset", 0) > 60000 && (!is_series() || !strcmp(js_str(st, "video_id", ""), vid));
  if (is_series() && has) {
    const cJSON *v = video_of(vid);
    snprintf(pl, sizeof pl, "%s S%d:E%d", T(resume ? "Resume" : "Play"), (int)js_num(v, "season", 0),
             (int)js_num(v, "episode", js_num(v, "number", 0)));
  } else
    snprintf(pl, sizeof pl, "%s", T(resume ? "Resume" : "Play"));
  float bw = W - 2 * PH_M;
  if (bw > 520)
    bw = 520;
  gfx_opacity_push(has || !D.meta ? 1 : 0.4f);
  phone_button(pl, "play", PH_M, y, bw, 46, 1, play_act, 0, 0);
  gfx_opacity_pop();
  y += 54;
  phone_button(T("Streams"), "p_src", PH_M, y, bw, 42, 0, list_streams_act, 0, 0);
  y += 56;
  /* My List, Trailer, Watched */
  float iw = 84, ix = PH_M - 14;
  ix += icon_btn(lib_has(ID()) ? "check" : "plus", T("My List"), ix, y, iw, mylist_act);
  ix += icon_btn("trailer", T("Trailer"), ix, y, iw, trailer_act);
  icon_btn(D.meta && (is_series() ? lib_series_watched(lib_get(ID()), js_arr(D.meta, "videos"))
                                  : lib_movie_watched(lib_get(ID())))
               ? "eye"
               : "eyeOff",
           T("Watched"), ix, y, iw, watched_act);
  y += 64;
  /* the story (tap for all of it), the cast */
  const char *desc = D.meta ? js_str(D.meta, "description", D.it.description) : D.it.description;
  if (*desc) {
    int lines = D.descOpen ? 20 : 3, n = text_wrap_count(desc, W - 2 * PH_M, lines, PH_TX_BODY, 400, 0);
    text_draw_wrapped(desc, PH_M, y, W - 2 * PH_M, lines, PH_TX_BODY, 400, RGBA(0xf2, 0xf1, 0xf6, 215), 0, ALIGN_LEFT,
                      21);
    hit_act(PH_M, y, W - 2 * PH_M, n * 21, more_act, 0, 0);
    y += n * 21 + 12;
  }
  if (D.cast[0]) {
    char c[480];
    snprintf(c, sizeof c, "%s: %s", T("Cast"), D.cast);
    text_draw_wrapped(c, PH_M, y, W - 2 * PH_M, 2, PH_TX_SMALL + 1, 500, PH_TXT2, 0, ALIGN_LEFT, 19);
    y += 2 * 19 + 14;
  }
  if (is_series()) {
    y += 8;
    y = episodes_draw(y, bottom);
  }
  if (!D.meta) {
    ph_text(PH_M, y, W - 2 * PH_M, 3, PH_TX_BODY, 21, ALIGN_LEFT);
    y += 80;
  }
  scroller_limits(&D.page, 0, y - y0 + 24 - bottom);
  /* Back, over the picture */
  {
    float k = D.page.off / (ph * 0.6f);
    if (k < 0)
      k = 0;
    if (k > 1)
      k = 1;
    if (k > 0) {
      gfx_rect(0, 0, W, PH_TOPBAR, RGBA(12, 11, 17, (int)(255 * k)));
      text_draw(D.it.name, 60, PH_TOPBAR / 2 - text_line_height(PH_TX_ROW) / 2.0f, PH_TX_ROW, 700,
                RGBA(0xf2, 0xf1, 0xf6, (int)(255 * k)), 0, W - 76, ALIGN_LEFT);
      hit_shield(0, 0, W, PH_TOPBAR);
    }
    int pr = touch_pressed(NULL, back_act, 0, 0);
    gfx_circle(PH_M + 18, PH_TOPBAR / 2, 18, pr ? WHITE_A(70) : BLACK_A(110));
    icon_draw("ph_back", PH_M + 6, PH_TOPBAR / 2 - 12, 24, PH_TXT);
    hit_act(0, 0, 64, PH_TOPBAR, back_act, 0, 0);
  }
}
static int key(int k) {
  (void)k;
  return 0;
}
Screen phoneDetail = { enter, leave, key, tick, draw, NULL, NULL, NULL, NULL };
