/* AniList rows, ids and taste data (see anilist.h). Main thread only: the network and the disk are reached through
   net_post_hdr_t and net_file, whose workers parse the JSON. */
#include "core/anilist.h"
#include "core/anime_map.h"
#include "core/json.h"
#include "core/net.h"
#include "core/store.h"
#include "core/i18n.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define AL_URL "https://graphql.anilist.co"
#define MAX_FLIGHT 2
#define MAX_PER_MIN 25
#define MAX_QUEUE 16
#define TTL_LIST (12 * 3600)
#define TTL_RECS (7 * 86400)
#define WAIT_429_MS 60000 /* Retry-After is not shown to us: AniList's window is a minute */

/* what a row asks for, kept lean: tags and studios are only for anilist_media_info */
#define MF                                                                                                   \
  "id idMal format episodes seasonYear status title{english romaji} genres averageScore popularity "         \
  "coverImage{extraLarge} bannerImage description(asHtml:false) startDate{year} nextAiringEpisode{episode} " \
  "studios(isMain:true){nodes{name}} trailer{id site}"

enum { JK_ROW, JK_INFO, JK_MAL };

typedef struct Job {
  struct Job *next;
  int kind, ttl, tries, stale;
  char *body;
  unsigned long long hash;
  AlCb cb;
  AlInfoCb icb;
  AlIdCb rcb;
  void *ud;
  char key[32]; /* JK_MAL: the pair to keep besides mal:N */
  int mal;
  unsigned id;
} Job;

static Job *qh, *qt;
static int qn, infl;
static unsigned ids;
static long long blockedUntil;
static long long stamps[MAX_PER_MIN];
static int nstamps;
static int al_requests, al_cache_hits; /* (counters for the dev hook) */

static long long now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* ---- the disk cache: one file per query ---- */
static unsigned long long fnv(const char *s) {
  unsigned long long h = 1469598103934665603ULL;
  for (; *s; s++)
    h = (h ^ (unsigned char)*s) * 1099511628211ULL;
  return h;
}
static void cache_path(const Job *j, char *out, size_t cap) {
  snprintf(out, cap, "%s/al_%016llx.json", store_data_dir(), j->hash);
}
static int cache_age_ok(const Job *j, int anyAge) {
  struct stat st;
  char p[1100];
  cache_path(j, p, sizeof p);
  return stat(p, &st) == 0 && st.st_size > 20 && (anyAge || time(NULL) - st.st_mtime < j->ttl);
}
static void cache_put(const Job *j, const unsigned char *body, size_t len) {
  char p[1100], t[1110];
  cache_path(j, p, sizeof p);
  snprintf(t, sizeof t, "%s.tmp", p);
  FILE *f = fopen(t, "wb");
  if (!f)
    return;
  int ok = fwrite(body, 1, len, f) == len;
  ok = fclose(f) == 0 && ok;
  if (!ok) {
    remove(t);
    return;
  }
  remove(p);
  rename(t, p);
}

/* ---- id pairs, kept for good ---- */
typedef struct {
  char key[24];
  int al;
} Pair;
static Pair *pairs;
static int npairs, cappairs, pairsLoaded;
static void pair_add(const char *key, int al) {
  if (npairs == cappairs)
    pairs = realloc(pairs, sizeof *pairs * (size_t)(cappairs = cappairs ? cappairs * 2 : 256));
  snprintf(pairs[npairs].key, sizeof pairs[npairs].key, "%s", key);
  pairs[npairs++].al = al;
}
static void pairs_path(char *out, size_t cap) {
  snprintf(out, cap, "%s/alids.txt", store_data_dir());
}
static void pairs_load(void) {
  if (pairsLoaded)
    return;
  pairsLoaded = 1;
  char p[1100], line[96];
  pairs_path(p, sizeof p);
  FILE *f = fopen(p, "rb");
  if (!f)
    return;
  while (fgets(line, sizeof line, f)) {
    char k[24];
    int a;
    if (sscanf(line, "%23s %d", k, &a) == 2 && a > 0)
      pair_add(k, a);
  }
  fclose(f);
}
static int pair_get(const char *key) {
  pairs_load();
  for (int i = 0; i < npairs; i++)
    if (!strcmp(pairs[i].key, key))
      return pairs[i].al;
  return 0;
}
static void pair_put(const char *key, int al) {
  if (!key[0] || al <= 0 || pair_get(key) == al)
    return;
  pair_add(key, al);
  char p[1100];
  pairs_path(p, sizeof p);
  FILE *f = fopen(p, "ab");
  if (f) {
    fprintf(f, "%s %d\n", key, al);
    fclose(f);
  }
}

