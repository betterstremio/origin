/* The recommendation maths (see recs_core.h). No app state, no I/O: tests/recs runs it on fixtures. */
#include "core/recs_core.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* seed, genre, lang, quality, fresh, old (per year past 8, max 10x), disc (a discover rank counts this much
   of a recommendation rank) */
const RecWeights RECS_W = { 1.0, 0.3, 0.35, 0.1, 0.1, 0.02, 0.6 };

double recs_seed_weight(double ageDays, double frac, int marked, int isSeries, int eps) {
  if (ageDays < 0) ageDays = 0;
  double comp = marked || frac >= 0.70 ? 1.0 : frac >= 0.20 ? 0.5 : frac >= 0.05 ? 0.25 : 0.0;
  if (isSeries && (eps >= 1 || frac >= 0.05) && comp < 0.5) comp = 0.5; /* a series being watched: what it is like matters now */
  if (isSeries && eps >= 3) comp = comp < 1.0 ? 1.0 : comp; /* three episodes in: they like it, whatever the last one's progress */
  double w = pow(0.5, ageDays / RECS_HALF_LIFE_D) * comp;
  if (isSeries && eps >= 3) w *= 1.3;
  return w;
}

int recs_key(int tmdb, char kind) { return kind == 't' ? -tmdb : tmdb; }

long recs_days(const char *s) {
  int y, m, d; if (!s || sscanf(s, "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12) return 0;
  /* days from civil (Howard Hinnant) */
  y -= m <= 2; long era = (y >= 0 ? y : y - 399) / 400; long yoe = y - era * 400;
  long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static const char *jstr(const cJSON *o, const char *k) { const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k); return cJSON_IsString(v) && v->valuestring ? v->valuestring : ""; }
static double jnum(const cJSON *o, const char *k) { const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k); return cJSON_IsNumber(v) ? v->valuedouble : 0; }

int recs_parse(const cJSON *res, char kind, RecCand *out, int max) {
  int n = 0; const cJSON *e;
  cJSON_ArrayForEach(e, res) {
    if (n >= max) break;
    char k = kind; const char *mt = jstr(e, "media_type");
    if (!k) k = !strcmp(mt, "movie") ? 'm' : !strcmp(mt, "tv") ? 't' : 0;
    if (!k) continue;                                        /* a person, a collection */
    const char *poster = jstr(e, "poster_path"), *bd = jstr(e, "backdrop_path");
    if (!poster[0] || !bd[0]) continue;                      /* no art: it would look broken on Home */
    RecCand *c = &out[n]; memset(c, 0, sizeof *c);
    c->tmdb = (int)jnum(e, "id"); c->kind = k; if (c->tmdb <= 0) continue;
    snprintf(c->title, sizeof c->title, "%s", k == 'm' ? jstr(e, "title") : jstr(e, "name"));
    snprintf(c->poster, sizeof c->poster, "%s", poster); snprintf(c->backdrop, sizeof c->backdrop, "%s", bd);
    snprintf(c->date, sizeof c->date, "%s", k == 'm' ? jstr(e, "release_date") : jstr(e, "first_air_date"));
    snprintf(c->lang, sizeof c->lang, "%s", jstr(e, "original_language"));
    snprintf(c->overview, sizeof c->overview, "%s", jstr(e, "overview"));
    const cJSON *g, *gs = cJSON_GetObjectItemCaseSensitive(e, "genre_ids");
    cJSON_ArrayForEach(g, gs) if (cJSON_IsNumber(g) && c->ng < 8) c->genres[c->ng++] = g->valueint;
    c->adult = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(e, "adult"));
    c->vote = (float)jnum(e, "vote_average"); c->votes = (int)jnum(e, "vote_count"); c->pop = (float)jnum(e, "popularity");
    n++;
  }
  return n;
}

/* ---- taste: genre and language shares over the seeds, by weight ---- */
typedef struct { int gid[RECS_GENRES]; double gw[RECS_GENRES]; int ng; double gmax; char lang[16][4]; double lw[16]; int nl; double wsum; } Taste;

static void taste_build(const RecSeed *s, int ns, Taste *t) {
  memset(t, 0, sizeof *t);
  for (int i = 0; i < ns; i++) {
    double w = s[i].w; if (w <= 0) continue; t->wsum += w;
    for (int g = 0; g < s[i].ng; g++) { int k = 0; while (k < t->ng && t->gid[k] != s[i].genres[g]) k++;
      if (k == t->ng) { if (t->ng == RECS_GENRES) continue; t->gid[t->ng++] = s[i].genres[g]; } t->gw[k] += w; }
    if (s[i].lang[0]) {
      int k = 0; while (k < t->nl && strcmp(t->lang[k], s[i].lang)) k++;
      if (k == t->nl && t->nl < 16) snprintf(t->lang[t->nl++], 4, "%s", s[i].lang);
      if (k < 16) t->lw[k] += w;
    }
  }
  for (int k = 0; k < t->ng; k++) if (t->gw[k] > t->gmax) t->gmax = t->gw[k];
}
static double taste_genre(const Taste *t, const RecCand *c) {
  if (!c->ng || t->gmax <= 0) return 0;
  double s = 0;
  for (int g = 0; g < c->ng; g++) for (int k = 0; k < t->ng; k++) if (t->gid[k] == c->genres[g]) { s += t->gw[k] / t->gmax; break; }
  return s / c->ng;
}
static double taste_lang(const Taste *t, const RecPrefs *p, const RecCand *c) {
  double pick = 0, share = 0;
  for (int i = 0; i < p->nlangs; i++) if (!strcmp(p->langs[i], c->lang)) { pick = 1; break; }
  if (t->wsum > 0) for (int k = 0; k < t->nl; k++) if (!strcmp(t->lang[k], c->lang)) { share = t->lw[k] / t->wsum; break; }
  return 0.5 * pick + 0.5 * share;
}
static int excluded(const RecPrefs *p, int key) { for (int i = 0; i < p->nexclude; i++) if (p->exclude[i] == key) return 1; return 0; }

static double part_rest(const Taste *t, const RecPrefs *p, const RecCand *c) {
  double q = c->vote / 10.0 * (c->votes >= 200 ? 1.0 : c->votes / 200.0);
  double fresh = 0, old = 0; long d = recs_days(c->date);
  if (d && p->today) { long age = p->today - d; fresh = age < 0 || age > 730 ? 0 : 1.0 - age / 730.0;
    double yrs = age / 365.0 - 8; if (yrs > 0) old = yrs > 10 ? 10 : yrs; }
  return RECS_W.genre * taste_genre(t, c) + RECS_W.lang * taste_lang(t, p, c) + RECS_W.quality * q + RECS_W.fresh * fresh - RECS_W.old * old;
}
static int unreleased(const RecPrefs *p, const RecCand *c) {
  long d = recs_days(c->date); if (!d || (p->today && d > p->today)) return 1;
  return c->kind == 'm' && p->minMovieAge > 0 && p->today && p->today - d < p->minMovieAge; /* "Hide films still in cinemas" */
}
static int cmp_score(const void *a, const void *b) { double x = ((const RecCand *)a)->score, y = ((const RecCand *)b)->score; return x < y ? 1 : x > y ? -1 : 0; }

/* every seed's list merged into one scored, de-duplicated array (not yet ordered) */
static int merge(const RecSeed *s, int ns, const RecPrefs *p, const Taste *t, RecCand *all, int max) {
  int n = 0; double wsum = 0; for (int i = 0; i < ns; i++) if (s[i].w > 0) wsum += s[i].w;
  if (wsum <= 0) return 0;
  int *seedKeys = calloc((size_t)ns + 1, sizeof(int)); if (!seedKeys) return 0;
  for (int i = 0; i < ns; i++) seedKeys[i] = recs_key(s[i].tmdb, s[i].kind);
  for (int i = 0; i < ns; i++) {
    if (s[i].w <= 0) continue;
    for (int r2 = 0; r2 < s[i].n + s[i].nd; r2++) {
      int fromDisc = r2 >= s[i].n, r = fromDisc ? r2 - s[i].n : r2; if (r >= RECS_LIST) continue;
      const RecCand *c = fromDisc ? &s[i].disc[r] : &s[i].list[r]; int key = recs_key(c->tmdb, c->kind);
      if (excluded(p, key) || (c->adult && !p->allowAdult) || unreleased(p, c)) continue;
      int self = 0; for (int k = 0; k < ns && !self; k++) self = seedKeys[k] == key; if (self) continue; /* a watched title itself */
      int at = 0; while (at < n && recs_key(all[at].tmdb, all[at].kind) != key) at++;
      if (at == n) { if (n == max) continue; all[n] = *c; all[n].seedPart = 0; all[n].score = 0; all[n].nSeeds = 0; all[n].bestSeed = -1; n++; }
      double contrib = s[i].w * (1.0 - (double)r / RECS_LIST) / wsum * (fromDisc ? RECS_W.disc : 1.0);
      RecCand *a = &all[at]; a->seedPart += contrib; a->nSeeds++;
      if (a->bestSeed < 0 || contrib > a->score) { a->bestSeed = i; a->score = contrib; } /* (score holds the best contribution until scoring) */
    }
  }
  free(seedKeys);
  for (int i = 0; i < n; i++) all[i].score = RECS_W.seed * all[i].seedPart + part_rest(t, p, &all[i]);
  return n;
}

