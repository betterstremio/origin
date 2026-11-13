/* tests/recs: the recommendation maths (src/core/recs_core.c), no app.
   recs_test               synthetic cases with checks (exit 1 on a failure)
   recs_test --real DIR    ranks what tests/recs/fetch.py saved from TMDB for a sample library and prints it
   recs_test --home DIR    runs the whole Home pipeline (seeds, taste, languages, rows, page) over cached TMDB answers
                           named sha1(request path)[:18].json, e.g. so/recs-proto-cache, and prints the page */
#include "core/recs_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <dirent.h>

static int fails;
#define CHECK(c, ...)                             \
  do {                                            \
    if (!(c)) {                                   \
      fails++;                                    \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                        \
      printf("\n");                               \
    } else                                        \
      printf("ok   " __VA_ARGS__), printf("\n");  \
  } while (0)

/* a TMDB-like result entry */
static cJSON *ent(int id, const char *title, const char *date, const char *lang, int g1, int g2, double vote, int votes,
                  int adult) {
  cJSON *e = cJSON_CreateObject();
  cJSON_AddNumberToObject(e, "id", id);
  cJSON_AddStringToObject(e, "title", title);
  cJSON_AddStringToObject(e, "release_date", date);
  cJSON_AddStringToObject(e, "original_language", lang);
  cJSON_AddStringToObject(e, "poster_path", "/p.jpg");
  cJSON_AddStringToObject(e, "backdrop_path", "/b.jpg");
  cJSON *g = cJSON_AddArrayToObject(e, "genre_ids");
  if (g1)
    cJSON_AddItemToArray(g, cJSON_CreateNumber(g1));
  if (g2)
    cJSON_AddItemToArray(g, cJSON_CreateNumber(g2));
  cJSON_AddNumberToObject(e, "vote_average", vote);
  cJSON_AddNumberToObject(e, "vote_count", votes);
  cJSON_AddBoolToObject(e, "adult", adult);
  return e;
}
static int find(const RecCand *c, int n, int id) {
  for (int i = 0; i < n; i++)
    if (c[i].tmdb == id)
      return i;
  return -1;
}

static void synthetic(void) {
  long today = recs_days("2026-11-08");
  /* weights */
  double fresh = recs_seed_weight(1, 0.95, 0, 0, 0), old = recs_seed_weight(60, 0.95, 0, 0, 0),
         half = recs_seed_weight(1, 0.4, 0, 0, 0), barely = recs_seed_weight(1, 0.03, 0, 0, 0),
         started = recs_seed_weight(1, 0.08, 0, 1, 0);
  CHECK(fresh > old, "recent finished film outweighs an old one (%.3f > %.3f)", fresh, old);
  CHECK(half > 0 && half < fresh, "half-watched counts, less (%.3f)", half);
  CHECK(barely == 0, "under 5%% watched is ignored");
  CHECK(started >= 0.48, "a series just started counts as being watched (%.3f)", started);
  CHECK(recs_seed_weight(1, 0.1, 0, 1, 4) > fresh, "a series 4 episodes in counts more than a film");
  CHECK(recs_key(5, 'm') != recs_key(5, 't'), "movie and tv ids kept apart");
  CHECK(recs_days("1970-01-02") == 1 && recs_days("2026-11-08") > 20000, "date to days");

  /* seed A (Hindi drama, fresh, finished) and seed B (English sci-fi, older) */
  cJSON *la = cJSON_CreateArray(), *lb = cJSON_CreateArray();
  cJSON_AddItemToArray(la, ent(101, "Both lists", "2026-06-01", "hi", 18, 35, 7.5, 900, 0));
  for (int i = 0; i < 18; i++) {
    char t[32];
    snprintf(t, sizeof t, "A-%d", i);
    cJSON_AddItemToArray(la, ent(200 + i, t, "2025-12-01", "hi", 18, 0, 6.5, 300, 0));
  }
  cJSON_AddItemToArray(la, ent(999, "Watched already", "2025-01-01", "hi", 18, 0, 8, 900, 0));
  cJSON_AddItemToArray(lb, ent(101, "Both lists", "2026-06-01", "hi", 18, 35, 7.5, 900, 0));
  cJSON_AddItemToArray(lb, ent(300, "Sci-fi one", "2026-01-01", "en", 878, 0, 7.0, 500, 0));
  cJSON_AddItemToArray(lb, ent(301, "Adult one", "2026-01-01", "en", 878, 0, 7.0, 500, 1));
  cJSON_AddItemToArray(lb, ent(302, "Not out yet", "2027-05-01", "en", 878, 0, 0, 0, 0));
  cJSON_AddItemToArray(lb, ent(303, "Dismissed", "2026-02-01", "en", 878, 0, 7.0, 500, 0));
  cJSON_AddItemToArray(lb, ent(400, "Seed A itself", "2025-10-01", "hi", 18, 0, 7.0, 500, 0));
  RecCand ca[RECS_LIST], cb[RECS_LIST];
  int na = recs_parse(la, 'm', ca, RECS_LIST), nb = recs_parse(lb, 'm', cb, RECS_LIST);
  CHECK(na == 20 && nb == 6, "parse keeps entries with art (%d, %d)", na, nb);
  RecSeed s[2] = { { .tmdb = 400,
                     .kind = 'm',
                     .title = "Seed A",
                     .genres = { 18 },
                     .ng = 1,
                     .lang = "hi",
                     .w = fresh,
                     .list = ca,
                     .n = na },
                   { .tmdb = 500,
                     .kind = 'm',
                     .title = "Seed B",
                     .genres = { 878 },
                     .ng = 1,
                     .lang = "en",
                     .w = old,
                     .list = cb,
                     .n = nb } };
  int ex[2] = { recs_key(999, 'm'), recs_key(303, 'm') };
  RecPrefs p = { .langs = { "hi", "en" }, .nlangs = 2, .exclude = ex, .nexclude = 2, .today = today };
  RecCand out[64];
  int n = recs_rank(s, 2, &p, out, 64);
  CHECK(n > 0 && out[0].tmdb == 101, "the title both seeds recommend comes first (got %d)", n ? out[0].tmdb : -1);
  CHECK(find(out, n, 999) < 0 && find(out, n, 303) < 0, "watched and dismissed titles are left out");
  CHECK(find(out, n, 301) < 0, "adult titles are left out");
  CHECK(find(out, n, 302) < 0, "unreleased titles are left out");
  CHECK(find(out, n, 400) < 0, "a seed never recommends itself");
  int fromA = 0, fifthA = -1;
  for (int i = 0; i < n; i++)
    if (out[i].bestSeed == 0 && ++fromA == 5) {
      fifthA = i;
      break;
    }
  CHECK(find(out, n, 300) >= 0 && find(out, n, 300) < fifthA,
        "diversity: the other seed's pick (at %d) comes before the dominant seed's 5th (at %d)", find(out, n, 300),
        fifthA);
  CHECK(n >= 20, "with too few alternatives the row still fills up from the dominant seed (%d picks)", n);

  /* Because you watched Seed B, minus the billboard's first pick */
  int skip[1] = { recs_key(out[0].tmdb, 'm') };
  RecCand byw[20];
  int nbw = recs_rank_seed(&s[1], s, 2, &p, skip, 1, byw, 20);
  CHECK(nbw == 1 && byw[0].tmdb == 300, "because-you-watched B: only B's own list, minus the billboard pick (%d)", nbw);

  /* language preference: same everything, Hindi picked */
  cJSON *lc = cJSON_CreateArray();
  cJSON_AddItemToArray(lc, ent(600, "English twin", "2026-03-01", "en", 18, 0, 7, 500, 0));
  cJSON_AddItemToArray(lc, ent(601, "Hindi twin", "2026-03-01", "hi", 18, 0, 7, 500, 0));
  RecCand cc[4];
  int nc = recs_parse(lc, 'm', cc, 4);
  RecSeed s2 = { .tmdb = 700, .kind = 'm', .genres = { 18 }, .ng = 1, .lang = "hi", .w = 1, .list = cc, .n = nc };
  RecPrefs p2 = { .langs = { "hi" }, .nlangs = 1, .today = today };
  RecCand o2[4];
  int n2 = recs_rank(&s2, 1, &p2, o2, 4);
  CHECK(n2 == 2 && o2[0].tmdb == 601, "a picked language wins a tie, even from lower in the list");

  /* "Hide films still in cinemas": off shows a 10-day-old film, on leaves it out */
  {
    cJSON *lz = cJSON_CreateArray();
    char d10[32];
    long t = today - 10;
    long z = t + 719468, era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097,
         yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100),
         mp = (5 * doy + 2) / 153;
    int dd = (int)(doy - (153 * mp + 2) / 5 + 1), mm = (int)(mp < 10 ? mp + 3 : mp - 9);
    long yy = yoe + era * 400 + (mm <= 2);
    snprintf(d10, sizeof d10, "%04ld-%02d-%02d", yy, mm, dd);
    cJSON_AddItemToArray(lz, ent(800, "Just out", d10, "hi", 18, 0, 7, 50, 0));
    cJSON_AddItemToArray(lz, ent(801, "Older", "2025-06-01", "hi", 18, 0, 7, 500, 0));
    RecCand cz[4];
    int nz = recs_parse(lz, 'm', cz, 4);
    RecSeed sz = { .tmdb = 900, .kind = 'm', .genres = { 18 }, .ng = 1, .lang = "hi", .w = 1, .list = cz, .n = nz };
    RecPrefs off = { .langs = { "hi" }, .nlangs = 1, .today = today }, on = off;
    on.minMovieAge = 30;
    RecCand oz[4];
    int a = recs_rank(&sz, 1, &off, oz, 4), hasA = find(oz, a, 800) >= 0;
    int b2 = recs_rank(&sz, 1, &on, oz, 4), hasB = find(oz, b2, 800) >= 0;
    CHECK(hasA && !hasB && b2 == 1, "hide-cinema toggle: off shows a 10-day-old film (%d), on leaves it out (%d)", hasA,
          hasB);
    cJSON_Delete(lz);
  }
  /* Top 10: kept in order, dismissed left out */
  RecCand t10[RECS_LIST];
  int nt = recs_filter(cb, nb, &p, t10, 10);
  CHECK(nt == 3 && t10[0].tmdb == 101 && t10[1].tmdb == 300,
        "Top 10 keeps the chart order, drops dismissed/adult/unreleased (%d)", nt);
  cJSON_Delete(la);
  cJSON_Delete(lb);
  cJSON_Delete(lc);
}