/* ---- items ---- */
/* AniList's text without tags, entities and line breaks, cut to cap-1 bytes on a character edge */
static void clean_text(char *dst, size_t cap, const char *s) {
  static const struct {
    const char *e, *r;
  } T[] = { { "&amp;", "&" }, { "&quot;", "\"" }, { "&#039;", "'" }, { "&#39;", "'" },
            { "&lt;", "<" },  { "&gt;", ">" },    { "&nbsp;", " " }, { "&hellip;", "..." } };
  size_t o = 0;
  int sp = 1, full = 0;
  while (*s) {
    const char *r = NULL;
    char c = *s;
    if (c == '<') {
      int br = (s[1] == 'b' || s[1] == 'B') && (s[2] == 'r' || s[2] == 'R');
      const char *e = strchr(s, '>');
      if (!e)
        break;
      s = e + 1;
      if (!br)
        continue;
      c = ' ';
    } else if (c == '&') {
      for (size_t i = 0; i < sizeof T / sizeof *T; i++)
        if (!strncmp(s, T[i].e, strlen(T[i].e))) {
          r = T[i].r;
          s += strlen(T[i].e);
          break;
        }
      if (!r)
        s++;
    } else
      s++;
    char one[2] = { c, 0 };
    if (!r)
      r = one;
    for (; *r; r++) {
      char d = *r;
      if (d == '\n' || d == '\r' || d == '\t')
        d = ' ';
      if (d == ' ') {
        if (sp)
          continue;
        sp = 1;
      } else
        sp = 0;
      if (o + 1 >= cap) {
        full = 1;
        break;
      }
      dst[o++] = d;
    }
    if (full)
      break;
  }
  if (full) {
    while (o && ((unsigned char)dst[o - 1] & 0xC0) == 0x80)
      o--;
    if (o && ((unsigned char)dst[o - 1] & 0xC0) == 0xC0)
      o--;
  }
  while (o && dst[o - 1] == ' ')
    o--;
  dst[o] = 0;
}
/* what a list answer said about each title, kept for the Anime tab's ranking and details line (a small ring: the
   newest 400 titles seen) */
#define NFACTS 400
static AlFacts *facts;
static int nfacts, factsAt;
static void facts_put(const AlFacts *f) {
  if (!facts && !(facts = calloc(NFACTS, sizeof *facts)))
    return;
  for (int i = 0; i < nfacts; i++)
    if (facts[i].id == f->id) {
      facts[i] = *f;
      return;
    }
  if (nfacts < NFACTS)
    facts[nfacts++] = *f;
  else {
    facts[factsAt] = *f;
    factsAt = (factsAt + 1) % NFACTS;
  }
}
int anilist_facts(int id, AlFacts *out) {
  for (int i = 0; i < nfacts; i++)
    if (facts[i].id == id) {
      if (out)
        *out = facts[i];
      return 1;
    }
  return 0;
}
int anilist_request_count(void) {
  return al_requests;
}
/* "[IMDb 8.9   ]2023   24 episodes   Fantasy   Madhouse" (the segments separated by 0x1f: the app's meta line
   draws each as its own part): never seasons (each entry is one season), never the language */