int recs_rank(const RecSeed *s, int ns, const RecPrefs *p, RecCand *out, int max) {
  RecCand *all = malloc(sizeof(RecCand) * RECS_MAX_CAND); if (!all) return 0;
  Taste t; taste_build(s, ns, &t);
  int n = merge(s, ns, p, &t, all, RECS_MAX_CAND);
  qsort(all, (size_t)n, sizeof *all, cmp_score);
  /* diversity: the first 20 take at most 4 per best seed; the rest wait their turn after them */
  int perSeed[RECS_MAX_SEEDS] = { 0 }, m = 0; char *used = calloc((size_t)n + 1, 1);
  for (int i = 0; i < n && m < max && m < 20; i++) { int b = all[i].bestSeed; if (b >= 0 && b < RECS_MAX_SEEDS && perSeed[b] >= 4) continue; if (b >= 0 && b < RECS_MAX_SEEDS) perSeed[b]++; out[m++] = all[i]; used[i] = 1; }
  for (int i = 0; i < n && m < max; i++) if (!used[i]) out[m++] = all[i];
  free(used); free(all); return m;
}

int recs_rank_seed(const RecSeed *seed, const RecSeed *s, int ns, const RecPrefs *p, const int *skip, int nskip, RecCand *out, int max) {
  RecCand *all = malloc(sizeof(RecCand) * RECS_MAX_CAND); if (!all) return 0;
  Taste t; taste_build(s, ns, &t);
  int n = merge(s, ns, p, &t, all, RECS_MAX_CAND), m = 0;
  qsort(all, (size_t)n, sizeof *all, cmp_score);
  double top = 0;
  for (int i = 0; i < n && m < max; i++) {
    int key = recs_key(all[i].tmdb, all[i].kind), in = 0, sk = 0;
    for (int r = 0; r < seed->n && !in; r++) in = recs_key(seed->list[r].tmdb, seed->list[r].kind) == key;
    for (int r = 0; r < seed->nd && !in; r++) in = recs_key(seed->disc[r].tmdb, seed->disc[r].kind) == key;
    for (int k = 0; k < nskip && !sk; k++) sk = skip[k] == key;
    if (!in || sk) continue;
    if (!m) top = all[i].score;
    if (m >= 8 && all[i].score < RECS_ROW_FLOOR * top) break; /* a weak tail ends the row early */
    out[m++] = all[i];
  }
  free(all); return m;
}

int recs_filter(const RecCand *in, int n, const RecPrefs *p, RecCand *out, int max) {
  int m = 0;
  for (int i = 0; i < n && m < max; i++) { const RecCand *c = &in[i];
    if (excluded(p, recs_key(c->tmdb, c->kind)) || (c->adult && !p->allowAdult) || unreleased(p, c)) continue;
    out[m++] = *c; }
  return m;
}

int recs_interleave(const RecCand *const *lists, const int *n, int nl, const RecPrefs *p, RecCand *out, int max) {
  int m = 0, at[8] = { 0 }; if (nl > 8) nl = 8;
  for (int more = 1; more && m < max; ) {
    more = 0;
    for (int l = 0; l < nl && m < max; l++) {
      while (at[l] < n[l]) {
        const RecCand *c = &lists[l][at[l]++]; int key = recs_key(c->tmdb, c->kind), dup = 0;
        for (int k = 0; k < m && !dup; k++) dup = recs_key(out[k].tmdb, out[k].kind) == key;
        if (dup || excluded(p, key) || (c->adult && !p->allowAdult) || unreleased(p, c)) continue;
        out[m++] = *c; more = 1; break;
      }
    }
  }
  return m;
}

/* =====================================================================================================
   Home rows engine (see the header). Everything below is pure: bounded arrays, no globals with state.
   ===================================================================================================== */

/* ---- dates ---- */
static long days_fast(const char *s) {            /* "2026-03-14" -> days since 1970, 0 when unreadable (recs_days without sscanf) */
  if (!s || s[0] < '0' || s[0] > '9' || s[4] != '-' || s[7] != '-') return 0;
  for (int i = 0; i < 10; i++) if (i != 4 && i != 7 && (s[i] < '0' || s[i] > '9')) return 0;
  int y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0'), m = (s[5] - '0') * 10 + (s[6] - '0'), d = (s[8] - '0') * 10 + (s[9] - '0');
  if (m < 1 || m > 12) return 0;
  y -= m <= 2; long era = (y >= 0 ? y : y - 399) / 400; long yoe = y - era * 400;
  long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}
static void civil(long z, int *y, int *m, int *d) {   /* days since 1970 -> y-m-d */
  z += 719468; long era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
  long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  *d = (int)(doy - (153 * mp + 2) / 5 + 1); *m = (int)(mp < 10 ? mp + 3 : mp - 9); *y = (int)(yoe + era * 400 + (*m <= 2));
}
static void ymd(long days, char *buf) { int y, m, d; civil(days, &y, &m, &d); snprintf(buf, 12, "%04d-%02d-%02d", y, m, d); }

/* ---- A. seed details ---- */
static const cJSON *jobj(const cJSON *o, const char *k) { return cJSON_GetObjectItemCaseSensitive(o, k); }
static void set_person(RecPerson *p, const cJSON *e) { p->id = (int)jnum(e, "id"); snprintf(p->name, sizeof p->name, "%s", jstr(e, "name")); }

int recs_parse_seed(const cJSON *root, char kind, RecSeedX *o, RecCand *rb, int mr, RecCand *sb, int ms) {
  if (!cJSON_IsObject(root)) return 0;
  int id = (int)jnum(root, "id"); if (id <= 0) return 0;
  o->tmdb = id; o->kind = kind;
  snprintf(o->title, sizeof o->title, "%s", kind == 'm' ? jstr(root, "title") : jstr(root, "name"));
  snprintf(o->lang, sizeof o->lang, "%s", jstr(root, "original_language"));
  o->ng = 0; const cJSON *g;
  cJSON_ArrayForEach(g, jobj(root, "genres")) { if (o->ng >= 8) break; int gi = (int)jnum(g, "id"); if (gi > 0) o->genres[o->ng++] = gi; }
  const char *dt = kind == 'm' ? jstr(root, "release_date") : jstr(root, "first_air_date");
  o->year = dt[0] >= '0' && dt[0] <= '9' ? atoi(dt) : 0;
  const cJSON *ex = jobj(root, "external_ids"); const char *im = ex ? jstr(ex, "imdb_id") : ""; if (!im[0]) im = jstr(root, "imdb_id");
  snprintf(o->imdb, sizeof o->imdb, "%s", im);
  o->nkw = 0; const cJSON *kws = jobj(root, "keywords"), *ka = NULL;
  if (kws) { ka = jobj(kws, kind == 'm' ? "keywords" : "results"); if (!cJSON_IsArray(ka)) ka = jobj(kws, kind == 'm' ? "results" : "keywords"); }
  cJSON_ArrayForEach(g, ka) { if (o->nkw >= RECS_KW) break; int ki = (int)jnum(g, "id"); if (ki > 0) o->kw[o->nkw++] = ki; }
  o->ncast = o->ndir = 0; const cJSON *cr = jobj(root, "credits"), *e;
  if (cr) {
    cJSON_ArrayForEach(e, jobj(cr, "cast")) { if (o->ncast >= 3) break; if ((int)jnum(e, "id") > 0) set_person(&o->cast[o->ncast++], e); }
    cJSON_ArrayForEach(e, jobj(cr, "crew")) { if (o->ndir >= 2) break; if (!strcmp(jstr(e, "job"), "Director") && (int)jnum(e, "id") > 0) set_person(&o->dir[o->ndir++], e); }
  }
  if (!o->ndir) cJSON_ArrayForEach(e, jobj(root, "created_by")) { if (o->ndir >= 2) break; if ((int)jnum(e, "id") > 0) set_person(&o->dir[o->ndir++], e); }
  const cJSON *rc = jobj(root, "recommendations"), *si = jobj(root, "similar");
  o->recs = rb; o->nrecs = rc && rb ? recs_parse(jobj(rc, "results"), kind, rb, mr) : 0;
  o->sim = sb; o->nsim = si && sb ? recs_parse(jobj(si, "results"), kind, sb, ms) : 0;
  return 1;
}

/* ---- B. taste ---- */
static int find_i(const int *a, int n, int v) { for (int i = 0; i < n; i++) if (a[i] == v) return i; return -1; }