/* ---- real data saved by fetch.py ---- */
static cJSON *load(const char *dir, const char *name) {
  char path[1024];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *b = malloc((size_t)len + 1);
  if (!b) {
    fclose(f);
    return NULL;
  }
  size_t got = fread(b, 1, (size_t)len, f);
  b[got] = 0;
  fclose(f);
  cJSON *j = cJSON_Parse(b);
  free(b);
  return j;
}
static void print_list(const char *title, const RecCand *c, int n, const RecSeed *s) {
  printf("\n== %s ==\n", title);
  for (int i = 0; i < n; i++)
    printf("%2d. %-38.38s %s %s %4.2f%s%s\n", i + 1, c[i].title, c[i].lang, c[i].date, c[i].score,
           s && c[i].bestSeed >= 0 ? "  via " : "", s && c[i].bestSeed >= 0 ? s[c[i].bestSeed].title : "");
}
static int real(const char *dir) {
  cJSON *seeds = load(dir, "seeds.json");
  if (!seeds) {
    printf("no %s/seeds.json: run fetch.py first\n", dir);
    return 1;
  }
  static RecCand lists[RECS_MAX_SEEDS][RECS_LIST], discs[RECS_MAX_SEEDS][RECS_LIST];
  RecSeed s[RECS_MAX_SEEDS];
  int ns = 0;
  int ex[RECS_MAX_SEEDS];
  const cJSON *e;
  time_t now = time(NULL);
  struct tm tm;
  gmtime_r(&now, &tm);
  char ymd[16];
  strftime(ymd, sizeof ymd, "%Y-%m-%d", &tm);
  cJSON_ArrayForEach(e, seeds) {
    if (ns >= RECS_MAX_SEEDS)
      break;
    RecSeed *x = &s[ns];
    memset(x, 0, sizeof *x);
    x->tmdb = cJSON_GetObjectItem(e, "tmdb")->valueint;
    x->kind = cJSON_GetObjectItem(e, "kind")->valuestring[0];
    snprintf(x->title, sizeof x->title, "%s", cJSON_GetObjectItem(e, "title")->valuestring);
    snprintf(x->lang, sizeof x->lang, "%s", cJSON_GetObjectItem(e, "lang")->valuestring);
    const cJSON *g;
    cJSON_ArrayForEach(g, cJSON_GetObjectItem(e, "genres"))
      if (x->ng < 8)
        x->genres[x->ng++] = g->valueint;
    x->w = recs_seed_weight(cJSON_GetObjectItem(e, "ageDays")->valuedouble, cJSON_GetObjectItem(e, "frac")->valuedouble,
                            cJSON_IsTrue(cJSON_GetObjectItem(e, "marked")), x->kind == 't',
                            cJSON_GetObjectItem(e, "eps")->valueint);
    cJSON *r = load(dir, cJSON_GetObjectItem(e, "file")->valuestring);
    x->n = r ? recs_parse(cJSON_GetObjectItem(r, "results"), x->kind, lists[ns], RECS_LIST) : 0;
    x->list = lists[ns];
    cJSON_Delete(r);
    cJSON *d = load(dir, cJSON_GetObjectItem(e, "disc")->valuestring);
    x->nd = d ? recs_parse(cJSON_GetObjectItem(d, "results"), x->kind, discs[ns], RECS_LIST) : 0;
    x->disc = discs[ns];
    cJSON_Delete(d);
    ex[ns] = recs_key(x->tmdb, x->kind);
    printf("seed %-34.34s %s w=%.3f  %d recommended + %d same language/genre\n", x->title, x->lang, x->w, x->n, x->nd);
    ns++;
  }
  RecPrefs p = { .langs = { "hi", "en" }, .nlangs = 2, .exclude = ex, .nexclude = ns, .today = recs_days(ymd) };
  RecCand out[64];
  int n = recs_rank(s, ns, &p, out, 64);
  print_list("Billboard (Top picks, first 8)", out, n < 8 ? n : 8, s);
  int best = 0;
  for (int i = 1; i < ns; i++)
    if (s[i].w > s[best].w)
      best = i;
  int skip[8];
  for (int i = 0; i < 8 && i < n; i++)
    skip[i] = recs_key(out[i].tmdb, out[i].kind);
  RecCand bw[20];
  int nbw = recs_rank_seed(&s[best], s, ns, &p, skip, n < 8 ? n : 8, bw, 20);
  char t[200];
  snprintf(t, sizeof t, "Because you watched %s", s[best].title);
  print_list(t, bw, nbw, s);
  cJSON *lj = load(dir, "langs.json");
  const cJSON *lv;
  int nl = 0;
  static RecCand per[8][RECS_LIST];
  const RecCand *pl[8];
  int pn[8];
  for (int k = 0; k < 2; k++) {
    nl = 0;
    cJSON_ArrayForEach(lv, lj) {
      if (nl >= 8)
        break;
      char f[64];
      snprintf(f, sizeof f, "top_%s_%s.json", k ? "tv" : "movie", lv->valuestring);
      cJSON *r = load(dir, f);
      pn[nl] = r ? recs_parse(cJSON_GetObjectItem(r, "results"), k ? 't' : 'm', per[nl], RECS_LIST) : 0;
      pl[nl] = per[nl];
      nl++;
      cJSON_Delete(r);
    }
    RecPrefs pt = p;
    pt.nexclude = 0;
    RecCand top[10];
    print_list(k ? "Top 10 series this week" : "Top 10 movies this week", top,
               recs_interleave(pl, pn, nl, &pt, top, 10), NULL);
  }
  cJSON_Delete(lj);
  cJSON_Delete(seeds);
  return 0;
}

/* ================= Home rows engine: synthetic tests ================= */
static void mk(RecCand *c, int id, char kind, const char *title, const char *lang, int g1, int g2, double vote,
               int votes, const char *date) {
  memset(c, 0, sizeof *c);
  c->tmdb = id;
  c->kind = kind;
  snprintf(c->title, sizeof c->title, "%s", title);
  snprintf(c->lang, sizeof c->lang, "%s", lang);
  snprintf(c->poster, sizeof c->poster, "/p");
  snprintf(c->backdrop, sizeof c->backdrop, "/b");
  snprintf(c->date, sizeof c->date, "%s", date);
  if (g1)
    c->genres[c->ng++] = g1;
  if (g2)
    c->genres[c->ng++] = g2;
  c->vote = (float)vote;
  c->votes = votes;
  c->pop = 50;
}
static void mkseed(RecSeedX *s, int id, char kind, const char *title, const char *lang, int g1, int g2, double w,
                   double age) {
  memset(s, 0, sizeof *s);
  s->tmdb = id;
  s->kind = kind;
  snprintf(s->title, sizeof s->title, "%s", title);
  snprintf(s->lang, sizeof s->lang, "%s", lang);
  if (g1)
    s->genres[s->ng++] = g1;
  if (g2)
    s->genres[s->ng++] = g2;
  s->w = w;
  s->ageDays = age;
}
static const char *tr_mark(const char *s) {
  static char buf[8][200];
  static int at;
  char *b = buf[at++ & 7];
  snprintf(b, 200, "[%s]", s);
  return b;
}
static int has(const char *hay, const char *needle) {
  return strstr(hay, needle) != NULL;
}
static int count_fam(const RecRowDef *d, int n, int fam) {
  int c = 0;
  for (int i = 0; i < n; i++)
    c += d[i].family == fam;
  return c;
}

static void t_parse_seed(void) {
  const char *mj =
      "{\"id\":77,\"title\":\"English Vinglish\",\"original_language\":\"hi\",\"release_date\":\"2012-10-05\",\"genres\":[{\"id\":18},{\"id\":35}],"
      "\"keywords\":{\"keywords\":[{\"id\":1,\"name\":\"a\"},{\"id\":2,\"name\":\"b\"}]},"
      "\"credits\":{\"cast\":[{\"id\":10,\"name\":\"Sri\"},{\"id\":11,\"name\":\"Adil\"},{\"id\":12,\"name\":\"Priya\"},{\"id\":13,\"name\":\"Fourth\"}],"
      "\"crew\":[{\"id\":21,\"name\":\"Prod\",\"job\":\"Producer\"},{\"id\":20,\"name\":\"Gauri\",\"job\":\"Director\"}]},"
      "\"recommendations\":{\"results\":[{\"id\":501,\"title\":\"R1\",\"poster_path\":\"/p\",\"backdrop_path\":\"/b\",\"release_date\":\"2020-01-01\",\"original_language\":\"hi\",\"genre_ids\":[18]}]},"
      "\"similar\":{\"results\":[{\"id\":601,\"title\":\"S1\",\"poster_path\":\"/p\",\"backdrop_path\":\"/b\"},{\"id\":602,\"title\":\"S2\",\"poster_path\":\"/p\",\"backdrop_path\":\"/b\"},{\"id\":603,\"title\":\"NoArt\"}]},"
      "\"external_ids\":{\"imdb_id\":\"tt2181931\"}}";
  const char *tj =
      "{\"id\":88,\"name\":\"Panchayat\",\"original_language\":\"hi\",\"first_air_date\":\"2020-04-03\",\"genres\":[{\"id\":35}],"
      "\"keywords\":{\"results\":[{\"id\":7,\"name\":\"village\"}]},\"credits\":{\"cast\":[{\"id\":30,\"name\":\"Jitendra\"}],\"crew\":[]},"
      "\"created_by\":[{\"id\":40,\"name\":\"TVF\"}],\"recommendations\":{\"results\":[]},\"similar\":{\"results\":[]},\"external_ids\":{\"imdb_id\":\"tt12004706\"}}";
  cJSON *m = cJSON_Parse(mj), *t = cJSON_Parse(tj);
  RecSeedX s;
  RecCand rb[20], sb[20];
  memset(&s, 0, sizeof s);
  s.w = 0.7;
  s.ageDays = 3;
  CHECK(recs_parse_seed(m, 'm', &s, rb, 20, sb, 20) == 1, "movie answer parses");
  CHECK(s.tmdb == 77 && !strcmp(s.title, "English Vinglish") && !strcmp(s.lang, "hi") && s.year == 2012 &&
            !strcmp(s.imdb, "tt2181931"),
        "movie: id, title, language, year, imdb");
  CHECK(s.ng == 2 && s.genres[0] == 18 && s.nkw == 2 && s.kw[1] == 2, "movie: genres and keywords.keywords");
  CHECK(s.ncast == 3 && s.cast[2].id == 12 && !strcmp(s.cast[0].name, "Sri") && s.ndir == 1 && s.dir[0].id == 20,
        "movie: first three cast, director only");
  CHECK(s.nrecs == 1 && s.nsim == 2 && s.recs == rb && s.sim == sb && s.recs[0].tmdb == 501,
        "movie: recommendations and similar (titles without art skipped)");
  CHECK(s.w == 0.7 && s.ageDays == 3, "caller's weight and age are left alone");
  memset(&s, 0, sizeof s);
  CHECK(recs_parse_seed(t, 't', &s, rb, 20, sb, 20) == 1 && !strcmp(s.title, "Panchayat") && s.year == 2020,
        "tv answer parses (name, first_air_date)");
  CHECK(s.nkw == 1 && s.kw[0] == 7 && s.ndir == 1 && s.dir[0].id == 40 && !strcmp(s.imdb, "tt12004706"),
        "tv: keywords.results, created_by as director, imdb");
  CHECK(recs_parse_seed(NULL, 'm', &s, rb, 20, sb, 20) == 0, "garbage is refused");
  cJSON_Delete(m);
  cJSON_Delete(t);
}