void anilist_meta_line(const AlFacts *f, const char *imdb, char *out, size_t cap) {
  char ep[48] = "", head[48] = "", n[16], a[16];
  if (f->episodes > 0 && f->aired > 0 && f->aired < f->episodes) {
    snprintf(n, sizeof n, "%d", f->episodes);
    snprintf(a, sizeof a, "%d", f->aired);
    snprintf(ep, sizeof ep, "%s", T2("{a} of {n} episodes out", "a", a, "n", n));
  } else if (f->episodes > 1) {
    snprintf(n, sizeof n, "%d", f->episodes);
    snprintf(ep, sizeof ep, "%s", T1("{n} episodes", "n", n));
  } else if (f->episodes == 1 && !f->movie)
    snprintf(ep, sizeof ep, "%s", T("1 episode"));
  else if (f->episodes <= 0 && f->aired > 1) {
    snprintf(a, sizeof a, "%d", f->aired);
    snprintf(ep, sizeof ep, "%s", T1("{a} episodes out", "a", a));
  }
  size_t o = 0;
  if (imdb && *imdb)
    o += (size_t)snprintf(head + o, sizeof head - o, "IMDb %s", imdb);
  if (f->year)
    o += (size_t)snprintf(head + o, sizeof head - o, "%s%d", o ? "   " : "", f->year);
  size_t w = (size_t)snprintf(out, cap, "%s", head);
  const char *seg[3] = { ep, f->ng ? f->genres[0] : "", f->studio };
  for (int i = 0; i < 3; i++)
    if (seg[i][0] && w + 2 < cap)
      w += (size_t)snprintf(out + w, cap - w, "\x1f%s", seg[i]);
}
static int fill_media(Item *it, const cJSON *m) {
  int id = (int)js_num(m, "id", 0);
  if (id <= 0)
    return 0;
  memset(it, 0, sizeof *it);
  snprintf(it->id, sizeof it->id, "anilist:%d", id);
  const char *fmt = js_str(m, "format", "");
  snprintf(it->type, sizeof it->type, "%s", !strcmp(fmt, "MOVIE") ? "movie" : "series");
  const cJSON *t = js_obj(m, "title");
  const char *nm = js_str(t, "english", NULL);
  if (!nm || !*nm)
    nm = js_str(t, "romaji", "");
  snprintf(it->name, sizeof it->name, "%s", nm);
  snprintf(it->poster, sizeof it->poster, "%s", js_str(js_obj(m, "coverImage"), "extraLarge", ""));
  snprintf(it->background, sizeof it->background, "%s", js_str(m, "bannerImage", ""));
  clean_text(it->description, sizeof it->description, js_str(m, "description", ""));
  AlFacts f;
  memset(&f, 0, sizeof f);
  f.id = id;
  f.movie = !strcmp(fmt, "MOVIE");
  f.year = (int)js_num(js_obj(m, "startDate"), "year", 0);
  if (!f.year)
    f.year = (int)js_num(m, "seasonYear", 0);
  const cJSON *ga = js_arr(m, "genres");
  for (int i = 0; ga && i < 3; i++) {
    const cJSON *e = cJSON_GetArrayItem(ga, i);
    if (cJSON_IsString(e) && e->valuestring)
      snprintf(f.genres[f.ng++], sizeof f.genres[0], "%s", e->valuestring);
  }
  f.score = (int)js_num(m, "averageScore", 0);
  f.popularity = (int)js_num(m, "popularity", 0);
  f.episodes = (int)js_num(m, "episodes", 0);
  const char *st = js_str(m, "status", "");
  snprintf(f.status, sizeof f.status, "%s", st);
  int nx = (int)js_num(js_obj(m, "nextAiringEpisode"), "episode", 0);
  f.aired = !strcmp(st, "RELEASING") ? (nx > 1 ? nx - 1 : 0) : !strcmp(st, "FINISHED") ? f.episodes : 0;
  const cJSON *sn = js_arr(js_obj(m, "studios"), "nodes");
  if (sn && cJSON_GetArraySize(sn))
    snprintf(f.studio, sizeof f.studio, "%s", js_str(cJSON_GetArrayItem(sn, 0), "name", ""));
  const cJSON *tr = js_obj(m, "trailer");
  if (!strcasecmp(js_str(tr, "site", ""), "youtube"))
    snprintf(f.yt, sizeof f.yt, "%s", js_str(tr, "id", ""));
  facts_put(&f);
  anilist_meta_line(&f, NULL, it->metaLine, sizeof it->metaLine);
  return 1;
}
static void collect(ItemList *l, const cJSON *arr, int rec) {
  int n = cJSON_GetArraySize(arr);
  if (n <= 0)
    return;
  l->items = calloc((size_t)n, sizeof *l->items);
  for (int i = 0; i < n; i++) {
    const cJSON *m = cJSON_GetArrayItem(arr, i);
    if (rec)
      m = js_obj(m, "mediaRecommendation");
    if (m && fill_media(&l->items[l->n], m))
      l->n++;
  }
  l->total = l->n;
}
static ItemList *list_of(const cJSON *tree) {
  ItemList *l = calloc(1, sizeof *l);
  l->total = -1;
  const cJSON *d = js_obj(tree, "data");
  const cJSON *pg = js_obj(d, "Page"), *st = js_obj(d, "Studio"), *md = js_obj(d, "Media");
  if (pg)
    collect(l, js_arr(pg, "media"), 0);
  else if (st)
    collect(l, js_arr(js_obj(st, "media"), "nodes"), 0);
  else if (md)
    collect(l, js_arr(js_obj(md, "recommendations"), "nodes"), 1);
  return l;
}
static int info_of(const cJSON *tree, AlInfo *o) {
  const cJSON *m = js_obj(js_obj(tree, "data"), "Media");
  if (!m)
    return 0;
  memset(o, 0, sizeof *o);
  o->id = (int)js_num(m, "id", 0);
  snprintf(o->format, sizeof o->format, "%s", js_str(m, "format", ""));
  const cJSON *e;
  cJSON_ArrayForEach(e, js_arr(m, "genres")) {
    if (cJSON_IsString(e) && o->ngenres < 8)
      snprintf(o->genres[o->ngenres++], sizeof o->genres[0], "%s", e->valuestring);
  }
  cJSON_ArrayForEach(e, js_arr(m, "tags")) {
    if (js_num(e, "rank", 0) >= 60 && o->ntags < 12)
      snprintf(o->tags[o->ntags++], sizeof o->tags[0], "%s", js_str(e, "name", ""));
  }
  const cJSON *s0 = cJSON_GetArrayItem(js_arr(js_obj(m, "studios"), "nodes"), 0);
  snprintf(o->studio, sizeof o->studio, "%s", js_str(s0, "name", ""));
  return o->id > 0;
}