void recs_taste_build(const RecSeedX *s, int ns, RecTaste *t) {
  memset(t, 0, sizeof *t);
  for (int i = 0; i < ns; i++) {
    double w = s[i].w; if (w <= 0) continue;
    t->wsum += w; t->kindw[s[i].kind == 't'] += w;
    for (int g = 0; g < s[i].ng; g++) {
      int k = find_i(t->gid, t->ng, s[i].genres[g]);
      if (k < 0) { if (t->ng == RECS_GENRES) continue; k = t->ng++; t->gid[k] = s[i].genres[g]; }
      t->gw[k] += w;
    }
    if (s[i].lang[0]) {
      int k = 0; while (k < t->nl && strcmp(t->lang[k], s[i].lang)) k++;
      if (k == t->nl) { if (t->nl == 16) goto kws; snprintf(t->lang[t->nl++], 4, "%s", s[i].lang); }
      t->lw[k] += w;
    }
  kws:
    for (int j = 0; j < s[i].nkw; j++) {
      int k = find_i(t->kw, t->nkw, s[i].kw[j]);
      if (k < 0) { if (t->nkw == RECS_T_KW) continue; k = t->nkw++; t->kw[k] = s[i].kw[j]; }
      t->kww[k] += w;
    }
    for (int pass = 0; pass < 2; pass++) {
      int cnt = pass ? s[i].ndir : s[i].ncast; const RecPerson *pp = pass ? s[i].dir : s[i].cast;
      for (int j = 0; j < cnt; j++) {
        int k = -1; for (int q = 0; q < t->np; q++) if (t->pid[q] == pp[j].id && t->pdir[q] == pass) { k = q; break; }
        if (k < 0) { if (t->np == RECS_T_PEOPLE) continue; k = t->np++; t->pid[k] = pp[j].id; t->pdir[k] = (char)pass; snprintf(t->pname[k], sizeof t->pname[k], "%s", pp[j].name); }
        t->pw[k] += pass ? w : w * (1.0 - 0.15 * j);
      }
    }
  }
  for (int k = 0; k < t->ng; k++) if (t->gw[k] > t->gmax) t->gmax = t->gw[k];
}
double recs_taste_lang(const RecTaste *t, const char *lang) {
  if (t->wsum <= 0 || !lang) return 0;
  for (int k = 0; k < t->nl; k++) if (!strcmp(t->lang[k], lang)) return t->lw[k] / t->wsum;
  return 0;
}
double recs_taste_genre(const RecTaste *t, int genre) {
  if (t->gmax <= 0) return 0;
  for (int k = 0; k < t->ng; k++) if (t->gid[k] == genre) return t->gw[k] / t->gmax;
  return 0;
}
double recs_taste_kind(const RecTaste *t, char kind) { return t->wsum > 0 ? t->kindw[kind == 't'] / t->wsum : 0; }

static double score_d(const RecTaste *t, const RecCand *c, long today, long d) {
  double g = 0;
  if (c->ng && t->gmax > 0) {
    for (int i = 0; i < c->ng; i++) for (int k = 0; k < t->ng; k++) if (t->gid[k] == c->genres[i]) { g += t->gw[k]; break; }
    g /= t->gmax * c->ng;
  }
  double ls = 0;
  if (t->wsum > 0) for (int k = 0; k < t->nl; k++) if (!strcmp(t->lang[k], c->lang)) { ls = t->lw[k] / t->wsum; break; }
  double conf = c->votes >= 200 ? 1.0 : c->votes / 200.0;
  double age = d && today ? (today - d) / 365.0 : 10.0; if (age < 0) age = 0;
  double fresh = age < 2 ? 1.0 - age / 2 : 0, old = age - 12; old = old < 0 ? 0 : old > 10 ? 10 : old;
  return 0.45 * g + 0.3 * ls + 0.15 * (c->vote / 10.0) * conf + 0.1 * fresh - 0.02 * old;
}
double recs_title_score(const RecTaste *t, const RecCand *c, long today) { return score_d(t, c, today, days_fast(c->date)); }

/* ---- C. languages ---- */
static int csv_tok(const char **p, char out[4]) {         /* next 2-3 letter code, lower case; 0 at the end */
  const char *s = *p;
  for (;;) {
    while (*s == ',' || *s == ' ' || *s == '|') s++;
    if (!*s) { *p = s; return 0; }
    int n = 0; char b[8];
    while (*s && *s != ',' && *s != ' ' && *s != '|') { if (n < 7) b[n++] = (char)((*s >= 'A' && *s <= 'Z') ? *s + 32 : *s); s++; }
    *p = s; if (n >= 2 && n <= 3) { memcpy(out, b, (size_t)n); out[n] = 0; return 1; }
  }
}
static int langs_add(RecLang *out, int n, const char *code, int strong) {
  for (int i = 0; i < n; i++) if (!strcmp(out[i].code, code)) { if (strong) out[i].strong = 1; return n; }
  if (n >= RECS_MAX_LANGS) return n;
  snprintf(out[n].code, 4, "%s", code); out[n].strong = strong; out[n].pick = 0; return n + 1;
}
int recs_langs_add_pick(RecLang out[RECS_MAX_LANGS], int n, const char *code, float pick) {
  char c[4]; const char *p = code; if (!code || !csv_tok(&p, c)) return n;
  for (int i = 0; i < n; i++) if (!strcmp(out[i].code, c)) { out[i].strong = 1; if (out[i].pick < pick) out[i].pick = pick; return n; }
  int at = n < RECS_MAX_LANGS ? n++ : -1;
  if (at < 0) for (int i = n - 1; i >= 0 && at < 0; i--) if (out[i].pick <= 0) at = i;   /* full: the last one nobody chose */
  if (at < 0) return n;
  snprintf(out[at].code, 4, "%s", c); out[at].strong = 1; out[at].pick = pick; return n;
}
int recs_langs_allowed(const char *ex, const char *region, const char *audio, const RecTaste *t, RecLang out[RECS_MAX_LANGS]) {
  int n = 0; char c[4];
  if (ex) { const char *p = ex; while (csv_tok(&p, c)) { int k = n; n = langs_add(out, n, c, 1); if (n > k) out[k].pick = k == 0 ? 1.0f : 0.6f; } if (n) return n; }
  if (t && t->wsum > 0) {                                  /* history: strong from 25%, largest first */
    int used[16] = { 0 };
    for (;;) {
      int best = -1; for (int k = 0; k < t->nl; k++) if (!used[k] && t->lw[k] / t->wsum >= 0.25 && (best < 0 || t->lw[k] > t->lw[best])) best = k;
      if (best < 0) break;
      used[best] = 1; n = langs_add(out, n, t->lang[best], 1);
    }
  }
  if (audio) { const char *p = audio; if (csv_tok(&p, c)) n = langs_add(out, n, c, 1); }
  if (region) { const char *p = region; while (csv_tok(&p, c)) n = langs_add(out, n, c, 0); }
  return n;
}

/* ---- D. candidate rows ---- */
static const struct { const char *code, *name; } LANG_NAMES[] = {
  { "hi", "Hindi" }, { "en", "English" }, { "ta", "Tamil" }, { "te", "Telugu" }, { "ml", "Malayalam" }, { "kn", "Kannada" }, { "bn", "Bengali" },
  { "mr", "Marathi" }, { "pa", "Punjabi" }, { "ur", "Urdu" }, { "ko", "Korean" }, { "ja", "Japanese" }, { "zh", "Chinese" }, { "es", "Spanish" },
  { "fr", "French" }, { "de", "German" }, { "it", "Italian" }, { "pt", "Portuguese" }, { "ru", "Russian" }, { "tr", "Turkish" }, { "ar", "Arabic" },
  { "nl", "Dutch" }, { "pl", "Polish" }, { "th", "Thai" }, { "id", "Indonesian" }, { "sv", "Swedish" }, { "da", "Danish" }, { "no", "Norwegian" },
  { "fi", "Finnish" }, { "he", "Hebrew" }, { "fa", "Persian" }, { "cs", "Czech" }, { "el", "Greek" }, { "hu", "Hungarian" }, { "vi", "Vietnamese" } };
int recs_lang_count(void) { return (int)(sizeof LANG_NAMES / sizeof LANG_NAMES[0]); }
const char *recs_lang_code(int i) { return i >= 0 && i < recs_lang_count() ? LANG_NAMES[i].code : ""; }
const char *recs_lang_name(const char *code) {
  if (code) for (size_t i = 0; i < sizeof LANG_NAMES / sizeof LANG_NAMES[0]; i++) if (!strcmp(LANG_NAMES[i].code, code)) return LANG_NAMES[i].name;
  return NULL;
}

/* movie genre -> the tv genre that holds it (0: tv has none) */
static int tv_genre(int g) {
  switch (g) {
    case 28: case 12: return 10759; case 878: case 14: return 10765; case 10752: return 10768;
    case 16: case 35: case 80: case 99: case 18: case 10751: case 9648: case 37: return g;
    case 10759: case 10762: case 10763: case 10764: case 10765: case 10766: case 10767: case 10768: return g;
    default: return 0;                                     /* horror, thriller, romance, history, music */
  }
}
static int movie_genre(int g) { return g == 10759 ? 28 : g == 10765 ? 878 : g == 10768 ? 10752 : g; }