static void t_taste_langs(void) {
  RecSeedX s[3];
  RecTaste t;
  mkseed(&s[0], 1, 'm', "A", "hi", 18, 35, 0.6, 3);
  mkseed(&s[1], 2, 'm', "B", "en", 878, 0, 0.3, 4);
  mkseed(&s[2], 3, 't', "C", "ko", 18, 0, 0.1, 5);
  s[0].kw[s[0].nkw++] = 100;
  s[1].kw[s[1].nkw++] = 100;
  s[0].cast[0].id = 10;
  snprintf(s[0].cast[0].name, 40, "Lead");
  s[0].ncast = 1;
  s[0].dir[0].id = 20;
  snprintf(s[0].dir[0].name, 40, "Dir");
  s[0].ndir = 1;
  recs_taste_build(s, 3, &t);
  CHECK(t.wsum > 0.99 && t.wsum < 1.01, "taste: weights sum (%.2f)", t.wsum);
  CHECK(recs_taste_lang(&t, "hi") > 0.59 && recs_taste_lang(&t, "hi") < 0.61 && recs_taste_lang(&t, "fr") == 0,
        "taste: language shares");
  CHECK(recs_taste_genre(&t, 18) == 1.0 && recs_taste_genre(&t, 878) < 0.5, "taste: genre relative to the top genre");
  CHECK(recs_taste_kind(&t, 't') > 0.09 && recs_taste_kind(&t, 't') < 0.11 && recs_taste_kind(&t, 'm') > 0.89,
        "taste: kind shares");
  CHECK(t.nkw == 1 && t.kww[0] > 0.89 && t.np == 2 && t.pdir[0] != t.pdir[1],
        "taste: keyword and person weights (cast and director kept apart)");
  /* title score: genre, language, quality, freshness */
  RecCand a, b;
  mk(&a, 9, 'm', "fit", "hi", 18, 35, 8, 500, "2026-06-01");
  mk(&b, 10, 'm', "off", "fr", 99, 0, 8, 500, "2026-06-01");
  long today = recs_days("2026-11-09");
  CHECK(recs_title_score(&t, &a, today) > recs_title_score(&t, &b, today) + 0.3,
        "score: a title that fits taste beats one that does not");
  RecCand fresh, old, thin;
  mk(&fresh, 11, 'm', "f", "hi", 18, 0, 7, 500, "2026-10-01");
  mk(&old, 12, 'm', "o", "hi", 18, 0, 7, 500, "1990-10-01");
  mk(&thin, 13, 'm', "t", "hi", 18, 0, 9.5, 3, "2026-10-01");
  CHECK(recs_title_score(&t, &fresh, today) > recs_title_score(&t, &old, today),
        "score: freshness and the old-title penalty");
  CHECK(recs_title_score(&t, &thin, today) < recs_title_score(&t, &fresh, today) + 0.0 + 0.1 &&
            recs_title_score(&t, &thin, today) < recs_title_score(&t, &fresh, today),
        "score: 3 votes at 9.5 do not beat 500 votes at 7 (quality is damped)");

  /* languages */
  RecLang L[RECS_MAX_LANGS];
  int n;
  n = recs_langs_allowed("ta,te,ml,kn", "hi,en", "en", &t, L);
  CHECK(n == 3 && !strcmp(L[0].code, "ta") && !strcmp(L[2].code, "ml") && L[0].strong && L[1].strong && L[2].strong,
        "languages: an explicit list wins outright, in order, capped at 3");
  n = recs_langs_allowed("", "hi,en,ur", "en", &t, L);
  CHECK(n == 3 && !strcmp(L[0].code, "hi") && L[0].strong && !strcmp(L[1].code, "en") && L[1].strong &&
            !strcmp(L[2].code, "ur") && !L[2].strong,
        "languages: history/audio strong (hi 60%%, en by audio), the region's extra one weak (%s %s %s)", L[0].code,
        L[1].code, L[2].code);
  n = recs_langs_allowed(NULL, "ur", "", &t, L);
  CHECK(n >= 2 && !strcmp(L[0].code, "hi") && L[0].strong && !strcmp(L[1].code, "en") && L[1].strong,
        "languages: en at 30%% of the history is strong without being asked for");
  int ur = -1, ko = -1;
  for (int i = 0; i < n; i++) {
    if (!strcmp(L[i].code, "ur"))
      ur = i;
    if (!strcmp(L[i].code, "ko"))
      ko = i;
  }
  CHECK(ko < 0 && ur >= 0 && !L[ur].strong, "languages: ko at 10%% is not allowed by history, the region's ur is weak");
  /* 25% threshold */
  RecSeedX q[2];
  RecTaste t2;
  mkseed(&q[0], 1, 'm', "A", "en", 18, 0, 0.76, 1);
  mkseed(&q[1], 2, 'm', "B", "hi", 18, 0, 0.24, 1);
  recs_taste_build(q, 2, &t2);
  n = recs_langs_allowed("", "hi,en", "", &t2, L);
  CHECK(n == 2 && !strcmp(L[0].code, "en") && L[0].strong && !strcmp(L[1].code, "hi") && !L[1].strong,
        "languages: 24%% of the history is below the line (hi stays weak, brought by the region)");
  q[0].w = 0.74;
  q[1].w = 0.26;
  recs_taste_build(q, 2, &t2);
  n = recs_langs_allowed("", "", "", &t2, L);
  CHECK(n == 2 && L[1].strong && !strcmp(L[1].code, "hi"), "languages: 26%% is strong");
  RecSeedX q4[4];
  const char *l4[4] = { "en", "hi", "ko", "es" };
  for (int i = 0; i < 4; i++)
    mkseed(&q4[i], i + 1, 'm', "x", l4[i], 18, 0, 1.0, 1);
  recs_taste_build(q4, 4, &t2);
  n = recs_langs_allowed("", "fr,de", "ja", &t2, L);
  CHECK(n == 3, "languages: never more than 3 (four languages at 25%% each, plus audio and region)");
  n = recs_langs_allowed("", "", "", &(RecTaste){ 0 }, L);
  CHECK(n == 0, "languages: nothing known gives nothing");
}

static RecRowCtx mkctx(const RecLang *l, int nl, const RecTaste *t, const RecSeedX *s, int ns, const char *region,
                       const char *today) {
  RecRowCtx c = {
    .region = region, .today = recs_days(today), .langs = l, .nlangs = nl, .taste = t, .seeds = s, .nseeds = ns
  };
  return c;
}