/* ---- jobs ---- */
static void job_free(Job *j) {
  free(j->body);
  free(j);
}
static void job_fail(Job *j) {
  if (j->kind == JK_ROW) {
    ItemList *l = calloc(1, sizeof *l);
    l->total = -1;
    j->cb(l, 0, j->ud);
  } else if (j->kind == JK_INFO)
    j->icb(NULL, 0, j->ud);
  else
    j->rcb(0, j->ud);
  job_free(j);
}
/* a parsed answer: 1 when it was good (the job is finished and the tree freed), 0 when it was no answer */
static int job_done(Job *j, cJSON *tree, int cached) {
  if (!tree || !js_obj(tree, "data") || cJSON_GetObjectItemCaseSensitive(tree, "errors"))
    return 0;
  if (j->kind == JK_ROW) {
    j->cb(list_of(tree), cached, j->ud);
  } else if (j->kind == JK_INFO) {
    AlInfo in;
    if (!info_of(tree, &in))
      return 0;
    j->icb(&in, cached, j->ud);
  } else {
    int al = (int)js_num(js_obj(js_obj(tree, "data"), "Media"), "id", 0);
    if (al > 0) {
      char k[24];
      snprintf(k, sizeof k, "mal:%d", j->mal);
      pair_put(k, al);
      pair_put(j->key, al);
    }
    j->rcb(al, j->ud);
  }
  net_json_free(tree);
  job_free(j);
  return 1;
}
static void enqueue(Job *j) {
  j->next = NULL;
  if (qt)
    qt->next = j;
  else
    qh = j;
  qt = j;
  qn++;
  while (qn > MAX_QUEUE) { /* nobody can wait for all of them: the oldest goes */
    Job *d = qh;
    qh = d->next;
    if (!qh)
      qt = NULL;
    qn--;
    job_fail(d);
  }
}
static int window_ok(long long now) {
  int k = 0;
  for (int i = 0; i < nstamps; i++)
    if (now - stamps[i] < 60000)
      stamps[k++] = stamps[i];
  nstamps = k;
  return nstamps < MAX_PER_MIN;
}