typedef struct { int movie, tv; const char *mpat, *tpat; } LgGenre;   /* tv 0 or tpat NULL: films only */
static const LgGenre LG[] = {
  { 28, 10759, "%s action films", "%s action and adventure series" }, { 12, 10759, "%s adventure films", NULL },
  { 35, 35, "%s comedy films", "%s comedy series" }, { 80, 80, "%s crime films", "%s crime series" },
  { 99, 99, "%s documentaries", "%s documentary series" }, { 18, 18, "%s drama films", "%s drama series" },
  { 14, 10765, "%s fantasy films", NULL }, { 27, 0, "%s horror films", NULL }, { 9648, 9648, "%s mystery films", "%s mystery series" },
  { 10749, 0, "%s romance films", NULL }, { 878, 10765, "%s sci-fi films", "%s sci-fi and fantasy series" }, { 53, 0, "%s thriller films", NULL },
  { 10752, 10768, "%s war films", "%s war series" }, { 36, 0, "%s history films", NULL }, { 10402, 0, "%s music films", NULL },
  { 16, 16, "%s animated films", "%s animated series" }, { 10751, 10751, "%s family films", "%s family series" } };
#define NLG ((int)(sizeof LG / sizeof LG[0]))

typedef struct { const RecRowCtx *cx; RecRowDef *out; int max, n; char d0[12]; int likesAnim; int month, day; char langCsv[16]; } Bld;

static const char *trs(const Bld *b, const char *s) { if (!b->cx->tr) return s; const char *r = b->cx->tr(s); return r && r[0] ? r : s; }
/* the translated pattern with its first "%s" replaced by arg (never used as a printf format) */
static void fmt_title(const Bld *b, char *dst, size_t n, const char *pat, const char *arg) {
  const char *p = trs(b, pat), *at = strstr(p, "%s");
  if (!at) { snprintf(dst, n, "%s", p); return; }
  snprintf(dst, n, "%.*s%s%s", (int)(at - p), p, arg ? arg : "", at + 2);
}
static RecRowDef *rd_new(Bld *b, int fam, char kind, int flags, const char *lang, double aff, long age) {
  if (b->n >= b->max) return NULL;
  RecRowDef *r = &b->out[b->n++]; memset(r, 0, sizeof *r);
  r->family = (unsigned char)fam; r->kind = kind; r->flags = (unsigned char)flags; if (lang) snprintf(r->lang, 4, "%s", lang);
  r->affinity = (float)aff; r->maxAge = age; r->seed = -1; return r;
}
static const char *kname(char k) { return k == 'm' ? "movie" : "tv"; }
static const char *excl(const Bld *b) { return b->likesAnim ? "" : "&without_genres=16,10751,10762"; }
static double kshare(const RecTaste *t, char k) { double s = recs_taste_kind(t, k); return s < 0.2 ? 0.2 : s; }

/* mood table. kw ids were looked up once on TMDB (/search/keyword), the name is in the comment. g = genre gate:
   anyOf 1 -> with_genres a|b|c (any of), 0 -> a,b (all of). Every row with a keyword has a gate. */
typedef struct { const char *title; char kind; unsigned char anyOf, status; short rtMax; int g[4]; int kw[3]; } Mood;
static const Mood MOODS[] = {
  { "Love stories with jokes in them", 'm', 0, 0, 0, { 10749, 35 }, { 0 } },
  { "Love stories that leave a mark", 'm', 0, 0, 0, { 10749, 18 }, { 0 } },
  { "Weddings, and everything that goes wrong", 'm', 1, 0, 0, { 10749, 35 }, { 13027 } },            /* wedding */
  { "Secrets in the family", 'm', 1, 0, 0, { 18, 53, 9648 }, { 6625 } },                             /* family secrets */
  { "Friends on the road", 'm', 1, 0, 0, { 35, 18 }, { 7312 } },                                     /* road trip */
  { "Growing up, badly", 't', 1, 0, 0, { 18, 35 }, { 10683 } },                                      /* coming of age */
  { "School, crushes and chaos", 't', 1, 0, 0, { 18, 35 }, { 6270 } },                               /* high school */
  { "One big score", 'm', 1, 0, 0, { 80, 53, 35 }, { 10051 } },                                      /* heist */
  { "Power, money and betrayal", 't', 0, 0, 0, { 18 }, { 6078 } },                                   /* politics */
  { "Hospitals and long shifts", 't', 1, 0, 0, { 18 }, { 11612 } },                                  /* hospital */
  { "Someone is lying", 'm', 1, 0, 0, { 53, 9648 }, { 0 } },
  { "Revenge, served properly", 'm', 1, 0, 0, { 28, 53, 80 }, { 9748 } },                            /* revenge */
  { "From the book", 'm', 0, 0, 0, { 18 }, { 818 } },                                                /* based on novel or book */
  { "It really happened", 'm', 1, 0, 0, { 18, 36, 80 }, { 9672 } },                                  /* based on true story */
  { "Friends who became family", 't', 0, 0, 0, { 35 }, { 6054 } },                                   /* friendship */
  { "One more episode", 't', 0, 3, 0, { 18 }, { 0 } },                                               /* status 3 = ended */
  { "Sitcoms to fall asleep to", 't', 0, 3, 0, { 35 }, { 0 } },
  { "Under an hour and forty", 'm', 0, 0, 100, { 35 }, { 0 } },
  { "Time is the problem", 'm', 1, 0, 0, { 878, 18, 35 }, { 4379 } },                                /* time travel */
  { "A killer on the loose", 'm', 1, 0, 0, { 53, 80, 9648 }, { 10714 } },                            /* serial killer */
  { "Stranded, with no way out", 'm', 1, 0, 0, { 12, 53, 28, 18 }, { 10349 } },                      /* survival */
  { "Small town, big secrets", 't', 1, 0, 0, { 18, 9648, 80 }, { 1415 } },                           /* small town */
  { "Lawyers who will not lose", 't', 1, 0, 0, { 18, 80 }, { 10909, 33519 } },                       /* lawyer, courtroom */
  { "Undercover, and in too deep", 'm', 1, 0, 0, { 28, 80, 53 }, { 1568 } },                         /* undercover */
  { "Life behind bars", 'm', 1, 0, 0, { 18, 80, 53 }, { 378 } },                                     /* prison */
  { "Fighters in the ring", 'm', 1, 0, 0, { 18, 28 }, { 209476 } },                                  /* boxing */
  { "Cricket, with the whole country watching", 'm', 1, 0, 0, { 18, 35, 99 }, { 5719 } },            /* cricket */
  { "Kitchens under pressure", 't', 1, 0, 0, { 18, 35, 99 }, { 18293 } },                            /* chef */
  { "Newsrooms and deadlines", 'm', 1, 0, 0, { 18, 53, 36 }, { 736 } },                              /* journalist */
  { "Crowns and the people behind them", 't', 1, 0, 0, { 18, 36 }, { 4833 } },                       /* royal family */
  { "When the machines wake up", 'm', 1, 0, 0, { 878, 53 }, { 310 } },                               /* artificial intelligence (a.i.) */
  { "The dead are walking", 'm', 1, 0, 0, { 27, 28, 53 }, { 12377 } },                               /* zombie */
  { "Haunted houses", 'm', 1, 0, 0, { 27, 9648 }, { 3358, 162846 } },                                /* haunted house, ghost */
  { "Second chances, late in life", 'm', 1, 0, 0, { 10749, 18, 35 }, { 34004 } },                    /* second chance */
  { "Mafia families", 'm', 1, 0, 0, { 80, 18, 53 }, { 10391 } },                                     /* mafia */
  { "Spies and double lives", 'm', 1, 0, 0, { 28, 53, 18 }, { 470 } },                               /* spy */
  { "Detectives on a cold case", 't', 1, 0, 0, { 80, 9648, 18 }, { 703 } },                          /* detective */
  { "Arranged marriages, and what comes after", 'm', 1, 0, 0, { 10749, 35, 18 }, { 2692 } },         /* arranged marriage */
  { "Out in space, far from home", 'm', 1, 0, 0, { 878, 12 }, { 3801 } },                            /* space travel */
  { "After the end of the world", 'm', 1, 0, 0, { 878, 28, 18 }, { 359337 } },                       /* post-apocalyptic */
};
#define NMOOD ((int)(sizeof MOODS / sizeof MOODS[0]))