static void t_rows(void) {
  static RecSeedX seeds[8];
  static RecCand rb[8][20], sb[8][20];
  static RecTaste t;
  RecRowDef *d = malloc(sizeof(RecRowDef) * 160);
  const char *lg[8] = { "hi", "hi", "en", "hi", "en", "hi", "en", "hi" };
  for (int i = 0; i < 8; i++) {
    mkseed(&seeds[i], 100 + i, i % 3 == 2 ? 't' : 'm', "Seed", lg[i], 18, i % 2 ? 35 : 10749, 1.0 - i * 0.1, 3 + i * 4);
    snprintf(seeds[i].title, sizeof seeds[i].title, "Seed %d", i);
    for (int j = 0; j < 20; j++) {
      mk(&rb[i][j], 1000 + i * 40 + j, 'm', "r", lg[i], 18, 0, 7, 300, "2025-05-05");
      mk(&sb[i][j], 5000 + i * 40 + j, 'm', "s", lg[i], 18, 0, 7, 300, "2025-05-05");
    }
    seeds[i].recs = rb[i];
    seeds[i].nrecs = i == 7 ? 3 : 20;
    seeds[i].sim = sb[i];
    seeds[i].nsim = i == 7 ? 2 : 20;
    seeds[i].kw[0] = 10683;
    seeds[i].nkw = 1;
    seeds[i].ncast = 3;
    seeds[i].cast[0].id = 1 + i;
    snprintf(seeds[i].cast[0].name, 40, "Lead %d", i);
    seeds[i].cast[1].id = 50 + i;
    snprintf(seeds[i].cast[1].name, 40, "Second %d", i);
    seeds[i].cast[2].id = 70 + i;
    snprintf(seeds[i].cast[2].name, 40, "Third %d", i);
    seeds[i].ndir = 1;
    seeds[i].dir[0].id = 90 + i % 2;
    snprintf(seeds[i].dir[0].name, 40, "Dir %d", i % 2);
  }
  recs_taste_build(seeds, 8, &t);
  RecLang L[3] = { { "hi", 1 }, { "en", 1 }, { "ur", 0 } };
  RecRowCtx cx = mkctx(L, 3, &t, seeds, 8, "IE", "2026-11-09");
  int n = recs_rows_build(&cx, d, 160);
  CHECK(n >= 60 && n <= 130, "rows: a normal build gives about 70-110 candidate rows (%d)", n);
  int bad = 0, longp = 0;
  for (int i = 0; i < n; i++) {
    if (!d[i].title[0])
      bad++;
    if (strlen(d[i].path) > sizeof d[i].path - 8 || strlen(d[i].key) > sizeof d[i].key - 8)
      longp++;
    if (d[i].path[0] && d[i].path[0] != '/')
      bad++;
    if (d[i].family != RF_PERSONAL && !d[i].path[0])
      bad++;
  }
  CHECK(!bad && !longp, "rows: every row has a title, a path from /3, a key; nothing truncated");
  CHECK(d[0].family == RF_PERSONAL && !d[0].path[0] && d[0].affinity >= 2, "rows: Top picks first, local (no request)");
  CHECK(count_fam(d, n, RF_BYW) == 5, "rows: 5 Because-you-watched rows (%d)", count_fam(d, n, RF_BYW));
  {
    int first = -1;
    for (int i = 0; i < n; i++)
      if (d[i].family == RF_BYW) {
        first = i;
        break;
      }
    CHECK(d[first].seed == 0 && has(d[first].title, "Seed 0") && has(d[first].path, "with_original_language=hi") &&
              has(d[first].path, "with_genres=18|10749"),
          "rows: byw uses that seed's language and top genres (%s)", d[first].path);
    int seven = 0;
    for (int i = 0; i < n; i++)
      if (d[i].family == RF_BYW && d[i].seed == 7)
        seven++;
    CHECK(seven == 0, "rows: a seed with too little material (5 titles) gets no byw row");
  }
  CHECK(count_fam(d, n, RF_CHART) == 2 && count_fam(d, n, RF_TREND) == 1, "rows: two charts and trending");
  int ok = 1;
  for (int i = 0; i < n; i++) {
    if (d[i].family == RF_CHART && !((d[i].flags & RECS_F_RANK) && d[i].maxAge == RECS_AGE_CHART))
      ok = 0;
    if (d[i].family == RF_TREND && d[i].maxAge != RECS_AGE_CHART)
      ok = 0;
    if ((d[i].family == RF_LANG_GENRE || d[i].family == RF_MOOD) && d[i].maxAge != RECS_AGE_LIST)
      ok = 0;
  }
  CHECK(ok, "rows: ages: charts and trending 12 h (rank flag on charts), lists 24 h");
  /* languages: only allowed ones; weak ones get fewer kinds */
  int outside = 0, weakGems = 0, weakDec = 0, strongGems = 0, strongDec = 0, weakAc = 0, weakNew = 0, weakLg = 0,
      strongLg = 0;
  for (int i = 0; i < n; i++) {
    int lf = d[i].family == RF_LANG_GENRE || d[i].family == RF_ACCLAIM || d[i].family == RF_GEMS ||
             d[i].family == RF_NEW || d[i].family == RF_DECADE;
    if (!lf)
      continue;
    if (strcmp(d[i].lang, "hi") && strcmp(d[i].lang, "en") && strcmp(d[i].lang, "ur"))
      outside++;
    int weak = !strcmp(d[i].lang, "ur");
    if (d[i].family == RF_GEMS) {
      if (weak)
        weakGems++;
      else
        strongGems++;
    }
    if (d[i].family == RF_DECADE) {
      if (weak)
        weakDec++;
      else
        strongDec++;
    }
    if (d[i].family == RF_ACCLAIM && weak)
      weakAc++;
    if (d[i].family == RF_NEW && weak)
      weakNew++;
    if (d[i].family == RF_LANG_GENRE) {
      if (weak)
        weakLg++;
      else
        strongLg++;
    }
  }
  CHECK(!outside, "rows: language rows only for the allowed languages");
  CHECK(weakGems == 0 && weakDec == 0 && strongGems == 2 && strongDec >= 2,
        "rows: weak language has no 'may have missed' and no decade rows (strong: %d, %d)", strongGems, strongDec);
  CHECK(weakAc == 1 && weakNew == 2 && weakLg > 0 && weakLg < strongLg / 2 + 1,
        "rows: weak language still has best-rated, new, and fewer genre rows (%d vs %d strong)", weakLg, strongLg);
  /* animation / kids */
  int lgBad = 0;
  for (int i = 0; i < n; i++)
    if ((d[i].family == RF_LANG_GENRE || d[i].family == RF_ACCLAIM || d[i].family == RF_GEMS || d[i].family == RF_NEW ||
         d[i].family == RF_DECADE || d[i].family == RF_SEASON) &&
        !has(d[i].path, "without_genres=16,10751,10762"))
      lgBad++;
  CHECK(lgBad == 0,
        "rows: language and season rows exclude animation, family and kids when the taste has no animation");
  {
    int famRows = 0;
    for (int i = 0; i < n; i++)
      if (d[i].family == RF_LANG_GENRE && (has(d[i].path, "with_genres=10751") || has(d[i].path, "with_genres=16")))
        famRows++;
    CHECK(famRows == 0, "rows: no 'family' or 'animated' genre rows when the taste has no animation");
  }
  {
    int kwNoGate = 0, kwRows = 0;
    for (int i = 0; i < n; i++)
      if (has(d[i].path, "with_keywords=")) {
        kwRows++;
        if (!has(d[i].path, "with_genres="))
          kwNoGate++;
      }
    CHECK(kwRows >= 20 && kwNoGate == 0,
          "rows: every keyword-based row in a full build (mood and season) has a genre gate (%d rows)", kwRows);
  }
  /* mood table */
  int mc = recs_mood_count();
  int gated = 1, kwrows = 0, exclOk = 1, titlesOk = 1;
  RecRowDef one;
  RecTaste tAnim;
  RecSeedX an[2];
  mkseed(&an[0], 1, 'm', "Toy", "en", 16, 10751, 1.0, 1);
  mkseed(&an[1], 2, 'm', "Cars", "en", 16, 35, 1.0, 1);
  recs_taste_build(an, 2, &tAnim);
  RecRowCtx cxA = mkctx(L, 3, &tAnim, an, 2, "IE", "2026-11-09");
  for (int i = 0; i < mc; i++) {
    for (int v = 0; v < 2; v++) {
      const RecRowCtx *c = v ? &cxA : &cx;
      if (!recs_mood_row(c, i, &one)) {
        if (v == 0)
          titlesOk = 0;
        continue;
      }
      if (has(one.path, "with_keywords=")) {
        if (!has(one.path, "with_genres="))
          gated = 0;
        if (v == 0)
          kwrows++;
      }
      if (v == 0 && !has(one.path, "without_genres=16,10751,10762"))
        exclOk = 0;
      if (v == 1 && has(one.path, "without_genres"))
        exclOk = 0;
      if (!one.title[0] || one.family != RF_MOOD)
        titlesOk = 0;
    }
  }
  CHECK(mc >= 35 && mc <= 45, "mood table: %d hand-written rows", mc);
  CHECK(gated && kwrows >= 30, "mood table: every keyword row has a genre gate (%d keyword rows)", kwrows);
  CHECK(exclOk, "mood table: animation / family / kids excluded unless the taste includes animation");
  CHECK(titlesOk, "mood table: every row builds on both kinds of taste and has a title");
  {
    int dup = 0;
    RecRowDef a, b;
    for (int i = 0; i < mc; i++)
      for (int j = i + 1; j < mc; j++)
        if (recs_mood_row(&cx, i, &a) && recs_mood_row(&cx, j, &b) && !strcmp(a.title, b.title))
          dup++;
    CHECK(dup == 0, "mood table: titles are unique");
  }
  {
    int mr = 0, tvgood = 1;
    for (int i = 0; i < n; i++)
      if (d[i].family == RF_MOOD) {
        mr++;
        if (!strcmp(d[i].lang, "")) {
        }
        if (has(d[i].path, "/discover/tv") && (has(d[i].path, "with_genres=27") || has(d[i].path, "=53")))
          tvgood = 0;
      }
    CHECK(mr == 26 && tvgood, "rows: the 26 best-fitting mood rows are built (%d)", mr);
    int hasLang = 0;
    for (int i = 0; i < n; i++)
      if (d[i].family == RF_MOOD && has(d[i].path, "with_original_language"))
        hasLang++;
    CHECK(hasLang == 0, "rows: with several allowed languages mood rows have no language filter (TMDB takes one code)");
  }
  {
    RecLang one1[1] = { { "hi", 1 } };
    RecRowCtx c1 = mkctx(one1, 1, &t, seeds, 8, "IE", "2026-11-09");
    RecRowDef *d1 = malloc(sizeof(RecRowDef) * 160);
    int n1 = recs_rows_build(&c1, d1, 160), hl = 0;
    for (int i = 0; i < n1; i++)
      if (d1[i].family == RF_MOOD && has(d1[i].path, "with_original_language=hi"))
        hl++;
    CHECK(hl > 0, "rows: a single allowed language is applied to mood rows (%d)", hl);
    free(d1);
  }
  /* people */
  {
    int cast = 0, dir = 0;
    for (int i = 0; i < n; i++)
      if (d[i].family == RF_PERSON) {
        if (has(d[i].path, "with_cast="))
          cast++;
        if (has(d[i].path, "with_crew="))
          dir++;
      }
    CHECK(cast == 4 && dir == 2, "rows: at most 4 cast rows and 2 directed-by rows (%d, %d)", cast, dir);
    int lead = 0;
    for (int i = 0; i < n; i++)
      if (d[i].family == RF_PERSON && has(d[i].title, "More with Lead"))
        lead++;
    CHECK(lead >= 2, "rows: the lead actors come first (%d of the 4 are leads)", lead);
  }
  /* providers */
  {
    RecRowCtx c2 = cx;
    c2.region = "PK";
    int k = recs_rows_build(&c2, d, 160), pr = 0;
    for (int i = 0; i < k; i++)
      if (d[i].family == RF_PROVIDER) {
        pr++;
      }
    CHECK(pr == 2, "providers: Pakistan has only Netflix and Apple TV+ in the table (%d)", pr);
    c2.region = "ZZ";
    k = recs_rows_build(&c2, d, 160);
    CHECK(count_fam(d, k, RF_PROVIDER) == 0, "providers: an unknown region has no provider rows");
    c2.region = "IN";
    k = recs_rows_build(&c2, d, 160);
    int pin = 0, fl = 1;
    for (int i = 0; i < k; i++)
      if (d[i].family == RF_PROVIDER) {
        pin++;
        if (!has(d[i].path, "vote_count.gte=") || !has(d[i].path, "sort_by=popularity.desc") ||
            !has(d[i].path, "watch_region=IN") || !has(d[i].key, "IN"))
          fl = 0;
      }
    CHECK(pin == 3 && fl,
          "providers: India has 3 (no Disney+), each with a vote floor, popularity sort, region in path and key (%d)",
          pin);
    c2.region = "IE";
    k = recs_rows_build(&c2, d, 160);
    pr = 0;
    for (int i = 0; i < k; i++)
      if (d[i].family == RF_PROVIDER)
        pr++;
    CHECK(pr == 4, "providers: Ireland has 4");
  }
  /* keys */
  {
    RecRowCtx c2 = cx;
    RecRowDef *d2 = malloc(sizeof(RecRowDef) * 160);
    c2.region = "US";
    int k = recs_rows_build(&c2, d2, 160), same = 0, withReg = 1;
    for (int i = 0; i < n && i < k; i++)
      if (d[i].family == RF_PROVIDER || d[i].family == RF_CHART || d[i].family == RF_TREND) {
        if (!strcmp(d[i].key, d2[i].key))
          same++;
        if (!has(d[i].key, "IE") && !has(d[i].key, "8"))
          withReg = 0;
      }
    CHECK(same == 0 && withReg, "keys: provider, chart and trending keys carry the region");
    int uniq = 1;
    for (int i = 0; i < n && uniq; i++)
      for (int j = i + 1; j < n; j++)
        if (!strcmp(d[i].key, d[j].key)) {
          uniq = 0;
          printf("   dup key %s\n", d[i].key);
          break;
        }
    CHECK(uniq, "keys: unique per row");
    int lk = 1;
    for (int i = 0; i < n; i++)
      if (d[i].family == RF_LANG_GENRE && !has(d[i].key, d[i].lang))
        lk = 0;
    CHECK(lk, "keys: language rows carry the language");
    free(d2);
  }
  /* translate callback */
  {
    RecRowCtx c2 = cx;
    c2.tr = tr_mark;
    RecRowDef *d2 = malloc(sizeof(RecRowDef) * 160);
    int k = recs_rows_build(&c2, d2, 160), ok2 = 1, lang = 0, person = 0;
    for (int i = 0; i < k; i++) {
      if (d2[i].title[0] != '[')
        ok2 = 0;
      if (d2[i].family == RF_LANG_GENRE && has(d2[i].title, "[Hindi]"))
        lang++;
      if (d2[i].family == RF_PERSON && has(d2[i].title, "Lead"))
        person++;
    }
    CHECK(
        ok2 && lang > 0 && person > 0,
        "translate: every title goes through tr; language names go through it separately; names stay as they are (%s)",
        d2[1].title);
    free(d2);
  }
  /* season */
  {
    RecRowCtx c2 = cx;
    int k;
    c2.today = recs_days("2026-12-01");
    k = recs_rows_build(&c2, d, 160);
    int xm = 0;
    for (int i = 0; i < k; i++)
      if (d[i].family == RF_SEASON && has(d[i].path, "with_keywords=207317") && has(d[i].path, "with_genres="))
        xm++;
    CHECK(xm == 1, "season: Christmas row in December, with a genre gate");
    c2.today = recs_days("2026-06-15");
    k = recs_rows_build(&c2, d, 160);
    CHECK(count_fam(d, k, RF_SEASON) == 0, "season: nothing in June");
    c2.today = recs_days("2026-10-20");
    k = recs_rows_build(&c2, d, 160);
    CHECK(count_fam(d, k, RF_SEASON) == 1, "season: Halloween in late October");
  }
  /* low memory and the max parameter */
  {
    RecRowCtx c2 = cx;
    c2.lowMem = 1;
    int k = recs_rows_build(&c2, d, 160);
    CHECK(count_fam(d, k, RF_BYW) == 3 && count_fam(d, k, RF_MOOD) == 12 && count_fam(d, k, RF_DECADE) == 0 && k < n,
          "low memory: 3 byw, 12 mood, no decade rows, fewer rows (%d < %d)", k, n);
    int k2 = recs_rows_build(&cx, d, 10);
    CHECK(k2 == 10, "rows: never more than max (%d)", k2);
  }
  /* date filters */
  {
    int nw = -1;
    for (int i = 0; i < n; i++)
      if (d[i].family == RF_NEW && d[i].kind == 'm') {
        nw = i;
        break;
      }
    CHECK(nw >= 0 && has(d[nw].path, "primary_release_date.gte=2026-07-12") &&
              has(d[nw].path, "primary_release_date.lte=2026-11-09"),
          "dates: New films filter from 120 days ago to today (%s)", nw >= 0 ? d[nw].path : "");
  }
  free(d);
}

