#include "core/anime_page.h"
#include "core/anime_addon.h"
#include "core/anime_taste.h"
#include "core/i18n.h"
#include "core/json.h"
#include "core/net.h"
#include "core/recs.h"
#include "core/store.h"
#include "platform/plat.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned pageGen, metaVer;
static int okc, failc;
static double openAt;
static ItemList trend, top; /* the billboard's candidates (up to 8 each) */
static int topAsked, topDone;

/* ---- the plan ---- */
static const char *const POP_GENRES[] = { "Action", "Comedy",        "Fantasy",      "Adventure", "Romance", "Drama",
                                          "Sci-Fi", "Slice of Life", "Supernatural", "Mystery",   "Sports",  "Horror" };
#define NPOP ((int)(sizeof POP_GENRES / sizeof *POP_GENRES))
static const char *season_name(int m) {
  return m < 3 ? T("Winter") : m < 6 ? T("Spring") : m < 9 ? T("Summer") : T("Fall");
}
static void add(AnimeRow *o, int *n, int max, const char *title, int src, int al, const char *arg) {
  if (*n >= max)
    return;
  AnimeRow *r = &o[(*n)++];
  memset(r, 0, sizeof *r);
  snprintf(r->title, sizeof r->title, "%s", title);
  r->src = src;
  r->al = al;
  snprintf(r->arg, sizeof r->arg, "%s", arg ? arg : "");
}
static int genre_used(char used[][24], int n, const char *g) {
  for (int i = 0; i < n; i++)
    if (!strcmp(used[i], g))
      return 1;
  return 0;
}
static void add_genre(AnimeRow *o, int *n, int max, const char *genre) {
  static const struct { const char *genre, *title; } titles[] = {
    { "Action", "Popular action anime" }, { "Adventure", "Popular adventure anime" },
    { "Comedy", "Popular comedy anime" }, { "Drama", "Popular drama anime" },
    { "Ecchi", "Popular ecchi anime" }, { "Fantasy", "Popular fantasy anime" },
    { "Horror", "Popular horror anime" }, { "Mahou Shoujo", "Popular mahou shoujo anime" },
    { "Mecha", "Popular mecha anime" }, { "Music", "Popular music anime" },
    { "Mystery", "Popular mystery anime" }, { "Psychological", "Popular psychological anime" },
    { "Romance", "Popular romance anime" }, { "Sci-Fi", "Popular sci-fi anime" },
    { "Slice of Life", "Popular slice of life anime" }, { "Sports", "Popular sports anime" },
    { "Supernatural", "Popular supernatural anime" }, { "Thriller", "Popular thriller anime" },
  };
  for (size_t i = 0; i < sizeof titles / sizeof *titles; i++)
    if (!strcmp(titles[i].genre, genre)) {
      add(o, n, max, T(titles[i].title), AS_AL, AL_ROW_GENRE, genre);
      return;
    }
  add(o, n, max, T1("Popular {genre} anime", "genre", genre), AS_AL, AL_ROW_GENRE, genre);
}
static int anime_page_down(void) {
  return failc >= 2 && okc == 0;
}
int anime_page_plan(AnimeRow *o, int max, int *modeOut) {
  int n = 0, mode;
  int seeds = anime_taste_seeds();
  if (anime_page_down())
    mode = AM_KITSU;
  else
    mode = recs_on() && seeds > 0 ? AM_FORYOU : AM_POPULAR;
  if (modeOut)
    *modeOut = mode;
  if (mode == AM_KITSU) {
    add(o, &n, max, T("Trending now"), AS_KITSU, 0, "kitsu-anime-trending");
    add(o, &n, max, T("Top airing"), AS_KITSU, 0, "kitsu-anime-airing");
    add(o, &n, max, T("Most popular"), AS_KITSU, 0, "kitsu-anime-popular");
    add(o, &n, max, T("Highest rated"), AS_KITSU, 0, "kitsu-anime-rating");
    add(o, &n, max, T("My List: anime"), AS_LIST, 0, "");
    return n;
  }
  int low = plat_mem_total_mb() > 0 && plat_mem_total_mb() <= 1200;
  char gen[8][24];
  int ng = 0;
  if (mode == AM_FORYOU)
    ng = anime_taste_genres(gen, 8);
  for (int i = 0; i < NPOP && ng < 8; i++)
    if (!genre_used(gen, ng, POP_GENRES[i]))
      snprintf(gen[ng++], 24, "%s", POP_GENRES[i]);
  int ids[5];
  char names[5][96];
  int nb = mode == AM_FORYOU ? anime_taste_recent(ids, names, low ? 3 : 5) : 0;
  if (nb > (low ? 3 : 5))
    nb = low ? 3 : 5;
  int bi = 0, gi = 0;
  char t[160], a[56];
  time_t now = time(NULL);
  struct tm *tm = localtime(&now);
  char sn[32];
  snprintf(sn, sizeof sn, "%s %d", season_name(tm->tm_mon), tm->tm_year + 1900);
#define BYW()                                                                                          \
  do {                                                                                                 \
    if (bi < nb) {                                                                                     \
      snprintf(a, sizeof a, "%d", ids[bi]);                                                            \
      add(o, &n, max, T1("Because you watched {name}", "name", names[bi]), AS_AL, AL_ROW_RECS_FOR, a); \
      bi++;                                                                                            \
    }                                                                                                  \
  } while (0)
#define GEN()                           \
  do {                                  \
    if (gi < ng)                        \
      add_genre(o, &n, max, gen[gi++]); \
  } while (0)
  if (mode == AM_FORYOU)
    add(o, &n, max, T("Top picks for you"), AS_TOP, 0, "");
  add(o, &n, max, T("Trending now"), AS_AL, AL_ROW_TRENDING, "");
  add(o, &n, max, T1("New this season \xC2\xB7 {season}", "season", sn), AS_AL, AL_ROW_THIS_SEASON, "");
  BYW();
  GEN();
  add(o, &n, max, T("Top rated anime"), AS_AL, AL_ROW_TOP_RATED, "");
  add(o, &n, max, T("Anime films"), AS_AL, AL_ROW_MOVIES, "");
  GEN();
  BYW();
  add(o, &n, max, T("Coming next season"), AS_AL, AL_ROW_NEXT_SEASON, "");
  if (mode == AM_FORYOU && anime_taste_studio()[0])
    add(o, &n, max, T1("More from {studio}", "studio", anime_taste_studio()), AS_AL, AL_ROW_STUDIO,
        anime_taste_studio());
  GEN();
  BYW();
  add(o, &n, max, T("Complete series to binge"), AS_AL, AL_ROW_FINISHED_BINGE, "");
  add(o, &n, max, T("My List: anime"), AS_LIST, 0, "");
  GEN();
  BYW();
  while (n < max && (gi < ng || bi < nb)) {
    GEN();
    BYW();
  }
#undef BYW
#undef GEN
  return n;
}
unsigned anime_page_sig(void) {
  AnimeRow r[32];
  int mode, n = anime_page_plan(r, 32, &mode);
  unsigned h = (2166136261u ^ (unsigned)mode) + (mode == AM_KITSU && anime_addon_ready());
  for (int i = 0; i < n; i++) {
    for (const char *p = r[i].title; *p; p++)
      h = (h ^ (unsigned char)*p) * 16777619u;
    for (const char *p = r[i].arg; *p; p++)
      h = (h ^ (unsigned char)*p) * 16777619u;
    h = (h ^ (unsigned)(r[i].src * 31 + r[i].al)) * 16777619u;
  }
  return h;
}
void anime_page_open(void) {
  if (!openAt)
    openAt = (double)time(NULL);
  anime_addon_set(1);
  if (recs_on())
    anime_taste_begin();
}
/* ---- asking a row ---- */
typedef struct {
  AlCb cb;
  void *ud;
  int kind;
  unsigned gen;
} Ask;
static void keep8(ItemList *dst, const ItemList *l) {
  itemlist_clear(dst);
  int n = l->n < 8 ? l->n : 8;
  if (n <= 0)
    return;
  dst->items = calloc((size_t)n, sizeof(Item));
  if (!dst->items)
    return;
  memcpy(dst->items, l->items, sizeof(Item) * (size_t)n);
  dst->n = n;
  dst->total = n;
}
static void on_al(ItemList *l, int cached, void *ud) {
  Ask *a = ud;
  if (a->gen == pageGen) {
    if (l->total < 0 && !l->n)
      failc++;
    else {
      okc++;
      if (a->kind == AL_ROW_TRENDING)
        keep8(&trend, l);
    }
  }
  a->cb(l, cached, a->ud);
  free(a);
}
/* Top picks: the lists asked (trending, popular, the heaviest genre) pooled, what the user has is left out, the rest
   ranked by the taste */