/* genre list for a request: ids in order for kind k, joined by sep. Returns the count written (0 = none valid). */
static int gen_join(char *dst, size_t n, const int *g, char kind, char sep) {
  int c = 0; size_t o = 0; dst[0] = 0; int seen[8]; int ns = 0;
  for (int i = 0; i < 4 && g[i]; i++) {
    int x = kind == 't' ? tv_genre(g[i]) : g[i]; if (!x || find_i(seen, ns, x) >= 0) continue;
    seen[ns++] = x; o += (size_t)snprintf(dst + o, n - o, "%s%d", c ? (sep == ',' ? "," : "|") : "", x); c++;
  }
  return c;
}
static double tg2(const RecTaste *t, int g) { double a = recs_taste_genre(t, g), b = recs_taste_genre(t, tv_genre(g)); return a > b ? a : b; }

static int mood_aff(const Bld *b, int i, double *aff) {
  const RecRowCtx *cx = b->cx; const Mood *m = &MOODS[i]; char gs[64]; int nkw = 0; while (nkw < 3 && m->kw[nkw]) nkw++;
  int gc = gen_join(gs, sizeof gs, m->g, m->kind, m->anyOf ? '|' : ',');
  int want = 0; while (want < 4 && m->g[want]) want++;
  if (gc == 0 || (!m->anyOf && gc < want)) return 0;                         /* no valid gate on this kind */
  double fit = 0; int cnt = 0; for (int k = 0; k < 4 && m->g[k]; k++) { fit += tg2(cx->taste, m->g[k]); cnt++; }
  fit = cnt ? fit / cnt : 0.3;
  double kfit = 0; for (int k = 0; k < nkw; k++) if (find_i(cx->taste->kw, cx->taste->nkw, m->kw[k]) >= 0) kfit += 1; kfit = nkw ? kfit / nkw : 0;
  *aff = 0.15 + 0.45 * fit + 0.3 * kfit + 0.1 * recs_taste_kind(cx->taste, m->kind);
  return 1;
}
static int mood_build(Bld *b, int i, RecRowDef *r) {
  const RecRowCtx *cx = b->cx; const Mood *m = &MOODS[i]; char gs[64]; int nkw = 0; double aff;
  if (!mood_aff(b, i, &aff)) return 0;
  while (nkw < 3 && m->kw[nkw]) nkw++;
  gen_join(gs, sizeof gs, m->g, m->kind, m->anyOf ? '|' : ',');
  memset(r, 0, sizeof *r); r->family = RF_MOOD; r->kind = m->kind; r->seed = -1; r->maxAge = RECS_AGE_LIST; r->affinity = (float)aff;
  char lang[40] = "", kws[40] = "", extra[64] = ""; size_t o = 0;
  if (cx->nlangs == 1) snprintf(lang, sizeof lang, "with_original_language=%s&", cx->langs[0].code);   /* TMDB takes ONE code here */
  for (int k = 0; k < nkw; k++) o += (size_t)snprintf(kws + o, sizeof kws - o, "%s%d", k ? "|" : "", m->kw[k]);
  if (m->status) snprintf(extra, sizeof extra, "&with_status=%d", m->status);
  if (m->rtMax) snprintf(extra, sizeof extra, "&with_runtime.gte=60&with_runtime.lte=%d", m->rtMax);
  snprintf(r->path, sizeof r->path, "/discover/%s?%swith_genres=%s%s%s%s%s&sort_by=popularity.desc&vote_count.gte=40", kname(m->kind), lang, gs, excl(b),
           nkw ? "&with_keywords=" : "", kws, extra);
  snprintf(r->key, sizeof r->key, "md|%d|%c|%s|%d", i, m->kind, cx->nlangs == 1 ? cx->langs[0].code : "-", b->likesAnim);
  snprintf(r->title, sizeof r->title, "%s", trs(b, m->title));
  return 1;
}

static void bld_init(Bld *b, const RecRowCtx *cx, RecRowDef *out, int max) {
  memset(b, 0, sizeof *b); b->cx = cx; b->out = out; b->max = max;
  ymd(cx->today, b->d0); int y, m, d; civil(cx->today, &y, &m, &d); b->month = m; b->day = d;
  b->likesAnim = cx->taste && cx->taste->wsum > 0 && recs_taste_genre(cx->taste, 16) * cx->taste->gmax / cx->taste->wsum >= 0.15;
  size_t o = 0; for (int i = 0; i < cx->nlangs; i++) o += (size_t)snprintf(b->langCsv + o, sizeof b->langCsv - o, "%s%s", i ? "," : "", cx->langs[i].code);
}

int recs_mood_count(void) { return NMOOD; }
int recs_mood_row(const RecRowCtx *cx, int i, RecRowDef *out) {
  if (i < 0 || i >= NMOOD) return 0;
  Bld b; bld_init(&b, cx, out, 0); return mood_build(&b, i, out);
}

static const struct { const char *cc; unsigned char mask; } PROV_REGION[] = {         /* 1 Netflix, 2 Prime Video, 4 Disney+, 8 Apple TV+ */
  { "IN", 1 | 2 | 8 }, { "US", 15 }, { "GB", 15 }, { "IE", 15 }, { "CA", 15 }, { "AU", 15 }, { "DE", 15 }, { "FR", 15 }, { "ES", 15 }, { "IT", 15 },
  { "BR", 15 }, { "MX", 15 }, { "JP", 15 }, { "KR", 15 }, { "PK", 1 | 8 }, { "BD", 1 | 8 }, { "AE", 15 }, { "NL", 15 }, { "TR", 15 }, { "PL", 15 } };
static const struct { const char *name; int id; } PROVIDERS[4] = { { "Netflix", 8 }, { "Prime Video", 119 }, { "Disney+", 337 }, { "Apple TV+", 350 } };