static void t_byw_top(void) {
  RecSeedX s;
  RecCand rec[20], sim[20], own[20], out[40];
  RecPrefs p = { .today = recs_days("2026-11-09") };
  mkseed(&s, 1, 'm', "English Vinglish", "hi", 18, 35, 1.0, 2);
  for (int i = 0; i < 20; i++) {
    char t[32];
    snprintf(t, sizeof t, "W%d", i);
    mk(&rec[i], 100 + i, 'm', t, i < 14 ? "en" : "hi", 18, 0, 7, 300, "2024-05-05");
    mk(&sim[i], 200 + i, 'm', t, i < 15 ? "en" : "fr", 35, 0, 7, 300, "2024-05-05");
    snprintf(t, sizeof t, "H%d", i);
    mk(&own[i], 300 + i, 'm', t, "hi", 18, 35, 7, 300, "2024-05-05");
  }
  s.recs = rec;
  s.nrecs = 20;
  s.sim = sim;
  s.nsim = 20;
  int ex[1] = { recs_key(300, 'm') };
  p.exclude = ex;
  p.nexclude = 1;
  int n = recs_byw_rank(&s, &p, own, 20, out, 20), hi = 0, hasEx = 0;
  for (int i = 0; i < n; i++) {
    hi += !strcmp(out[i].lang, "hi");
    hasEx += out[i].tmdb == 300;
  }
  CHECK(n == 20 && hi >= 15 && !hasEx, "byw: %d of 20 are Hindi, the dismissed title is left out", hi);
  {
    RecPrefs np = { .today = p.today };
    int n2 = recs_byw_rank(&s, &np, NULL, 0, out, 20), hi2 = 0;
    for (int i = 0; i < n2; i++)
      hi2 += !strcmp(out[i].lang, "hi");
    CHECK(hi2 >= 6 && out[0].tmdb == 114,
          "byw: even without the own-language list, the Hindi recommendations are pulled up (%d Hindi, first %d)", hi2,
          out[0].tmdb);
  }
  {
    int dup = 0;
    for (int i = 0; i < n; i++)
      for (int j = i + 1; j < n; j++)
        dup += out[i].tmdb == out[j].tmdb;
    CHECK(dup == 0, "byw: no duplicates");
  }

  /* top picks: Hindi and English each half the history, TMDB's lists English-first */
  RecSeedX ss[4];
  static RecCand l1[4][20], l2[4][20];
  RecTaste t;
  for (int k = 0; k < 4; k++) {
    mkseed(&ss[k], 10 + k, 'm', "S", k < 2 ? "hi" : "en", 18, 0, 1.0, 2);
    for (int j = 0; j < 20; j++) {
      int en = j < 14;
      char nm[32];
      snprintf(nm, sizeof nm, "%s%d", en ? "E" : "H", j);
      mk(&l1[k][j], en ? 1000 + k * 3 + j : 2000 + (k % 2) * 3 + j, 'm', nm, en ? "en" : "hi", 18, 0, 7.5, 400,
         "2025-08-08");
      mk(&l2[k][j], en ? 1100 + k * 3 + j : 2100 + (k % 2) * 3 + j, 'm', nm, en ? "en" : "hi", 18, 0, 7.5, 400,
         "2025-08-08");
    }
    ss[k].recs = l1[k];
    ss[k].nrecs = 20;
    ss[k].sim = l2[k];
    ss[k].nsim = 20;
  }
  recs_taste_build(ss, 4, &t);
  RecPrefs pp = { .today = p.today };
  static RecCand tp[60], plain[60];
  int np = recs_top_picks(ss, 4, &t, &pp, 0, plain, 60), enPlain = 0;
  for (int i = 0; i < 20 && i < np; i++)
    enPlain += !strcmp(plain[i].lang, "en");
  int nt = recs_top_picks(ss, 4, &t, &pp, 20, tp, 60), en = 0, hi3 = 0;
  for (int i = 0; i < 20 && i < nt; i++) {
    en += !strcmp(tp[i].lang, "en");
    hi3 += !strcmp(tp[i].lang, "hi");
  }
  CHECK(enPlain >= 15, "top picks: without the balance English drowns Hindi (%d of 20 English)", enPlain);
  CHECK(nt >= 40 && en <= 11 && hi3 >= 9,
        "top picks: with the balance, Hindi gets its half (%d Hindi, %d English of 20)", hi3, en);
  {
    int dup = 0;
    for (int i = 0; i < nt; i++)
      for (int j = i + 1; j < nt; j++)
        dup += tp[i].tmdb == tp[j].tmdb && tp[i].kind == tp[j].kind;
    CHECK(dup == 0 && nt <= 60, "top picks: no duplicates, bounded by max (%d)", nt);
  }
  { /* the seeds' own-language lists feed Top picks: TMDB's lists for these Hindi seeds are all English */
    RecSeedX hs[4];
    static RecCand wl[4][20], ol[4][20], none[20];
    RecTaste th;
    (void)none;
    for (int k = 0; k < 4; k++) {
      mkseed(&hs[k], 40 + k, 'm', "S", k < 2 ? "hi" : "en", 18, 0, 1.0, 2);
      for (int j = 0; j < 20; j++) {
        char nm[24];
        snprintf(nm, sizeof nm, "E%d", j);
        mk(&wl[k][j], 3000 + (k % 2) * 2 + j, 'm', nm, "en", 18, 0, 7.5, 400, "2025-08-08");
        snprintf(nm, sizeof nm, "H%d", j);
        mk(&ol[k][j], 4000 + k * 2 + j, 'm', nm, "hi", 18, 0, 7.0, 400, "2025-08-08");
      }
      hs[k].recs = wl[k];
      hs[k].nrecs = 20;
      hs[k].own = ol[k];
      hs[k].nown = 20;
    }
    recs_taste_build(hs, 4, &th);
    int k2 = recs_top_picks(hs, 4, &th, &pp, 20, tp, 60), hh = 0;
    for (int i = 0; i < 20 && i < k2; i++)
      hh += !strcmp(tp[i].lang, "hi");
    CHECK(hh >= 8 && hh <= 12, "top picks: own-language lists give the Hindi half real supply (%d Hindi of 20)", hh);
    int pre = 1;
    for (int w = 4; w <= 20; w += 4) {
      int h = 0;
      for (int i = 0; i < w; i++)
        h += !strcmp(tp[i].lang, "hi");
      if (h < w / 2 - 1 || h > w / 2 + 2)
        pre = 0;
    }
    CHECK(pre, "top picks: every prefix (4, 8, 12, 16, 20) is balanced, not only the total");
  }
  {
    RecSeedX one[1];
    mkseed(&one[0], 1, 'm', "S", "en", 18, 0, 1.0, 1);
    one[0].recs = l1[2];
    one[0].nrecs = 20;
    one[0].sim = NULL;
    one[0].nsim = 0;
    RecTaste t1;
    recs_taste_build(one, 1, &t1);
    int k = recs_top_picks(one, 1, &t1, &pp, 20, tp, 60);
    CHECK(k > 0, "top picks: a single-language history still fills the row (%d)", k);
  }
}

/* ---- the page ---- */
#define NR 72
static RecCand g_cards[NR][20];
static RecRowDef g_defs[NR];
static RecRowIn g_in[NR];

static void page_fixture(int thin) {
  for (int r = 0; r < NR; r++) {
    RecRowDef *d = &g_defs[r];
    memset(d, 0, sizeof *d);
    d->seed = -1;
    int fam, n = 20;
    float aff;
    if (r == 0) {
      fam = RF_PERSONAL;
      aff = 2;
      d->flags = RECS_F_KEEP;
    } else if (r <= 5) {
      fam = RF_BYW;
      aff = 1.2f;
      d->flags = RECS_F_KEEP;
      d->seed = r - 1;
    } else if (r <= 7) {
      fam = RF_CHART;
      aff = 0.8f;
      d->flags = RECS_F_RANK | RECS_F_KEEP;
      n = 10;
    } else if (r == 8) {
      fam = RF_TREND;
      aff = 0.5f;
    } else if (r < 40) {
      fam = RF_LANG_GENRE;
      aff = 0.30f + (r % 7) * 0.04f;
    } else if (r < 62) {
      fam = RF_MOOD;
      aff = 0.28f + (r % 5) * 0.05f;
    } else if (r < 68) {
      fam = RF_NEW;
      aff = 0.45f;
    } else {
      fam = RF_PROVIDER;
      aff = 0.3f;
    }
    if (thin && r >= 70)
      n = 7;
    d->family = (unsigned char)fam;
    d->kind = r % 4 == 0 ? 't' : 'm';
    d->affinity = aff;
    snprintf(d->title, sizeof d->title, "Row %d", r);
    for (int j = 0; j < n; j++) {
      int id = 1 + (int)((r * 7 + j * 13 + (j < 6 ? (r % 3) * 31 : 0)) % 520);
      char t[24], date[16];
      snprintf(t, sizeof t, "T%d", id);
      snprintf(date, sizeof date, "2025-%02d-%02d", 1 + id % 12, 1 + id % 27);
      mk(&g_cards[r][j], id, id % 5 == 0 ? 't' : 'm', t, id % 3 == 0 ? "hi" : "en", id % 2 ? 18 : 35, id % 7 ? 0 : 28,
         5 + id % 40 / 10.0, 100 + id, date);
    }
    g_in[r].def = d;
    g_in[r].cards = g_cards[r];
    g_in[r].n = n;
  }
}