typedef struct {
  AlCb cb;
  void *ud;
  int need, got, np;
  unsigned gen;
  Item pool[80];
} TopAsk;
static int pool_rank(const void *a, const void *b) {
  int x = anime_taste_score(atoi(((const Item *)a)->id + 8)), y = anime_taste_score(atoi(((const Item *)b)->id + 8));
  return x < y ? 1 : x > y ? -1 : 0;
}
static void on_top_list(ItemList *l, int cached, void *ud) {
  (void)cached;
  TopAsk *t = ud;
  if (t->gen != pageGen) {
    itemlist_free(l);
    if (++t->got == t->need) {
      t->cb(calloc(1, sizeof(ItemList)), 0, t->ud);
      free(t);
    }
    return;
  }
  for (int i = 0; i < l->n && t->np < 80; i++) {
    const Item *it = &l->items[i];
    int alid = strncmp(it->id, "anilist:", 8) ? 0 : atoi(it->id + 8);
    if (!alid || anime_taste_has(alid))
      continue;
    int dup = 0;
    for (int k = 0; k < t->np && !dup; k++)
      dup = !strcmp(t->pool[k].id, it->id);
    if (!dup)
      t->pool[t->np++] = *it;
  }
  if (l->total < 0 && !l->n)
    failc++;
  else
    okc++;
  itemlist_free(l);
  if (++t->got < t->need)
    return;
  ItemList *out = calloc(1, sizeof *out);
  out->total = t->np ? 1 : -1;
  if (t->np) {
    qsort(t->pool, (size_t)t->np, sizeof t->pool[0], pool_rank);
    int n = t->np < 20 ? t->np : 20;
    out->items = calloc((size_t)n, sizeof(Item));
    if (out->items) {
      memcpy(out->items, t->pool, sizeof(Item) * (size_t)n);
      out->n = n;
      out->total = n;
      keep8(&top, out);
    }
  }
  topDone = 1;
  t->cb(out, 0, t->ud);
  free(t);
}
void anime_page_ask(const AnimeRow *r, AlCb cb, void *ud) {
  if (r->src == AS_LIST) {
    ItemList *l = calloc(1, sizeof *l);
    anime_page_mylist(l);
    l->total = l->n;
    cb(l, 1, ud);
    return;
  }
  if (r->src == AS_TOP) {
    TopAsk *t = calloc(1, sizeof *t);
    t->cb = cb;
    t->ud = ud;
    char g[1][24];
    int genre = anime_taste_genres(g, 1);
    t->need = genre ? 3 : 2;
    t->gen = pageGen;
    topAsked = 1;
    anilist_row(AL_ROW_TRENDING, "", on_top_list, t);
    anilist_row(AL_ROW_POPULAR, "", on_top_list, t);
    if (genre)
      anilist_row(AL_ROW_GENRE, g[0], on_top_list, t);
    return;
  }
  Ask *a = calloc(1, sizeof *a);
  a->cb = cb;
  a->ud = ud;
  a->kind = r->al;
  a->gen = pageGen;
  anilist_row(r->al, r->arg, on_al, a);
}
int anime_page_def(const AnimeRow *r, CatalogDef *d) {
  char base[200];
  snprintf(base, sizeof base, "%s", ANIME_KITSU_URL);
  char *s = strstr(base, "/manifest.json");
  if (s)
    *s = 0;
  return anime_addon_ready() && sb_source_def("community.anime.kitsu", base, "anime", r->arg, "", r->title, d);
}
int anime_page_billboard(ItemList *out) {
  const ItemList *src = NULL;
  int foryou = recs_on() && anime_taste_seeds() > 0;
  if (top.n >= 3)
    src = &top;
  else if (trend.n >= 3 && (!foryou || !topAsked || (topDone && top.n < 3) || anime_page_down()))
    src = &trend;
  else if (trend.n >= 3 && foryou && openAt > 0 && (double)time(NULL) - openAt > 5)
    src = &trend;
  if (!src)
    return 0;
  out->n = 0;
  out->items = calloc((size_t)src->n, sizeof(Item));
  if (!out->items)
    return 0;
  memcpy(out->items, src->items, sizeof(Item) * (size_t)src->n);
  out->n = src->n;
  out->total = src->n;
  return out->n;
}
static double mtime_ms(const cJSON *v) {
  if (cJSON_IsNumber(v))
    return v->valuedouble;
  if (cJSON_IsString(v) && v->valuestring) {
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    int ms = 0;
    if (sscanf(v->valuestring, "%d-%d-%dT%d:%d:%d.%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min,
               &tm.tm_sec, &ms) >= 6) {
      tm.tm_year -= 1900;
      tm.tm_mon -= 1;
      return (double)timegm(&tm) * 1000 + ms;
    }
  }
  return 0;
}
typedef struct {
  const cJSON *it;
  double at;
} Pick;
static int pick_cmp(const void *a, const void *b) {
  double x = ((const Pick *)a)->at, y = ((const Pick *)b)->at;
  return x < y ? 1 : x > y ? -1 : 0;
}
int anime_page_mylist(ItemList *out) {
  cJSON *lib = lib_all();
  int cnt = cJSON_GetArraySize(lib), n = 0;
  Pick *pk = cnt > 0 ? malloc(sizeof(Pick) * (size_t)cnt) : NULL;
  out->items = NULL;
  out->n = 0;
  out->total = 0;
  if (!pk)
    return 0;
  const cJSON *it;
  cJSON_ArrayForEach(it, lib) {
    if (js_bool(it, "removed", 0) || !js_str(it, "name", "")[0] ||
        !lib_id_is_anime(js_str(it, "type", ""), js_str(it, "_id", "")))
      continue;
    pk[n].it = it;
    pk[n++].at = mtime_ms(cJSON_GetObjectItemCaseSensitive(it, "_mtime"));
  }
  if (n) {
    qsort(pk, (size_t)n, sizeof pk[0], pick_cmp);
    out->items = calloc((size_t)n, sizeof(Item));
    for (int i = 0; i < n && out->items; i++) {
      Item *o = &out->items[out->n++];
      const cJSON *l = pk[i].it;
      snprintf(o->id, sizeof o->id, "%s", js_str(l, "_id", ""));
      snprintf(o->type, sizeof o->type, "%s", js_str(l, "type", "series"));
      snprintf(o->name, sizeof o->name, "%s", js_str(l, "name", ""));
      snprintf(o->poster, sizeof o->poster, "%s", js_str(l, "poster", ""));
      snprintf(o->background, sizeof o->background, "%s", js_str(l, "background", ""));
      snprintf(o->logo, sizeof o->logo, "%s", js_str(l, "logo", ""));
    }
  }
  free(pk);
  return out->n;
}