static void on_file(int status, const unsigned char *body, size_t len, void *ud);
static void on_net(int status, const unsigned char *body, size_t len, void *ud) {
  Job *j = ud;
  infl--;
  if (status == 429 && j->tries++ == 0) { /* once more after the wait */
    blockedUntil = now_ms() + WAIT_429_MS;
    j->next = qh;
    qh = j;
    if (!qt)
      qt = j;
    qn++;
    fprintf(stderr, "[anilist] 429: waiting %d s\n", WAIT_429_MS / 1000);
    return;
  }
  if (status == 200) {
    cJSON *tree = net_json(body);
    if (tree && js_obj(tree, "data") && !cJSON_GetObjectItemCaseSensitive(tree, "errors")) {
      if (j->ttl > 0)
        cache_put(j, body, len);
      job_done(j, tree, 0);
      return;
    }
    if (tree)
      net_json_free(tree);
  }
  if (j->ttl > 0 && !j->stale && cache_age_ok(j, 1)) { /* the network failed: an old answer beats none */
    char p[1100];
    j->stale = 1;
    cache_path(j, p, sizeof p);
    net_file(p, on_file, j);
    return;
  }
  job_fail(j);
}
static void on_file(int status, const unsigned char *body, size_t len, void *ud) {
  Job *j = ud;
  (void)len;
  cJSON *tree = status == 200 ? net_json(body) : NULL;
  if (tree && job_done(j, tree, 1)) {
    al_cache_hits++;
    return;
  }
  if (tree)
    net_json_free(tree);
  if (j->stale)
    job_fail(j);
  else
    enqueue(j); /* an unreadable cache file: ask again */
  /* (the tick sends it) */
}
static void pump(void) {
  long long now = now_ms();
  while (qh && infl < MAX_FLIGHT && now >= blockedUntil && window_ok(now)) {
    Job *j = qh;
    qh = j->next;
    if (!qh)
      qt = NULL;
    qn--;
    infl++;
    if (getenv("SB_ANILIST_OFF")) { /* development: AniList cannot be reached (the Kitsu fallback, the empty page) */
      on_net(0, NULL, 0, j);
      continue;
    }
    al_requests++;
    stamps[nstamps++] = now;
    net_post_hdr_t(AL_URL, j->body, "application/json", "Accept: application/json", 20000, on_net, j);
  }
}
static unsigned submit(Job *j, const char *query, const char *vars) {
  size_t n = strlen(query) + strlen(vars) + 40;
  j->body = malloc(n);
  snprintf(j->body, n, "{\"query\":\"%s\",\"variables\":%s}", query, vars);
  j->hash = fnv(j->body);
  j->id = ++ids;
  unsigned id = j->id;
  if (j->ttl > 0 && cache_age_ok(j, 0)) {
    char p[1100];
    cache_path(j, p, sizeof p);
    net_file(p, on_file, j);
  } else {
    enqueue(j);
    pump();
  }
  return id;
}
static char *jstr(const char *s) { /* a JSON string with its quotes */
  cJSON *c = cJSON_CreateString(s ? s : "");
  char *o = cJSON_PrintUnformatted(c);
  cJSON_Delete(c);
  return o;
}

unsigned anilist_row(int kind, const char *arg, AlCb cb, void *ud) {
  static const char *SEAS[] = { "WINTER", "SPRING", "SUMMER", "FALL" };
  char q[900], v[300] = "{}";
  const char *page = "Page(perPage:50){media(type:ANIME,";
  const char *tail = ",isAdult:false){" MF "}}";
  int ttl = TTL_LIST;
  if (!cb)
    return 0;
  switch (kind) {
  case AL_ROW_TRENDING:
    snprintf(q, sizeof q, "{%ssort:TRENDING_DESC%s}", page, tail);
    break;
  case AL_ROW_THIS_SEASON: {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    snprintf(q, sizeof q, "{%sseason:%s,seasonYear:%d,sort:POPULARITY_DESC%s}", page, SEAS[tm->tm_mon / 3],
             tm->tm_year + 1900, tail);
    break;
  }
  case AL_ROW_NEXT_SEASON:
    snprintf(q, sizeof q, "{%sstatus:NOT_YET_RELEASED,sort:POPULARITY_DESC%s}", page, tail);
    break;
  case AL_ROW_POPULAR:
    snprintf(q, sizeof q, "{%ssort:POPULARITY_DESC%s}", page, tail);
    break;
  case AL_ROW_TOP_RATED:
    snprintf(q, sizeof q, "{%ssort:SCORE_DESC%s}", page, tail);
    break;
  case AL_ROW_MOVIES:
    snprintf(q, sizeof q, "{%sformat:MOVIE,sort:POPULARITY_DESC%s}", page, tail);
    break;
  case AL_ROW_FINISHED_BINGE:
    snprintf(q, sizeof q, "{%sstatus:FINISHED,episodes_greater:23,sort:SCORE_DESC%s}", page, tail);
    break;
  case AL_ROW_GENRE: {
    char *s = jstr(arg);
    snprintf(q, sizeof q, "query($g:[String]){%sgenre_in:$g,sort:POPULARITY_DESC%s}", page, tail);
    snprintf(v, sizeof v, "{\"g\":[%s]}", s);
    free(s);
    break;
  }
  case AL_ROW_STUDIO: {
    char *s = jstr(arg);
    snprintf(q, sizeof q,
             "query($s:String){Studio(search:$s){media(perPage:50,sort:POPULARITY_DESC,isMain:true){nodes{" MF "}}}}");
    snprintf(v, sizeof v, "{\"s\":%s}", s);
    free(s);
    break;
  }
  case AL_ROW_RECS_FOR:
    snprintf(q, sizeof q,
             "query($id:Int){Media(id:$id,type:ANIME){recommendations(perPage:20,sort:RATING_DESC){nodes{"
             "mediaRecommendation{" MF "}}}}}");
    snprintf(v, sizeof v, "{\"id\":%d}", arg ? atoi(arg) : 0);
    ttl = TTL_RECS;
    break;
  default:
    return 0;
  }
  Job *j = calloc(1, sizeof *j);
  j->kind = JK_ROW;
  j->ttl = ttl;
  j->cb = cb;
  j->ud = ud;
  return submit(j, q, v);
}