static void t_page(void) {
  RecTaste t;
  RecSeedX s[2];
  mkseed(&s[0], 1, 'm', "A", "hi", 18, 35, 1, 1);
  mkseed(&s[1], 2, 'm', "B", "en", 35, 0, 1, 1);
  recs_taste_build(s, 2, &t);
  page_fixture(1);
  int bill[8], ex[6];
  for (int i = 0; i < 8; i++)
    bill[i] = recs_key(g_cards[0][i].tmdb, g_cards[0][i].kind);
  for (int i = 0; i < 6; i++)
    ex[i] = recs_key(g_cards[20][i].tmdb, g_cards[20][i].kind);
  RecPageCtx cx = {
    .taste = &t, .today = recs_days("2026-11-09"), .maxRows = 36, .bill = bill, .nbill = 8, .exclude = ex, .nexclude = 6
  };
  static RecPage pg;
  int n = recs_page_build(&cx, g_in, NR, &pg);
  CHECK(n == 36 && pg.n == 36, "page: filled to maxRows 36 (%d)", n);
  int small = 0, thinOn = 0;
  for (int i = 0; i < pg.n; i++) {
    if (pg.rows[i].n < 8)
      small++;
    if (pg.rows[i].in >= 70)
      thinOn++;
  }
  CHECK(small == 0 && thinOn == 0, "page: a row needs >= 8 cards; the two 7-card rows are dropped");
  const RecRowDef *D0 = g_in[pg.rows[0].in].def;
  CHECK(D0->family == RF_PERSONAL, "slots: Top picks first");
  CHECK(g_in[pg.rows[1].in].def->family == RF_BYW && g_in[pg.rows[3].in].def->family == RF_BYW &&
            g_in[pg.rows[7].in].def->family == RF_BYW,
        "slots: Because you watched at 1, 3, 7");
  CHECK(g_in[pg.rows[2].in].def->family == RF_CHART && g_in[pg.rows[5].in].def->family == RF_CHART,
        "slots: charts at 2 and 5");
  CHECK(g_in[pg.rows[11].in].def->family == RF_TREND, "slots: trending at 11");
  int bywCount = 0;
  for (int i = 0; i < pg.n; i++)
    bywCount += g_in[pg.rows[i].in].def->family == RF_BYW;
  CHECK(bywCount == 5, "slots: all five Because-you-watched rows are placed (%d)", bywCount);
  /* dedup */
  {
    int viol2 = 0, violSix = 0, inEx = 0, ranked = 0;
    struct {
      int key;
      int c, six;
    } *tab = calloc(4096, sizeof *tab);
    int nt = 0;
#define TAB(k)                           \
  ({                                     \
    int _k = (k), _i = 0;                \
    while (_i < nt && tab[_i].key != _k) \
      _i++;                              \
    if (_i == nt) {                      \
      tab[nt].key = _k;                  \
      nt++;                              \
    }                                    \
    &tab[_i];                            \
  })
    for (int i = 0; i < 8; i++) {
      TAB(bill[i])->c = 1;
      TAB(bill[i])->six = 1;
    }
    for (int i = 0; i < pg.n; i++) {
      const RecRowDef *D = g_in[pg.rows[i].in].def;
      int rk = D->flags & RECS_F_RANK;
      ranked += rk != 0;
      for (int q = 0; q < pg.rows[i].n; q++) {
        const RecCand *c = &g_in[pg.rows[i].in].cards[pg.rows[i].idx[q]];
        int key = recs_key(c->tmdb, c->kind);
        for (int e = 0; e < 6; e++)
          if (ex[e] == key)
            inEx++;
        __typeof__(tab[0]) *e = TAB(key);
        e->c++;
        if (!rk && q < 6) {
          if (e->six)
            violSix++;
          e->six = 1;
        } else if (q < 6)
          e->six = 1;
      }
    }
    for (int i = 0; i < nt; i++)
      if (tab[i].c > 2)
        viol2++;
    CHECK(viol2 == 0, "dedup: no title more than twice on the page (billboard counted)");
    CHECK(violSix == 0, "dedup: no title in the first six cards of two rows (chart rows exempt)");
    CHECK(inEx == 0, "dedup: watched / dismissed titles never appear");
    free(tab);
  }
  /* family variety */
  {
    int run = 1, maxrun = 1;
    for (int i = 1; i < pg.n; i++) {
      run = g_in[pg.rows[i].in].def->family == g_in[pg.rows[i - 1].in].def->family ? run + 1 : 1;
      if (run > maxrun)
        maxrun = run;
    }
    CHECK(maxrun <= 3, "variety: no run of more than 3 rows of the same family (%d)", maxrun);
  }
  /* ranking inside a row */
  {
    const RecPageRow *r = NULL;
    for (int i = 0; i < pg.n; i++)
      if (g_in[pg.rows[i].in].def->family == RF_LANG_GENRE) {
        r = &pg.rows[i];
        break;
      }
    int sorted = 1;
    if (r)
      for (int q = 1; q < r->n; q++) {
        const RecCand *a = &g_in[r->in].cards[r->idx[q - 1]], *b = &g_in[r->in].cards[r->idx[q]];
        /* the first-six demotion can break the order at the seam; check inside the first six only */
        if (q < 6 && recs_title_score(&t, a, cx.today) < recs_title_score(&t, b, cx.today) - 1e-9)
          sorted = 0;
      }
    CHECK(r && sorted, "ranking: a normal row is ordered by the taste score");
    const RecPageRow *ch = &pg.rows[2];
    int inOrder = 1, last = -1;
    for (int q = 0; q < ch->n; q++) {
      if (ch->idx[q] <= last)
        inOrder = 0;
      last = ch->idx[q];
    }
    CHECK(inOrder && ch->n <= 10, "ranking: a chart keeps TMDB's order and holds at most 10 (%d)", ch->n);
  }
  /* maxRows */
  {
    RecPageCtx c2 = cx;
    c2.maxRows = 20;
    int k = recs_page_build(&c2, g_in, NR, &pg);
    CHECK(k == 20, "maxRows: 20 on a low-memory device (%d)", k);
    c2.maxRows = 500;
    k = recs_page_build(&c2, g_in, NR, &pg);
    CHECK(k <= RECS_PAGE_MAX && k > 36, "maxRows: clamped to the page size (%d)", k);
  }
  /* too few candidates */
  {
    RecPageCtx c2 = cx;
    int k = recs_page_build(&c2, g_in, 4, &pg);
    CHECK(k >= 1 && k <= 4, "few candidates: the page just ends (%d)", k);
    k = recs_page_build(&c2, g_in, 0, &pg);
    CHECK(k == 0, "no candidates: empty page");
  }
  /* a row full of excluded titles is dropped */
  {
    int allEx[20];
    for (int i = 0; i < 20; i++)
      allEx[i] = recs_key(g_cards[30][i].tmdb, g_cards[30][i].kind);
    RecPageCtx c2 = cx;
    c2.exclude = allEx;
    c2.nexclude = 20;
    c2.nbill = 0;
    int k = recs_page_build(&c2, g_in, NR, &pg), on = 0;
    for (int i = 0; i < k; i++)
      on += pg.rows[i].in == 30;
    CHECK(on == 0, "exclusions: a row whose titles are all excluded is dropped");
  }
  /* timing */
  {
    page_fixture(0);
    struct timespec a, b;
    int reps = 300;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < reps; i++)
      recs_page_build(&cx, g_in, NR, &pg);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double us = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / 1e3 / reps;
#ifdef __SANITIZE_ADDRESS__
    double limit = 4000;
#else
    double limit = 1000;
#endif
    printf("timing: page build of %d candidate rows x 20 cards: %.0f us per build%s\n", NR, us,
           limit > 1000 ? " (AddressSanitizer build: the limit is relaxed)" : "");
    CHECK(us < limit, "timing: page build under %.0f us (%.0f us)", limit, us);
  }
}

static void home_synthetic(void) {
  printf("\n--- Home rows engine ---\n");
  t_parse_seed();
  t_taste_langs();
  t_rows();
  t_byw_top();
  t_page();
}

/* ================= --home DIR: the whole Home pipeline over cached TMDB answers ================= */
typedef struct {
  uint32_t h[5];
  uint64_t len;
  unsigned char buf[64];
  int nb;
} Sha1;
static uint32_t rol(uint32_t x, int n) {
  return (x << n) | (x >> (32 - n));
}
static void sha1_block(Sha1 *s, const unsigned char *p) {
  uint32_t w[80], a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
  for (int i = 16; i < 80; i++)
    w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20) {
      f = (b & c) | (~b & d);
      k = 0x5A827999;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDC;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6;
    }
    uint32_t t = rol(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = rol(b, 30);
    b = a;
    a = t;
  }
  s->h[0] += a;
  s->h[1] += b;
  s->h[2] += c;
  s->h[3] += d;
  s->h[4] += e;
}
static void sha1_hex18(const char *msg, char *out) {
  Sha1 s = { { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 }, 0, { 0 }, 0 };
  size_t n = strlen(msg);
  for (size_t i = 0; i < n; i++) {
    s.buf[s.nb++] = (unsigned char)msg[i];
    if (s.nb == 64) {
      sha1_block(&s, s.buf);
      s.nb = 0;
    }
  }
  s.len = (uint64_t)n * 8;
  s.buf[s.nb++] = 0x80;
  if (s.nb > 56) {
    while (s.nb < 64)
      s.buf[s.nb++] = 0;
    sha1_block(&s, s.buf);
    s.nb = 0;
  }
  while (s.nb < 56)
    s.buf[s.nb++] = 0;
  for (int i = 0; i < 8; i++)
    s.buf[56 + i] = (unsigned char)(s.len >> (56 - 8 * i));
  sha1_block(&s, s.buf);
  char hex[48];
  for (int i = 0; i < 5; i++)
    snprintf(hex + i * 8, 9, "%08x", s.h[i]);
  memcpy(out, hex, 18);
  out[18] = 0;
}
static cJSON *fetch_path(const char *dir, const char *path) {
  char h[24], f[32];
  sha1_hex18(path, h);
  snprintf(f, sizeof f, "%s.json", h);
  return load(dir, f);
}
static void enc_uri(const char *s, char *out, size_t n) { /* JavaScript's encodeURIComponent */
  size_t o = 0;
  for (; *s && o + 4 < n; s++) {
    unsigned char c = (unsigned char)*s;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-_.!~*'()", c))
      out[o++] = (char)c;
    else
      o += (size_t)snprintf(out + o, n - o, "%%%02X", c);
  }
  out[o] = 0;
}