/* ---- the Kitsu meta of a title ---- */
#define NMETA 64
typedef struct {
  char id[64];
  AnimeMeta m;
  int st; /* 0 free, 1 asked, 2 answered */
} MetaEnt;
static MetaEnt meta[NMETA];
static int metaNext;
static MetaEnt *meta_find(const char *id) {
  for (int i = 0; i < NMETA; i++)
    if (meta[i].st && !strcmp(meta[i].id, id))
      return &meta[i];
  return NULL;
}
static MetaEnt *meta_new(const char *id) {
  MetaEnt *e = &meta[metaNext++ % NMETA];
  memset(e, 0, sizeof *e);
  snprintf(e->id, sizeof e->id, "%s", id);
  e->st = 1;
  return e;
}
static void meta_store(const char *id, const char *bg, const char *logo, const char *imdb) {
  MetaEnt *e = meta_find(id);
  if (!e)
    e = meta_new(id);
  snprintf(e->m.bg, sizeof e->m.bg, "%s", bg ? bg : "");
  snprintf(e->m.logo, sizeof e->m.logo, "%s", logo ? logo : "");
  snprintf(e->m.imdb, sizeof e->m.imdb, "%s", imdb ? imdb : "");
  e->st = 2;
  metaVer++;
}
static void meta_key(const char *id, char *k, size_t cap) {
  snprintf(k, cap, "animemeta:%s", id);
}
static void on_meta(cJSON *m, void *ud) {
  char *id = ud;
  if (!m) {
    MetaEnt *e = meta_find(id);
    if (e)
      e->st = 0; /* (asked again later: the addon may not be in yet) */
    free(id);
    return;
  }
  const char *bg = js_str(m, "background", ""), *lg = js_str(m, "logo", "");
  char im[12] = "";
  const cJSON *ir = cJSON_GetObjectItemCaseSensitive(m, "imdbRating");
  if (cJSON_IsNumber(ir))
    snprintf(im, sizeof im, "%.1f", ir->valuedouble);
  else if (cJSON_IsString(ir) && ir->valuestring)
    snprintf(im, sizeof im, "%.10s", ir->valuestring);
  meta_store(id, bg, lg, im);
  cJSON *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "bg", bg);
  cJSON_AddStringToObject(o, "logo", lg);
  cJSON_AddStringToObject(o, "imdb", im);
  char *s = cJSON_PrintUnformatted(o);
  cJSON_Delete(o);
  char k[100];
  meta_key(id, k, sizeof k);
  if (s)
    cache_write(k, (const unsigned char *)s, strlen(s));
  free(s);
  free(id);
}
typedef struct {
  char id[64], type[16];
} DiskAsk;
static void on_meta_disk(int st, const unsigned char *b, size_t n, void *ud) {
  (void)n;
  DiskAsk *d = ud;
  cJSON *j = st == 200 ? net_json(b) : NULL;
  if (cJSON_IsObject(j)) {
    meta_store(d->id, js_str(j, "bg", ""), js_str(j, "logo", ""), js_str(j, "imdb", ""));
    net_json_free(j);
  } else {
    net_json_free(j);
    MetaEnt *e = meta_find(d->id);
    if (e)
      e->st = 0;
  }
  free(d);
}
void anime_meta_ask(const char *type, const char *id) {
  if (!id || !id[0] || !anime_addon_ready() || meta_find(id))
    return;
  meta_new(id);
  char k[100], p[1200];
  meta_key(id, k, sizeof k);
  if (cache_file(k, 7L * 86400L, p, sizeof p)) {
    DiskAsk *d = calloc(1, sizeof *d);
    snprintf(d->id, sizeof d->id, "%s", id);
    snprintf(d->type, sizeof d->type, "%s", type);
    net_file(p, on_meta_disk, d);
    return;
  }
  sb_meta(type && type[0] ? type : "series", id, on_meta, strdup(id));
}
int anime_meta_get(const char *id, AnimeMeta *m) {
  MetaEnt *e = meta_find(id);
  if (!e || e->st != 2)
    return 0;
  if (m)
    *m = e->m;
  return 1;
}
unsigned anime_meta_version(void) {
  return metaVer;
}
int anime_meta_apply(Item *it) {
  AnimeMeta m;
  int ch = 0;
  if (!anime_meta_get(it->id, &m))
    return 0;
  if (m.bg[0] && strcmp(it->background, m.bg)) {
    snprintf(it->background, sizeof it->background, "%s", m.bg);
    ch = 1;
  }
  if (m.logo[0] && strcmp(it->logo, m.logo)) {
    snprintf(it->logo, sizeof it->logo, "%s", m.logo);
    ch = 1;
  }
  AlFacts f;
  if (!strncmp(it->id, "anilist:", 8) && anilist_facts(atoi(it->id + 8), &f)) {
    char ln[96];
    anilist_meta_line(&f, m.imdb[0] ? m.imdb : NULL, ln, sizeof ln);
    if (strcmp(ln, it->metaLine)) {
      snprintf(it->metaLine, sizeof it->metaLine, "%s", ln);
      ch = 1;
    }
  }
  return ch;
}

void anime_page_close(void) {
  pageGen++;
  openAt = 0;
  itemlist_clear(&trend);
  itemlist_clear(&top);
  topAsked = topDone = 0;
  okc = failc = 0;
  memset(meta, 0, sizeof meta);
  metaVer++;
}