int recs_rows_build(const RecRowCtx *cx, RecRowDef *out, int max) {
  Bld B; bld_init(&B, cx, out, max); Bld *b = &B; const RecTaste *t = cx->taste; RecRowDef *r;
  char ago120[12], ago200[12], ago365[12]; ymd(cx->today - 120, ago120); ymd(cx->today - 200, ago200); ymd(cx->today - 365, ago365);
  const char *region = cx->region && cx->region[0] ? cx->region : "US";

  /* top picks (local), then because-you-watched (one request each) */
  if ((r = rd_new(b, RF_PERSONAL, 0, RECS_F_KEEP, NULL, 2.0, RECS_AGE_LIST))) { snprintf(r->title, sizeof r->title, "%s", trs(b, "Top picks for you")); snprintf(r->key, sizeof r->key, "personal"); }
  { int pick[RECS_MAX_SEEDS], np = 0, want = cx->lowMem ? 3 : 5;
    for (int i = 0; i < cx->nseeds && i < 64; i++) {
      const RecSeedX *s = &cx->seeds[i]; if (s->w <= 0 || s->nrecs + s->nsim < 8) continue;
      int at = np < RECS_MAX_SEEDS ? np : RECS_MAX_SEEDS - 1; if (np < RECS_MAX_SEEDS) np++;
      while (at > 0 && cx->seeds[pick[at - 1]].ageDays > s->ageDays) { pick[at] = pick[at - 1]; at--; }
      pick[at] = i;
    }
    for (int q = 0; q < np && q < want; q++) {
      const RecSeedX *s = &cx->seeds[pick[q]]; char gs[48] = "", lg[40] = ""; size_t o = 0; int anim = 0;
      for (int g = 0; g < s->ng; g++) { if (s->genres[g] == 16 || s->genres[g] == 10751 || s->genres[g] == 10762) anim = 1; if (g < 3) o += (size_t)snprintf(gs + o, sizeof gs - o, "%s%d", g ? "|" : "", s->genres[g]); }
      if (s->lang[0]) snprintf(lg, sizeof lg, "with_original_language=%s&", s->lang);
      if (!(r = rd_new(b, RF_BYW, s->kind, RECS_F_KEEP, s->lang, 1.0 + s->w, RECS_AGE_LIST))) break;
      r->seed = pick[q];
      snprintf(r->path, sizeof r->path, "/discover/%s?%s%s%s%s&sort_by=popularity.desc&vote_count.gte=%d", kname(s->kind), lg, gs[0] ? "with_genres=" : "", gs,
               anim ? "" : "&without_genres=16,10751,10762", !strcmp(s->lang, "en") ? 200 : 25);
      snprintf(r->key, sizeof r->key, "byw|%c%d|%s", s->kind, s->tmdb, s->lang);
      fmt_title(b, r->title, sizeof r->title, "Because you watched %s", s->title);
    }
  }
  /* charts and trending */
  if ((r = rd_new(b, RF_CHART, 'm', RECS_F_RANK | RECS_F_KEEP, NULL, 0.8, RECS_AGE_CHART))) { snprintf(r->title, sizeof r->title, "%s", trs(b, "Top 10 films this week")); snprintf(r->path, sizeof r->path, "/trending/movie/week"); snprintf(r->key, sizeof r->key, "chart|m|%s", region); }
  if ((r = rd_new(b, RF_CHART, 't', RECS_F_RANK | RECS_F_KEEP, NULL, 0.8, RECS_AGE_CHART))) { snprintf(r->title, sizeof r->title, "%s", trs(b, "Top 10 series this week")); snprintf(r->path, sizeof r->path, "/trending/tv/week"); snprintf(r->key, sizeof r->key, "chart|t|%s", region); }
  if ((r = rd_new(b, RF_TREND, 0, 0, NULL, 0.5, RECS_AGE_CHART))) { snprintf(r->title, sizeof r->title, "%s", trs(b, "Everyone is watching")); snprintf(r->path, sizeof r->path, "/trending/all/week"); snprintf(r->key, sizeof r->key, "trend|%s", region); }

  /* language rows */
  int gorder[NLG]; double gwt[NLG]; int ng = 0;                       /* the taste's genres as LG indices, best first */
  for (int k = 0; k < t->ng; k++) {
    int mg = movie_genre(t->gid[k]), li = -1; for (int i = 0; i < NLG; i++) if (LG[i].movie == mg) { li = i; break; }
    if (li < 0) continue;
    if (!b->likesAnim && (mg == 16 || mg == 10751)) continue;
    int at = -1; for (int i = 0; i < ng; i++) if (gorder[i] == li) { at = i; break; }
    if (at < 0) { gorder[ng] = li; gwt[ng] = 0; at = ng++; } gwt[at] += t->gw[k];
  }
  { static const int defg[5] = { 18, 35, 28, 53, 10749 };
    for (int d = 0; d < 5 && ng < 5; d++) { int li = -1; for (int i = 0; i < NLG; i++) if (LG[i].movie == defg[d]) li = i; if (find_i(gorder, ng, li) < 0) { gorder[ng] = li; gwt[ng++] = 0; } } }
  for (int i = 1; i < ng; i++) { int go = gorder[i], j = i; double gw = gwt[i]; while (j > 0 && gwt[j - 1] < gw) { gorder[j] = gorder[j - 1]; gwt[j] = gwt[j - 1]; j--; } gorder[j] = go; gwt[j] = gw; }
  double gtop = ng && gwt[0] > 0 ? gwt[0] : 1;
  for (int li = 0; li < cx->nlangs; li++) {
    const RecLang *L = &cx->langs[li]; const char *ln = recs_lang_name(L->code); if (!ln) continue;
    char lname[48]; snprintf(lname, sizeof lname, "%s", trs(b, ln));
    int en = !strcmp(L->code, "en"); double sh = recs_taste_lang(t, L->code);
    if (sh < L->pick) sh = L->pick;                                   /* a chosen language ranks as if it were most of the history */
    int second = L->pick > 0 && L->pick < 1;                          /* the second language: a little less of everything */
    int ngen = L->strong ? (second ? (cx->lowMem ? 2 : 3) : (cx->lowMem ? 3 : 4)) : (cx->lowMem ? 1 : 2);
    for (int kk = 0; kk < 2; kk++) {
      char kind = kk ? 't' : 'm'; int usedTv[NLG], nu = 0, done = 0;
      for (int q = 0; q < ng && done < ngen; q++) {
        const LgGenre *G = &LG[gorder[q]]; const char *pat = kk ? G->tpat : G->mpat; int gid = kk ? G->tv : G->movie;
        if (!pat || !gid || find_i(usedTv, nu, gid) >= 0) continue;
        usedTv[nu++] = gid;
        if (!(r = rd_new(b, RF_LANG_GENRE, kind, 0, L->code, sh * (gwt[q] / gtop) * kshare(t, kind) + 0.2, RECS_AGE_LIST))) goto langs_done;
        done++; fmt_title(b, r->title, sizeof r->title, pat, lname);
        snprintf(r->path, sizeof r->path, "/discover/%s?with_original_language=%s&with_genres=%d%s&sort_by=popularity.desc&vote_count.gte=%d", kname(kind), L->code, gid, excl(b), en ? 200 : 15);
        snprintf(r->key, sizeof r->key, "lg|%s|%c|%d|%d", L->code, kind, gid, b->likesAnim);
      }
    }
    if ((r = rd_new(b, RF_ACCLAIM, 'm', 0, L->code, sh * 0.7 + 0.1, RECS_AGE_LIST))) {
      fmt_title(b, r->title, sizeof r->title, "The best-rated %s films", lname);
      snprintf(r->path, sizeof r->path, "/discover/movie?with_original_language=%s%s&sort_by=vote_average.desc&vote_count.gte=%d", L->code, excl(b), en ? 3000 : 150);
      snprintf(r->key, sizeof r->key, "ac|%s|%d", L->code, b->likesAnim); }
    if (L->strong && (r = rd_new(b, RF_GEMS, 'm', 0, L->code, sh * 0.5 + 0.1, RECS_AGE_LIST))) {
      fmt_title(b, r->title, sizeof r->title, "%s films you may have missed", lname);
      snprintf(r->path, sizeof r->path, "/discover/movie?with_original_language=%s%s&sort_by=vote_average.desc&vote_count.gte=%d&vote_count.lte=%d", L->code, excl(b), en ? 300 : 30, en ? 1500 : 150);
      snprintf(r->key, sizeof r->key, "gem|%s|%d", L->code, b->likesAnim); }
    if ((r = rd_new(b, RF_NEW, 'm', 0, L->code, sh * 0.6 + 0.25, RECS_AGE_LIST))) {
      fmt_title(b, r->title, sizeof r->title, "New %s films", lname);
      snprintf(r->path, sizeof r->path, "/discover/movie?with_original_language=%s%s&sort_by=popularity.desc&primary_release_date.gte=%s&primary_release_date.lte=%s", L->code, excl(b), ago120, b->d0);
      snprintf(r->key, sizeof r->key, "nw|%s|m|%d", L->code, b->likesAnim); }
    if ((r = rd_new(b, RF_NEW, 't', 0, L->code, sh * 0.6 + 0.2, RECS_AGE_LIST))) {
      fmt_title(b, r->title, sizeof r->title, "New %s series", lname);
      snprintf(r->path, sizeof r->path, "/discover/tv?with_original_language=%s%s&sort_by=popularity.desc&first_air_date.gte=%s&first_air_date.lte=%s", L->code, excl(b), ago200, b->d0);
      snprintf(r->key, sizeof r->key, "nw|%s|t|%d", L->code, b->likesAnim); }
    if (L->strong && !second && !cx->lowMem) for (int dec = 2000; dec >= 1990; dec -= 10) {
      if (!(r = rd_new(b, RF_DECADE, 'm', 0, L->code, sh * 0.5 + 0.1 - (dec == 1990 ? 0.05 : 0), RECS_AGE_LIST))) break;
      fmt_title(b, r->title, sizeof r->title, dec == 2000 ? "%s films from the 2000s" : "%s films from the 1990s", lname);
      snprintf(r->path, sizeof r->path, "/discover/movie?with_original_language=%s%s&sort_by=popularity.desc&vote_count.gte=%d&primary_release_date.gte=%d-01-01&primary_release_date.lte=%d-12-31", L->code, excl(b), en ? 300 : 20, dec, dec + 9);
      snprintf(r->key, sizeof r->key, "dc|%s|%d|%d", L->code, dec, b->likesAnim);
    }
  }
langs_done:;

  /* people: lead cast and directors */
  for (int pass = 0; pass < 2; pass++) {
    int want = pass ? (cx->lowMem ? 1 : 2) : (cx->lowMem ? 2 : 4); char used[RECS_T_PEOPLE] = { 0 };
    for (int q = 0; q < want; q++) {
      int best = -1; for (int k = 0; k < t->np; k++) if (!used[k] && t->pdir[k] == pass && t->pname[k][0] && (best < 0 || t->pw[k] > t->pw[best])) best = k;
      if (best < 0) break;
      used[best] = 1;
      double sh = t->wsum > 0 ? t->pw[best] / t->wsum : 0;
      if (!(r = rd_new(b, RF_PERSON, 'm', 0, NULL, (pass ? 0.2 : 0.25) + sh, RECS_AGE_LIST))) break;
      fmt_title(b, r->title, sizeof r->title, pass ? "Directed by %s" : "More with %s", t->pname[best]);
      snprintf(r->path, sizeof r->path, "/discover/movie?%s=%d&sort_by=popularity.desc&vote_count.gte=20", pass ? "with_crew" : "with_cast", t->pid[best]);
      snprintf(r->key, sizeof r->key, "pe|%d|%d", pass, t->pid[best]);
    }
  }

  /* mood rows: the best-fitting of the table (ranked on a small stack array, then built) */
  { struct { double a; int i; } mc[NMOOD]; int built = 0, want = cx->lowMem ? 12 : 26;
    for (int i = 0; i < NMOOD; i++) { double a; if (!mood_aff(b, i, &a)) continue;
      int j = built++; while (j > 0 && mc[j - 1].a < a) { mc[j] = mc[j - 1]; j--; } mc[j].a = a; mc[j].i = i; }
    for (int q = 0; q < built && q < want && b->n < b->max; q++) { RecRowDef *m = &b->out[b->n]; if (mood_build(b, mc[q].i, m)) b->n++; } }

  /* season */
  { int md = b->month * 100 + b->day, hasHi = 0; for (int i = 0; i < cx->nlangs; i++) if (!strcmp(cx->langs[i].code, "hi")) hasHi = 1;
    const char *title = NULL, *gate = NULL; int kw = 0;
    if (md >= 1115 && md <= 1225) { title = "It is nearly Christmas"; kw = 207317; gate = "35|10749|18|14"; }          /* christmas */
    else if (md >= 1226 || md <= 102) { title = "New Year's Eve plans"; kw = 613; gate = "35|10749|18"; }               /* new year's eve */
    else if (md >= 201 && md <= 214) { title = "A Valentine's Day watchlist"; kw = 160404; gate = "10749|35|18"; }       /* valentine's day */
    else if (md >= 1010 && md <= 1031) { title = "Halloween nights"; kw = 3335; gate = "27|53|9648"; }                  /* halloween */
    else if (hasHi && md >= 1101 && md <= 1114) { title = "Diwali with the family"; kw = 289363; gate = "35|18|10749"; } /* diwali */
    if (title && (r = rd_new(b, RF_SEASON, 'm', 0, NULL, 0.75, RECS_AGE_LIST))) {
      char lgp[40] = ""; if (cx->nlangs == 1) snprintf(lgp, sizeof lgp, "with_original_language=%s&", cx->langs[0].code);
      snprintf(r->title, sizeof r->title, "%s", trs(b, title));
      snprintf(r->path, sizeof r->path, "/discover/movie?%swith_genres=%s%s&with_keywords=%d&sort_by=popularity.desc&vote_count.gte=50", lgp, gate, excl(b), kw);
      snprintf(r->key, sizeof r->key, "se|%d|%s|%d", kw, b->langCsv, b->likesAnim);
    }
  }

  /* providers: only services the region has */
  { unsigned mask = 0; for (size_t i = 0; i < sizeof PROV_REGION / sizeof PROV_REGION[0]; i++) if (!strcmp(PROV_REGION[i].cc, region)) mask = PROV_REGION[i].mask;
    for (int p = 0; p < 4; p++) if (mask & (1u << p)) {
      if (!(r = rd_new(b, RF_PROVIDER, 'm', 0, NULL, 0.3, RECS_AGE_LIST))) break;
      fmt_title(b, r->title, sizeof r->title, "New on %s", PROVIDERS[p].name);
      snprintf(r->path, sizeof r->path, "/discover/movie?watch_region=%s&with_watch_providers=%d&with_watch_monetization_types=flatrate&sort_by=popularity.desc&vote_count.gte=50&primary_release_date.gte=%s&primary_release_date.lte=%s",
               region, PROVIDERS[p].id, ago365, b->d0);
      snprintf(r->key, sizeof r->key, "pr|%s|%d", region, PROVIDERS[p].id);
    }
  }
  return b->n;
}