unsigned anilist_media_info(int id, AlInfoCb cb, void *ud) {
  char v[40];
  if (!cb)
    return 0;
  snprintf(v, sizeof v, "{\"id\":%d}", id);
  Job *j = calloc(1, sizeof *j);
  j->kind = JK_INFO;
  j->ttl = TTL_RECS;
  j->icb = cb;
  j->ud = ud;
  return submit(
      j, "query($id:Int){Media(id:$id,type:ANIME){id format genres tags{name rank} studios(isMain:true){nodes{name}}}}",
      v);
}

/* ---- ids ---- */
static void ask_mal(int mal, const char *key, AlIdCb cb, void *ud) {
  char v[40];
  snprintf(v, sizeof v, "{\"m\":%d}", mal);
  Job *j = calloc(1, sizeof *j);
  j->kind = JK_MAL;
  j->mal = mal;
  j->rcb = cb;
  j->ud = ud;
  snprintf(j->key, sizeof j->key, "%s", key);
  submit(j, "query($m:Int){Media(idMal:$m,type:ANIME){id}}", v);
}
typedef struct {
  char key[24];
  AlIdCb cb;
  void *ud;
} KJob;
static void on_kitsu(int status, const unsigned char *body, size_t len, void *ud) {
  KJob *k = ud;
  (void)len;
  int mal = 0;
  cJSON *j = status == 200 ? net_json(body) : NULL;
  const cJSON *m0 = cJSON_GetArrayItem(js_arr(j, "data"), 0);
  const char *ex = js_str(js_obj(m0, "attributes"), "externalId", NULL);
  if (ex)
    mal = atoi(ex);
  if (j)
    net_json_free(j);
  if (mal > 0)
    ask_mal(mal, k->key, k->cb, k->ud);
  else
    k->cb(0, k->ud);
  free(k);
}
void anilist_resolve(const char *id, AlIdCb cb, void *ud) {
  if (!cb)
    return;
  if (id && !strncmp(id, "tt", 2)) {
    cb(anime_map_alid(id), ud);
    return;
  }
  const char *colon = id ? strchr(id, ':') : NULL;
  int n = colon ? atoi(colon + 1) : 0;
  if (n <= 0) {
    cb(0, ud);
    return;
  }
  if (!strncmp(id, "anilist:", 8)) {
    cb(n, ud);
    return;
  }
  int kitsu = !strncmp(id, "kitsu:", 6);
  if (!kitsu && strncmp(id, "mal:", 4)) {
    cb(0, ud);
    return;
  }
  char key[24];
  snprintf(key, sizeof key, "%s:%d", kitsu ? "kitsu" : "mal", n);
  int al = pair_get(key);
  if (al) {
    cb(al, ud);
    return;
  }
  if (!kitsu) {
    ask_mal(n, key, cb, ud);
    return;
  }
  char u[200];
  KJob *k = calloc(1, sizeof *k);
  snprintf(k->key, sizeof k->key, "%s", key);
  k->cb = cb;
  k->ud = ud;
  snprintf(u, sizeof u, "https://kitsu.io/api/edge/anime/%d/mappings?filter%%5BexternalSite%%5D=myanimelist%%2Fanime",
           n);
  net_get(u, on_kitsu, k);
}