static RecCand *g_pool;
static int g_npool, g_capPool;
static int pool_has(int key) {
  for (int i = 0; i < g_npool; i++)
    if (recs_key(g_pool[i].tmdb, g_pool[i].kind) == key)
      return 1;
  return 0;
}
static void pool_load(const char *dir) {
  DIR *d = opendir(dir);
  if (!d)
    return;
  struct dirent *e;
  g_capPool = 20000;
  g_pool = malloc(sizeof(RecCand) * (size_t)g_capPool);
  while ((e = readdir(d))) {
    size_t l = strlen(e->d_name);
    if (l < 6 || strcmp(e->d_name + l - 5, ".json"))
      continue;
    cJSON *j = load(dir, e->d_name);
    if (!j)
      continue;
    const cJSON *r = cJSON_GetObjectItem(j, "results");
    if (cJSON_IsArray(r) && !cJSON_GetObjectItem(j, "recommendations") && cJSON_GetArraySize(r) > 0) {
      const cJSON *f = cJSON_GetArrayItem(r, 0);
      char kind = cJSON_GetObjectItem(f, "title") ? 'm' : cJSON_GetObjectItem(f, "name") ? 't' : 0;
      if (kind && !cJSON_GetObjectItem(f, "media_type")) {
        RecCand tmp[RECS_LIST];
        int n = recs_parse(r, kind, tmp, RECS_LIST);
        for (int i = 0; i < n && g_npool < g_capPool; i++)
          if (!pool_has(recs_key(tmp[i].tmdb, tmp[i].kind)))
            g_pool[g_npool++] = tmp[i];
      }
    }
    cJSON_Delete(j);
  }
  closedir(d);
}
static const char *qval(const char *path, const char *name, char *buf, size_t n) {
  char pat[64];
  snprintf(pat, sizeof pat, "%s=", name);
  const char *p = path;
  size_t pl = strlen(pat);
  while ((p = strstr(p, pat))) {
    if (p[-1] == '?' || p[-1] == '&') {
      p += pl;
      size_t k = strcspn(p, "&");
      if (k >= n)
        k = n - 1;
      memcpy(buf, p, k);
      buf[k] = 0;
      return buf;
    }
    p++;
  }
  return NULL;
}
static int g_sortVote;
static int pool_cmp(const void *a, const void *b) {
  const RecCand *x = a, *y = b;
  double u = g_sortVote ? x->vote : x->pop, v = g_sortVote ? y->vote : y->pop;
  return u < v ? 1 : u > v ? -1 : 0;
}
/* what /discover would say, approximately, from the cached cards: kind, language, genre gate, vote floor, sort, dates
 */
static int synth_discover(const char *path, RecCand *out, int max) {
  char kind = !strncmp(path, "/discover/movie", 15) ? 'm' : !strncmp(path, "/discover/tv", 12) ? 't' : 0;
  char b[160], g[160];
  if (!kind || strstr(path, "with_cast") || strstr(path, "with_crew") || strstr(path, "with_watch_providers"))
    return 0; /* not emulable offline */
  const char *lang = qval(path, "with_original_language", b, sizeof b), *gen = qval(path, "with_genres", g, sizeof g);
  char vb[32], sb[48], d1[16], d2[16];
  const char *vm = qval(path, "vote_count.gte", vb, sizeof vb), *vx = qval(path, "vote_count.lte", sb, sizeof sb);
  const char *dk = kind == 'm' ? "primary_release_date" : "first_air_date";
  char k1[48], k2[48];
  snprintf(k1, sizeof k1, "%s.gte", dk);
  snprintf(k2, sizeof k2, "%s.lte", dk);
  const char *dg = qval(path, k1, d1, sizeof d1), *dl = qval(path, k2, d2, sizeof d2);
  char so[40];
  const char *sort = qval(path, "sort_by", so, sizeof so);
  RecCand *tmp = malloc(sizeof(RecCand) * (size_t)(g_npool ? g_npool : 1));
  int n = 0;
  for (int i = 0; i < g_npool; i++) {
    const RecCand *c = &g_pool[i];
    if (c->kind != kind)
      continue;
    if (lang && strcmp(c->lang, lang))
      continue;
    if (vm && c->votes < atoi(vm))
      continue;
    if (vx && c->votes > atoi(vx))
      continue;
    if (dg && strcmp(c->date, dg) < 0)
      continue;
    if (dl && strcmp(c->date, dl) > 0)
      continue;
    if (gen) {
      int any = strchr(gen, '|') != NULL, ok = !any, cnt = 0;
      const char *p = gen;
      while (*p) {
        int id = atoi(p), has_ = 0;
        for (int q = 0; q < c->ng; q++)
          has_ |= c->genres[q] == id;
        cnt++;
        if (any)
          ok |= has_;
        else
          ok &= has_;
        while (*p && *p != '|' && *p != ',')
          p++;
        if (*p)
          p++;
      }
      if (!cnt || !ok)
        continue;
    }
    char excl_[64];
    if (qval(path, "without_genres", excl_, sizeof excl_)) {
      int bad = 0;
      const char *p = excl_;
      while (*p) {
        int id = atoi(p);
        for (int q = 0; q < c->ng; q++)
          bad |= c->genres[q] == id;
        while (*p && *p != ',')
          p++;
        if (*p)
          p++;
      }
      if (bad)
        continue;
    }
    tmp[n++] = *c;
  }
  g_sortVote = sort && !strncmp(sort, "vote_average", 12);
  qsort(tmp, (size_t)n, sizeof *tmp, pool_cmp);
  int m = n < max ? n : max;
  memcpy(out, tmp, sizeof *out * (size_t)m);
  free(tmp);
  return m;
}

static void tally(const RecCand *c, int n, char *out, size_t sz) {
  char l[6][4];
  int k[6] = { 0 }, nl = 0;
  for (int i = 0; i < n; i++) {
    int j = 0;
    while (j < nl && strcmp(l[j], c[i].lang))
      j++;
    if (j == nl) {
      if (nl == 6)
        continue;
      snprintf(l[nl++], 4, "%s", c[i].lang);
    }
    k[j]++;
  }
  for (int a = 0; a < nl; a++)
    for (int b = a + 1; b < nl; b++)
      if (k[b] > k[a]) {
        int t = k[a];
        k[a] = k[b];
        k[b] = t;
        char tl[4];
        memcpy(tl, l[a], 4);
        memcpy(l[a], l[b], 4);
        memcpy(l[b], tl, 4);
      }
  size_t o = 0;
  out[0] = 0;
  for (int a = 0; a < nl && a < 4; a++)
    o += (size_t)snprintf(out + o, sz - o, "%s%s %d", a ? ", " : "", l[a], k[a]);
}

static RecCand g_rowCards[RECS_PAGE_IN][RECS_ROW_CARDS];
static int home_real(const char *dir) {
  static const struct {
    const char *name;
    char kind;
    double age, frac;
  } H[] = { { "My Life with the Walter Boys", 't', 3, 1.0 },
            { "English Vinglish", 'm', 6, 0.9 },
            { "Dil Dhadakne Do", 'm', 10, 1.0 },
            { "Musafir Cafe", 't', 14, 1.0 },
            { "Salaam Namaste", 'm', 18, 1.0 },
            { "House of Cards", 't', 5, 0.6 },
            { "Ocean's Eleven", 'm', 9, 0.5 },
            { "The Resident", 't', 30, 0.3 } };
  static RecSeedX seeds[8];
  static RecCand rb[8][RECS_LIST], sb[8][RECS_LIST];
  int ns = 0;
  long today = recs_days("2026-11-09");
  for (size_t i = 0; i < sizeof H / sizeof H[0]; i++) {
    char q[200], path[300];
    enc_uri(H[i].name, q, sizeof q);
    snprintf(path, sizeof path, "/search/%s?query=%s", H[i].kind == 'm' ? "movie" : "tv", q);
    cJSON *sr = fetch_path(dir, path);
    const cJSON *f = sr ? cJSON_GetArrayItem(cJSON_GetObjectItem(sr, "results"), 0) : NULL;
    if (!f) {
      printf("no cached search for %s\n", H[i].name);
      cJSON_Delete(sr);
      continue;
    }
    snprintf(path, sizeof path, "/%s/%d?append_to_response=keywords,credits,recommendations,similar,external_ids",
             H[i].kind == 'm' ? "movie" : "tv", cJSON_GetObjectItem(f, "id")->valueint);
    cJSON_Delete(sr);
    cJSON *dj = fetch_path(dir, path);
    if (!dj) {
      printf("no cached details for %s\n", H[i].name);
      continue;
    }
    RecSeedX *s = &seeds[ns];
    memset(s, 0, sizeof *s);
    if (!recs_parse_seed(dj, H[i].kind, s, rb[ns], RECS_LIST, sb[ns], RECS_LIST)) {
      cJSON_Delete(dj);
      continue;
    }
    s->ageDays = H[i].age;
    s->w = recs_seed_weight(H[i].age, H[i].frac, 0, H[i].kind == 't', H[i].kind == 't' && H[i].frac >= 0.3 ? 3 : 0);
    printf("seed %-30.30s %s w=%.2f kw=%d cast=%d dir=%d recs=%d sim=%d %s\n", s->title, s->lang, s->w, s->nkw,
           s->ncast, s->ndir, s->nrecs, s->nsim, s->imdb);
    cJSON_Delete(dj);
    ns++;
  }
  if (!ns) {
    printf("nothing cached in %s\n", dir);
    return 1;
  }
  pool_load(dir);
  printf("offline pool: %d distinct cached titles\n", g_npool);
  static RecTaste t;
  recs_taste_build(seeds, ns, &t);
  RecLang L[RECS_MAX_LANGS];
  int nl = recs_langs_allowed("", "en", "en", &t, L);
  printf("languages:");
  for (int i = 0; i < nl; i++)
    printf(" %s(%s)", L[i].code, L[i].strong ? "strong" : "weak");
  printf("\n");
  RecRowCtx cx = {
    .region = "IE", .today = today, .langs = L, .nlangs = nl, .taste = &t, .seeds = seeds, .nseeds = ns
  };
  static RecRowDef defs[RECS_PAGE_IN];
  int nd = recs_rows_build(&cx, defs, RECS_PAGE_IN);
  int ex[16];
  for (int i = 0; i < ns; i++)
    ex[i] = recs_key(seeds[i].tmdb, seeds[i].kind);
  RecPrefs pf = { .nexclude = ns, .exclude = ex, .today = today };
  static RecCand picks[60], ownBuf[8][RECS_LIST];
  static RecRowIn in[RECS_PAGE_IN];
  int exact = 0, synth = 0, none = 0, np = 0;
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < nd; i++) {
      if ((pass == 0) != (defs[i].family == RF_BYW) && !(pass == 1))
        continue; /* pass 0: the byw rows only, their lists feed Top picks */
      if (pass == 1 && defs[i].family == RF_BYW)
        continue;
      if (pass == 1 && !np)
        np = recs_top_picks(seeds, ns, &t, &pf, 28, picks, 60);
      RecRowDef *d = &defs[i];
      int n = 0;
      in[i].def = d;
      in[i].cards = g_rowCards[i];
      if (d->family == RF_PERSONAL) {
        for (int k = 8; k < np && n < RECS_ROW_CARDS; k++)
          g_rowCards[i][n++] = picks[k];
      } else {
        static RecCand tmp[RECS_ROW_CARDS];
        cJSON *r = fetch_path(dir, d->path);
        int nt = 0;
        if (r) {
          nt = recs_parse(cJSON_GetObjectItem(r, "results"), d->kind, tmp, RECS_LIST);
          exact++;
          cJSON_Delete(r);
        } else {
          nt = synth_discover(d->path, tmp, RECS_LIST);
          if (nt)
            synth++;
          else
            none++;
        }
        if (d->family == RF_BYW) {
          memcpy(ownBuf[d->seed], tmp, sizeof(RecCand) * (size_t)nt);
          seeds[d->seed].own = ownBuf[d->seed];
          seeds[d->seed].nown = nt;
          n = recs_byw_rank(&seeds[d->seed], &pf, tmp, nt, g_rowCards[i], RECS_ROW_CARDS);
        } else {
          n = nt < RECS_ROW_CARDS ? nt : RECS_ROW_CARDS;
          memcpy(g_rowCards[i], tmp, sizeof(RecCand) * (size_t)n);
        }
      }
      in[i].n = n;
    }
  printf(
      "%d candidate rows: %d answered from the exact cached request, %d approximated offline from cached cards, %d with no data offline\n",
      nd, exact, synth, none);
  int bill[8], nb = np < 8 ? np : 8;
  for (int i = 0; i < nb; i++)
    bill[i] = recs_key(picks[i].tmdb, picks[i].kind);
  RecPageCtx pc = {
    .taste = &t, .today = today, .maxRows = 36, .bill = bill, .nbill = nb, .exclude = ex, .nexclude = ns
  };
  static RecPage pg;
  int rows = recs_page_build(&pc, in, nd, &pg);
  char tl[96];
  tally(picks, nb, tl, sizeof tl);
  printf("\n== Billboard (%d): ", nb);
  for (int i = 0; i < nb; i++)
    printf("%s%s", i ? " | " : "", picks[i].title);
  printf("\n   languages: %s\n", tl);
  static const char *FAM[] = { "lang-genre", "acclaim",  "gems",  "new",   "decade", "person",  "mood",
                               "season",     "provider", "chart", "trend", "byw",    "personal" };
  for (int r = 0; r < rows; r++) {
    const RecPageRow *pr = &pg.rows[r];
    const RecCand *cards = in[pr->in].cards;
    static RecCand sel[RECS_PAGE_CARDS];
    for (int q = 0; q < pr->n; q++)
      sel[q] = cards[pr->idx[q]];
    tally(sel, pr->n, tl, sizeof tl);
    printf("%2d [%s] %s (%d cards; %s)\n     ", r, FAM[in[pr->in].def->family], in[pr->in].def->title, pr->n, tl);
    for (int q = 0; q < 5 && q < pr->n; q++)
      printf("%s%s", q ? " | " : "", sel[q].title);
    printf("\n");
  }
  free(g_pool);
  return 0;
}