/* ---- E. Because you watched ---- */
typedef struct { const RecCand *c; double b, r; } BywItem;
static int byw_cmp(const void *a, const void *b) { double x = ((const BywItem *)a)->r, y = ((const BywItem *)b)->r; return x < y ? 1 : x > y ? -1 : 0; }

int recs_byw_rank(const RecSeedX *s, const RecPrefs *p, const RecCand *own, int nown, RecCand *out, int max) {
  BywItem it[3 * RECS_LIST]; int n = 0, self = recs_key(s->tmdb, s->kind);
  for (int f = 0; f < 3; f++) {
    const RecCand *l = f == 0 ? s->recs : f == 1 ? s->sim : own; int ln = f == 0 ? s->nrecs : f == 1 ? s->nsim : nown; double wt = f == 0 ? 1.0 : f == 1 ? 0.5 : 0.7;
    if (!l) continue;
    for (int r = 0; r < ln && r < RECS_LIST; r++) {
      const RecCand *c = &l[r]; int key = recs_key(c->tmdb, c->kind);
      if (key == self || excluded(p, key) || (c->adult && !p->allowAdult) || unreleased(p, c)) continue;
      int at = 0; while (at < n && recs_key(it[at].c->tmdb, it[at].c->kind) != key) at++;
      if (at == n) { it[n].c = c; it[n].b = 0; n++; }
      it[at].b += wt * (1.0 - (double)r / RECS_LIST);
    }
  }
  for (int i = 0; i < n; i++) {
    const RecCand *c = it[i].c; int same = s->lang[0] && !strcmp(c->lang, s->lang), ov = 0;
    for (int g = 0; g < c->ng; g++) for (int k = 0; k < s->ng; k++) if (c->genres[g] == s->genres[k]) { ov++; break; }
    double q = c->vote / 10.0 * (c->votes >= 200 ? 1.0 : c->votes / 200.0);
    it[i].r = it[i].b + 0.9 * same + 0.5 * ov / (s->ng > 0 ? s->ng : 1) + 0.1 * q;
  }
  qsort(it, (size_t)n, sizeof it[0], byw_cmp);
  int m = 0; for (; m < n && m < max; m++) { out[m] = *it[m].c; out[m].score = it[m].r; out[m].seedPart = it[m].b; out[m].bestSeed = -1; out[m].nSeeds = 1; }
  return m;
}

/* ---- F. Top picks ---- */
int recs_top_picks(const RecSeedX *s, int ns, const RecTaste *t, const RecPrefs *p, int window, RecCand *out, int max) {
  RecSeed rs[RECS_MAX_SEEDS]; int n = 0;
  for (int i = 0; i < ns && n < RECS_MAX_SEEDS; i++) {
    RecSeed *r = &rs[n++]; memset(r, 0, sizeof *r); r->tmdb = s[i].tmdb; r->kind = s[i].kind; snprintf(r->title, sizeof r->title, "%s", s[i].title);
    r->ng = s[i].ng; memcpy(r->genres, s[i].genres, sizeof r->genres); snprintf(r->lang, sizeof r->lang, "%s", s[i].lang); r->w = s[i].w;
    r->list = s[i].recs; r->n = s[i].recs ? s[i].nrecs : 0; if (s[i].own && s[i].nown > 0) { r->disc = s[i].own; r->nd = s[i].nown; }   /* the own-language list is the discover list (weight 0.6) */
    else { r->disc = s[i].sim; r->nd = s[i].sim ? s[i].nsim : 0; }               /* else similar plays its part */
  }
  int pm = max < 100 ? 100 : max; if (pm > RECS_MAX_CAND) pm = RECS_MAX_CAND;
  RecCand *pool = malloc(sizeof(RecCand) * (size_t)pm); if (!pool) return 0;
  int m = recs_rank(rs, n, p, pool, pm);
  int w = window < m ? window : m; if (w > max) w = max; if (w < 0) w = 0;
  /* every language with under 5% of the history shares one "other" bucket (10% of the places) */
  unsigned char used[RECS_MAX_CAND] = { 0 }; char lc[24][4]; double lshare[24], lfloor[24] = { 0 }; int lcn[24] = { 0 }, nl = 0, o = 0;
  signed char bk[RECS_MAX_CAND];
  for (int i = 0; i < m; i++) {
    double sh = recs_taste_lang(t, pool[i].lang), fl = 0;
    for (int l = 0; l < p->nlangs; l++) if (p->langPick[l] > 0 && !strcmp(p->langs[l], pool[i].lang)) { fl = p->langPick[l] >= 1 ? 0.35 : 0.2; if (sh < fl) sh = fl; break; }   /* a chosen language: a floor */
    const char *code = sh < 0.05 ? "" : pool[i].lang; int k = 0;
    while (k < nl && strcmp(lc[k], code)) k++;
    if (k == nl) { if (nl < 24) { snprintf(lc[nl], 4, "%s", code); lshare[nl] = sh < 0.05 ? 0.1 : sh; lfloor[nl] = fl; nl++; } else k = nl - 1; }
    bk[i] = (signed char)k;
  }
  for (int pos = 0; pos < w; pos++) {
    int pick = -1;
    for (int b = 0; b < nl && pick < 0; b++) if (lfloor[b] > 0 && lcn[b] < (int)(lfloor[b] * (pos + 1)))   /* a chosen language behind its floor: its best title now */
      for (int i = 0; i < m && pick < 0; i++) if (!used[i] && bk[i] == b) pick = i;
    for (int i = 0; i < m && pick < 0; i++) if (!used[i] && lcn[(int)bk[i]] < (int)(lshare[(int)bk[i]] * (pos + 1)) + 1) pick = i;   /* quota grows with the position, so every prefix is balanced */
    if (pick < 0) for (int i = 0; i < m; i++) if (!used[i]) { pick = i; break; }
    if (pick < 0) break;
    used[pick] = 1; out[o++] = pool[pick]; lcn[(int)bk[pick]]++;
  }
  for (int i = 0; i < m && o < max; i++) if (!used[i]) out[o++] = pool[i];
  free(pool); return o;
}