/* ---- the dev hook: SB_ANILIST_TEST=1 ---- */
typedef struct {
  const char *label;
  long long t0;
} TCtx;
static TCtx tc[8];
static int tdone, tstarted;
static void t_row(ItemList *l, int cached, void *ud) {
  TCtx *c = ud;
  fprintf(stderr, "[anilist-test] %s: %d items, first: %s | %s | %s, %lld ms, cache %s\n", c->label, l->n,
          l->n > 0 ? l->items[0].name : "-", l->n > 1 ? l->items[1].name : "-", l->n > 2 ? l->items[2].name : "-",
          now_ms() - c->t0, cached ? "hit" : "miss");
  if (l->n > 0)
    fprintf(stderr, "[anilist-test] %s: sample %s | %s | %.80s\n", c->label, l->items[0].id, l->items[0].metaLine,
            l->items[0].description);
  itemlist_free(l);
  tdone++;
}
static void t_id(int al, void *ud) {
  TCtx *c = ud;
  fprintf(stderr, "[anilist-test] %s: -> anilist %d, %lld ms\n", c->label, al, now_ms() - c->t0);
  tdone++;
}
static void t_info(const AlInfo *in, int cached, void *ud) {
  TCtx *c = ud;
  if (!in)
    fprintf(stderr, "[anilist-test] %s: failed\n", c->label);
  else
    fprintf(stderr, "[anilist-test] %s: %s, %d genres (%s), %d tags (%s), studio %s, %lld ms, cache %s\n", c->label,
            in->format, in->ngenres, in->ngenres ? in->genres[0] : "-", in->ntags, in->ntags ? in->tags[0] : "-",
            in->studio, now_ms() - c->t0, cached ? "hit" : "miss");
  tdone++;
}
static void dev_hook(void) {
  static long long t0;
  static int on = -1;
  if (on < 0)
    on = getenv("SB_ANILIST_TEST") != NULL;
  if (!on)
    return;
  long long now = now_ms();
  if (!t0)
    t0 = now;
  if (!tstarted && now - t0 > 10000) {
    tstarted = 1;
    tc[0] = (TCtx){ "TRENDING", now };
    tc[1] = (TCtx){ "THIS_SEASON", now };
    tc[2] = (TCtx){ "GENRE Action", now };
    tc[3] = (TCtx){ "RECS_FOR 21", now };
    tc[4] = (TCtx){ "resolve kitsu:12", now };
    tc[5] = (TCtx){ "info 21", now };
    anilist_row(AL_ROW_TRENDING, NULL, t_row, &tc[0]);
    anilist_row(AL_ROW_THIS_SEASON, NULL, t_row, &tc[1]);
    anilist_row(AL_ROW_GENRE, "Action", t_row, &tc[2]);
    anilist_row(AL_ROW_RECS_FOR, "21", t_row, &tc[3]);
    anilist_resolve("kitsu:12", t_id, &tc[4]);
    anilist_media_info(21, t_info, &tc[5]);
  }
  static int printed;
  if (tstarted && tdone >= 6 && !printed) {
    printed = 1;
    fprintf(stderr, "[anilist-test] done: %d requests, %d cache hits\n", al_requests, al_cache_hits);
  }
}

static void anime_map_test(void) {
  static const int ids[] = { 16498, 20958, 113415, 145064, 1, 999999 };
  static long long t0;
  static int on = -1, started, printed;
  if (on < 0)
    on = getenv("SB_ANIME_MAP_TEST") != NULL;
  if (!on)
    return;
  long long now = now_ms();
  if (!t0)
    t0 = now;
  if (!started && now - t0 > 10000) {
    started = 1;
    anime_map_want(ids, (int)(sizeof ids / sizeof ids[0]));
    fprintf(stderr, "[animemap-test] asked %d ids\n", (int)(sizeof ids / sizeof ids[0]));
  }
  if (started && !printed) {
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++)
      if (!anime_map_known(ids[i]))
        return;
    printed = 1;
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++)
      fprintf(stderr, "[animemap-test] %d: imdb %s, kitsu %d, season %d\n", ids[i],
              anime_map_imdb(ids[i]) ? anime_map_imdb(ids[i]) : "-", anime_map_kitsu(ids[i]), anime_map_season(ids[i]));
    fprintf(stderr, "[animemap-test] tt2560140 -> %d\n", anime_map_alid("tt2560140"));
  }
}

void anilist_tick(void) {
  if (qh)
    pump();
  dev_hook();
  anime_map_test();
  anime_map_tick();
}