/* ---- Main and Second language (Settings > For You) ---- */
static void t_picks(void) {
  RecSeedX q[4];
  RecTaste t;
  for (int i = 0; i < 4; i++)
    mkseed(&q[i], 1 + i, 'm', "E", i == 3 ? "ta" : "en", 18, 35, 1.0, 5); /* 75% English, 25% Tamil... */
  q[3].w = 0.05;
  recs_taste_build(q, 4, &t); /* ...made ~2% Tamil */
  RecLang L[RECS_MAX_LANGS];
  int n = recs_langs_allowed("ta,en", "en", "en", &t, L);
  CHECK(n == 2 && !strcmp(L[0].code, "ta") && L[0].pick == 1.0f && !strcmp(L[1].code, "en") && L[1].pick > 0.59f &&
            L[1].pick < 0.61f,
        "picks: main first with 1, second with 0.6");
  n = recs_langs_allowed("", "hi,en,ur", "en", &t, L);
  int any = 0;
  for (int i = 0; i < n; i++)
    any += L[i].pick > 0;
  CHECK(any == 0, "picks: Automatic chooses nothing (no lift)");
  n = recs_langs_add_pick(L, n, "ko", 0.6f);
  int ko = -1;
  for (int i = 0; i < n; i++)
    if (!strcmp(L[i].code, "ko"))
      ko = i;
  CHECK(n == 3 && ko >= 0 && L[ko].strong && L[ko].pick > 0.5f,
        "picks: a second language on top of Automatic takes the last unchosen place (%d)", n);
  /* rows: Tamil chosen as main with almost no Tamil history ranks like a main language */
  {
    RecLang P[2] = { { "ta", 1, 1.0f }, { "en", 1, 0.6f } };
    RecRowCtx cx = { .region = "IN", .today = recs_days("2026-11-09"), .langs = P, .nlangs = 2, .taste = &t };
    RecRowDef *d = malloc(sizeof(RecRowDef) * 160);
    int k = recs_rows_build(&cx, d, 160);
    double taMax = 0, enDec = 0;
    int ta = 0;
    for (int i = 0; i < k; i++) {
      if (!strcmp(d[i].lang, "ta") && d[i].family == RF_LANG_GENRE) {
        ta++;
        if (d[i].affinity > taMax)
          taMax = d[i].affinity;
      }
      if (!strcmp(d[i].lang, "en") && d[i].family == RF_DECADE)
        enDec++;
    }
    CHECK(ta >= 6 && taMax >= 0.9,
          "picks: main-language genre rows are there, the best-fitting one near the top (%d rows, best %.2f)", ta,
          taMax);
    double taNew = 0;
    for (int i = 0; i < k; i++)
      if (!strcmp(d[i].lang, "ta") && d[i].family == RF_NEW)
        taNew = d[i].affinity;
    CHECK(taNew >= 0.8, "picks: New Tamil films ranks near the top (%.2f)", taNew);
    CHECK(enDec == 0, "picks: the second language has no decade rows");
    RecLang A[2] = { { "ta", 1, 0 }, { "en", 1, 0 } };
    cx.langs = A;
    int k2 = recs_rows_build(&cx, d, 160);
    double old = 0;
    for (int i = 0; i < k2; i++)
      if (!strcmp(d[i].lang, "ta") && d[i].family == RF_NEW)
        old = d[i].affinity;
    CHECK(old < taNew - 0.3, "picks: without the choice the same row scores lower (%.2f vs %.2f)", old, taNew);
    free(d);
  }
  /* page: low-affinity Tamil rows get spread down the page when Tamil is the main language */
  {
    page_fixture(0);
    for (int r = 9; r < 40; r++) {
      snprintf(g_defs[r].lang, 4, "%s", r % 6 == 0 ? "ta" : "en");
      if (r % 6 == 0)
        g_defs[r].affinity = 0.1f;
    }
    int bill[1] = { 0 };
    RecPageCtx cx = { .taste = &t, .today = recs_days("2026-11-09"), .maxRows = 36, .bill = bill, .nbill = 0 };
    static RecPage pg;
    int k = recs_page_build(&cx, g_in, NR, &pg), firstTa = -1;
    for (int i = 0; i < k && firstTa < 0; i++)
      if (!strcmp(g_in[pg.rows[i].in].def->lang, "ta"))
        firstTa = i;
    CHECK(firstTa < 0 || firstTa > 12, "picks: without a choice the Tamil rows sink (first at %d)", firstTa);
    RecPage base = pg;
    RecLang Z[1] = { { "ta", 1, 0 } };
    cx.langs = Z;
    cx.nlangs = 1;
    k = recs_page_build(&cx, g_in, NR, &pg);
    CHECK(k == base.n && !memcmp(pg.rows, base.rows, sizeof pg.rows[0] * (size_t)k),
          "picks: a language nobody chose changes nothing");
    RecLang M[1] = { { "ta", 1, 1.0f } };
    cx.langs = M;
    k = recs_page_build(&cx, g_in, NR, &pg);
    firstTa = -1;
    int gap = 0, last = -1, nta = 0;
    for (int i = 0; i < k; i++)
      if (!strcmp(g_in[pg.rows[i].in].def->lang, "ta")) {
        if (firstTa < 0)
          firstTa = i;
        if (last >= 0 && i - last > gap)
          gap = i - last;
        last = i;
        nta++;
      }
    CHECK(firstTa >= 0 && firstTa <= 6, "picks: a main-language row within the first 6 rows (%d)", firstTa);
    CHECK(nta >= 5 && gap <= 5, "picks: main-language rows spread down the page (%d rows, widest gap %d)", nta, gap);
  }
  /* Top picks: a chosen language gets places early even with little history */
  {
    static RecSeedX s[3];
    static RecCand rc[3][20];
    RecCand out[60];
    for (int i = 0; i < 3; i++) {
      mkseed(&s[i], 10 + i, 'm', "S", "en", 18, 0, 1.0, 5);
      for (int j = 0; j < 20; j++)
        mk(&rc[i][j], 1000 + i * 40 + j, 'm', "c", j >= 14 ? "ta" : "en", 18, 0, 7, 300, "2025-05-05");
      s[i].recs = rc[i];
      s[i].nrecs = 20;
    }
    RecTaste te;
    recs_taste_build(s, 3, &te);
    RecPrefs p = { .today = recs_days("2026-11-09") };
    int m = recs_top_picks(s, 3, &te, &p, 28, out, 60), ta0 = 0, ta1 = 0;
    for (int i = 0; i < 10 && i < m; i++)
      ta0 += !strcmp(out[i].lang, "ta");
    snprintf(p.langs[0], 4, "ta");
    p.langPick[0] = 1;
    p.nlangs = 1;
    m = recs_top_picks(s, 3, &te, &p, 28, out, 60);
    for (int i = 0; i < 10 && i < m; i++)
      ta1 += !strcmp(out[i].lang, "ta");
    CHECK(ta1 >= 3 && ta1 > ta0, "picks: Tamil in the first 10 Top picks (%d, was %d)", ta1, ta0);
  }
}

int main(int argc, char **argv) {
  if (argc > 2 && !strcmp(argv[1], "--real"))
    return real(argv[2]);
  if (argc > 2 && !strcmp(argv[1], "--home"))
    return home_real(argv[2]);
  synthetic();
  home_synthetic();
  t_picks();
  printf("\n%s (%d failed)\n", fails ? "FAILED" : "all passed", fails);
  return fails ? 1 : 0;
}