/* ---- G. page builder ---- */
#define PG_HS 16384
#define PG_RANK_CARDS 10
typedef struct { int key; unsigned char cnt, six, ex; } PgSlot;
typedef struct {
  PgSlot h[PG_HS];
  int nrank[RECS_PAGE_IN]; unsigned char rank[RECS_PAGE_IN][RECS_ROW_CARDS];
  int key[RECS_PAGE_IN][RECS_ROW_CARDS];
  unsigned char used[RECS_PAGE_IN];
} PgScratch;

static PgSlot *pg_get(PgSlot *h, int key, int create) {
  unsigned i = ((unsigned)key * 2654435761u) >> 18;
  for (int k = 0; k < PG_HS; k++, i = (i + 1) & (PG_HS - 1)) {
    if (h[i].key == key) return &h[i];
    if (!h[i].key) { if (!create) return NULL; h[i].key = key; return &h[i]; }
  }
  return NULL;
}

/* the cards a row may show, best first (taste score, or the given order for KEEP / RANK rows) */
static void pg_prepare(PgScratch *S, const RecPageCtx *cx, const RecRowIn *r, int i) {
  int rankRow = r->def->flags & RECS_F_RANK, keep = r->def->flags & (RECS_F_KEEP | RECS_F_RANK);
  double sc[RECS_ROW_CARDS]; int n = r->n < RECS_ROW_CARDS ? r->n : RECS_ROW_CARDS, m = 0;
  for (int j = 0; j < n; j++) {
    const RecCand *c = &r->cards[j]; int key = recs_key(c->tmdb, c->kind); S->key[i][j] = key;
    PgSlot *e = pg_get(S->h, key, 0); if (e && e->ex) continue;
    if (c->adult && !cx->allowAdult) continue;
    long d = days_fast(c->date);
    if (!rankRow) { if (!d || (cx->today && d > cx->today)) continue; if (c->kind == 'm' && cx->minMovieAge > 0 && cx->today && cx->today - d < cx->minMovieAge) continue; }
    double s = keep ? 0 : score_d(cx->taste, c, cx->today, d);
    if (!keep) for (int l = 0; l < cx->nlangs; l++) if (cx->langs[l].pick > 0 && !strcmp(cx->langs[l].code, c->lang)) { s += 0.15 * cx->langs[l].pick; break; }
    int at = m++; while (at > 0 && sc[at - 1] < s) { sc[at] = sc[at - 1]; S->rank[i][at] = S->rank[i][at - 1]; at--; }   /* stable insertion: ties keep input order */
    sc[at] = s; S->rank[i][at] = (unsigned char)j;
  }
  if (rankRow && m > PG_RANK_CARDS) m = PG_RANK_CARDS;
  S->nrank[i] = m;
}

static int pg_take(PgScratch *S, const RecRowIn *in, int i, RecPageRow *o) {
  int rankRow = in[i].def->flags & RECS_F_RANK, cap = rankRow ? PG_RANK_CARDS : RECS_PAGE_CARDS;
  unsigned char cl[RECS_ROW_CARDS], lt[RECS_ROW_CARDS]; int ncl = 0, nlt = 0;
  for (int q = 0; q < S->nrank[i] && ncl < cap; q++) {
    int j = S->rank[i][q]; PgSlot *e = pg_get(S->h, S->key[i][j], 0);
    if (e && e->cnt >= 2) continue;
    if (!rankRow && e && e->six && ncl < 6) lt[nlt++] = (unsigned char)j; else cl[ncl++] = (unsigned char)j;
  }
  if (!rankRow && ncl < 6) return 0;
  int tot = ncl; for (int q = 0; q < nlt && tot < cap; q++) cl[tot++] = lt[q];
  if (tot < 8) return 0;
  o->in = (short)i; o->n = (short)tot; memcpy(o->idx, cl, (size_t)tot);
  return 1;
}
static void pg_commit(PgScratch *S, const RecPageRow *o) {
  for (int q = 0; q < o->n; q++) { PgSlot *e = pg_get(S->h, S->key[o->in][o->idx[q]], 1); if (!e) continue; if (e->cnt < 250) e->cnt++; if (q < 6) e->six = 1; }
}
static int pg_place(PgScratch *S, const RecRowIn *in, int i, RecPage *pg) {
  S->used[i] = 1; if (pg->n >= RECS_PAGE_MAX) return 0;
  RecPageRow r; if (!pg_take(S, in, i, &r)) return 0;
  pg_commit(S, &r); pg->rows[pg->n++] = r; return 1;
}

int recs_page_build(const RecPageCtx *cx, const RecRowIn *in, int nin, RecPage *pg) {
  pg->n = 0; if (nin > RECS_PAGE_IN) nin = RECS_PAGE_IN;
  int maxRows = cx->maxRows > RECS_PAGE_MAX ? RECS_PAGE_MAX : cx->maxRows; if (maxRows <= 0 || nin <= 0) return 0;
  PgScratch *S = calloc(1, sizeof *S); if (!S) return 0;
  for (int i = 0; i < cx->nexclude && i < 4000; i++) { PgSlot *e = pg_get(S->h, cx->exclude[i], 1); if (e) e->ex = 1; }
  for (int i = 0; i < cx->nbill && i < 64; i++) { PgSlot *e = pg_get(S->h, cx->bill[i], 1); if (e) { e->cnt = 1; e->six = 1; } }
  for (int i = 0; i < nin; i++) pg_prepare(S, cx, &in[i], i);
  int personal = -1, pinned[RECS_PAGE_IN], np = 0, pc = 0, byw[RECS_PAGE_IN], nb = 0, bc = 0, pool[RECS_PAGE_IN], npool = 0;
  for (int i = 0; i < nin; i++) {
    int f = in[i].def->family, ok = S->nrank[i] >= 8;
    if (f == RF_PERSONAL) { if (personal < 0) personal = i; }
    else if (!ok) continue;
    else if (f == RF_CHART) pinned[np++] = i;
    else if (f == RF_BYW) byw[nb++] = i;
    else if (f != RF_TREND) pool[npool++] = i;
  }
  for (int i = 0; i < nin; i++) if (in[i].def->family == RF_TREND && S->nrank[i] >= 8) pinned[np++] = i;    /* trending comes after the charts */
  if (personal >= 0) pg_place(S, in, personal, pg);
  while (pg->n < maxRows) {
    int at = pg->n, placed = 0;
    int slot = (at == 2 || at == 5 || at == 11) && pc < np;
    if (slot) { while (pc < np && !placed) placed = pg_place(S, in, pinned[pc++], pg); if (placed) continue; }
    if ((at == 1 || at % 4 == 3) && bc < nb) { while (bc < nb && !placed) placed = pg_place(S, in, byw[bc++], pg); if (placed) continue; }
    if (npool) {   /* a chosen language with no row lately takes this slot (main within 3 rows, second within 5) */
      int forced = -1;
      for (int l = 0; l < cx->nlangs && forced < 0; l++) {
        const RecLang *L = &cx->langs[l]; if (L->pick <= 0) continue;
        int win = L->pick >= 1 ? 3 : 5, seen = 0;
        for (int k = 0; k < win && k < pg->n && !seen; k++) seen = !strcmp(in[pg->rows[pg->n - 1 - k].in].def->lang, L->code);
        if (seen || pg->n < 1) continue;
        double bs = -1e9;
        for (int q = 0; q < npool; q++) { int i = pool[q]; if (S->used[i] || strcmp(in[i].def->lang, L->code)) continue;
          if (in[i].def->affinity > bs) { bs = in[i].def->affinity; forced = i; } }
      }
      if (forced >= 0 && pg_place(S, in, forced, pg)) continue;
      int fam[4] = { -1, -1, -1, -1 }; for (int k = 0; k < 4 && k < pg->n; k++) fam[k] = in[pg->rows[pg->n - 1 - k].in].def->family;
      int best = -1; double bs = -1e9;
      for (int q = 0; q < npool; q++) {
        int i = pool[q]; if (S->used[i]) continue;
        int dup = 0, six = S->nrank[i] < 6 ? S->nrank[i] : 6, same = 0;
        for (int k = 0; k < six; k++) { PgSlot *e = pg_get(S->h, S->key[i][S->rank[i][k]], 0); if (e && e->six) dup++; }
        for (int k = 0; k < 4; k++) if (fam[k] == in[i].def->family) same++;
        double sc = in[i].def->affinity - 0.25 * same - 0.8 * dup / 6.0 + (in[i].def->kind == 't' ? 0.05 : 0);
        if (sc > bs) { bs = sc; best = i; }
      }
      if (best >= 0) { pg_place(S, in, best, pg); continue; }
    }
    /* the greedy pool is empty: use up what the fixed slots left, else stop */
    if (bc < nb) { pg_place(S, in, byw[bc++], pg); continue; }
    if (pc < np) { pg_place(S, in, pinned[pc++], pg); continue; }
    break;
  }
  free(S); return pg->n;
}
