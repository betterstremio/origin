/* "For You" (see recs.h): the watch history -> seeds -> TMDB lists -> recs_core ranking -> Home's rows.
   One worker thread does every file read, JSON parse, ranking and page build; the main thread only decides when,
   sends the requests the worker asks for and swaps in what it finished (see "threads" below). */
#include "core/recs.h"
#include "core/recs_core.h"
#include "core/app.h"
#include "core/i18n.h"
#include "core/net.h"
#include "core/store.h"
#include "core/tmdb.h"
#include "core/stremio.h"
#include "core/nuvio.h"
#include "platform/plat.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IMG_P "https://image.tmdb.org/t/p/w342"
#define IMG_B "https://image.tmdb.org/t/p/w1280"
#define RESULT_AGE (12 * 3600)
#define INFLIGHT 3         /* requests asked of TMDB at once (the id lookups after the page is up: at most 2 of them, and only while idle) */
#define BILL 8             /* billboard titles */
#define SHOWN_MIN 8        /* a row with fewer known cards than this never reaches the page */
#define SLICE_US 100       /* what one main-thread tick may spend on gathering the library's seeds or moving the old id map */

/* ---- small JSON helpers ---- */
static const char *rjs(const cJSON *o, const char *k) { const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k); return cJSON_IsString(v) && v->valuestring ? v->valuestring : ""; }
static double rjn(const cJSON *o, const char *k) { const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k); return cJSON_IsNumber(v) ? v->valuedouble : 0; }
static long today_days(void) { return (long)(time(NULL) / 86400); }
static long long us_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
static long long cpu_ms(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

/* ---- settings ---- */
static const struct { const char *cc, *name, *langs; } REGIONS[] = {
  { "IN", "India", "hi,en" }, { "US", "the US", "en" }, { "GB", "the UK", "en" }, { "IE", "Ireland", "en" }, { "CA", "Canada", "en,fr" },
  { "AU", "Australia", "en" }, { "DE", "Germany", "de,en" }, { "FR", "France", "fr,en" }, { "ES", "Spain", "es,en" }, { "IT", "Italy", "it,en" },
  { "BR", "Brazil", "pt,en" }, { "MX", "Mexico", "es,en" }, { "JP", "Japan", "ja,en" }, { "KR", "South Korea", "ko,en" }, { "PK", "Pakistan", "ur,hi,en" },
  { "BD", "Bangladesh", "bn,hi,en" }, { "AE", "the UAE", "ar,hi,en" }, { "NL", "the Netherlands", "nl,en" }, { "TR", "Turkey", "tr,en" }, { "PL", "Poland", "pl,en" } };
#define NREG ((int)(sizeof REGIONS / sizeof REGIONS[0]))
/* the audio setting's 3-letter codes (LANG_OPTS in settings.c) as TMDB's 2-letter ones */
static const char *const AUDIO[][2] = { { "eng", "en" }, { "ara", "ar" }, { "fre", "fr" }, { "spa", "es" }, { "ger", "de" }, { "ita", "it" }, { "por", "pt" }, { "rus", "ru" },
  { "tur", "tr" }, { "pol", "pl" }, { "dut", "nl" }, { "swe", "sv" }, { "heb", "he" }, { "hin", "hi" }, { "jpn", "ja" }, { "kor", "ko" }, { "chi", "zh" } };
const char *recs_region_name(const char *cc) { for (int i = 0; i < NREG; i++) if (!strcmp(REGIONS[i].cc, cc)) return REGIONS[i].name; return cc; }
int  recs_on(void) { return strcmp(store_get("recsOn", "true"), "false") != 0; }
void recs_set_on(int on) { store_set("recsOn", on ? "true" : "false"); recs_refresh(); }
int  recs_paused(void) { return !strcmp(store_get("recsPaused", "false"), "true"); }
void recs_set_paused(int on) { store_set("recsPaused", on ? "true" : "false"); }
int  recs_hide_cinema(void) { return !strcmp(store_get("recsHideCinema", "false"), "true"); }
void recs_set_hide_cinema(int on) { store_set("recsHideCinema", on ? "true" : "false"); recs_refresh(); }
const char *recs_region(void) {
  const char *r = store_get("recsRegion", ""); if (r[0]) return r;
  static char cc[4]; if (cc[0]) return cc;
#if SB_WINDOWED
  { const char *c = plat_country(); if (c) { snprintf(cc, sizeof cc, "%s", c); return cc; } } /* the system's country first */
#endif
  const char *loc = plat_ui_locale(); const char *d = loc ? strpbrk(loc, "-_") : NULL; /* "en-IN" */
  if (d && strlen(d + 1) >= 2) { cc[0] = (char)(d[1] & ~0x20); cc[1] = (char)(d[2] & ~0x20); cc[2] = 0; } else snprintf(cc, sizeof cc, "US");
  return cc;
}
void recs_set_region(const char *cc) { store_set("recsRegion", cc ? cc : ""); store_set("recsLangs", ""); recs_refresh(); }
static const char *region_langs(const char *cc) { for (int i = 0; i < NREG; i++) if (!strcmp(REGIONS[i].cc, cc)) return REGIONS[i].langs; return "en"; }
const char *recs_langs(void) { const char *l = store_get("recsLangs", ""); return l[0] ? l : region_langs(recs_region()); }
void recs_set_langs(const char *csv) { store_set("recsLangs", csv ? csv : ""); recs_refresh(); }
/* the default audio language as 2 letters; "" when the setting is off, unset or not in the table */
static void audio_code(char out[4]) {
  out[0] = 0; if (!strcmp(store_get("enableDefaultAudioTrack", "true"), "false")) return;
  const char *a = store_get("defaultAudioTrack", ""); if (strlen(a) == 2) { snprintf(out, 4, "%s", a); return; }
  for (size_t i = 0; i < sizeof AUDIO / sizeof AUDIO[0]; i++) if (!strcmp(AUDIO[i][0], a)) { snprintf(out, 4, "%s", AUDIO[i][1]); return; }
}
static unsigned fnv(const char *s) { unsigned h = 2166136261u; while (*s) h = (h ^ (unsigned char)*s++) * 16777619u; return h; }
/* what a page depends on: when it changes the page is made again (and a saved page for another one is not used) */
static unsigned settings_sig(char *buf, size_t cap) {
  char au[4]; audio_code(au);
  snprintf(buf, cap, "%s|%s|%s|%s|%d|%d|%s", recs_region(), store_get("recsLangs", ""), au, store_get("enableDefaultAudioTrack", "true"), recs_hide_cinema(), recs_on(), i18n_lang());
  if (acct_is_nuvio() && nv_profile_current()) { size_t l = strlen(buf); char s[160]; nv_scope(nv_profile_current(), s, sizeof s); snprintf(buf + l, cap - l, "|%s", s); } /* each profile its own page (a Stremio account: as it always was) */
  return fnv(buf);
}

/* =====================================================================================================
   threads: the worker (W) owns every file read, JSON parse, recs_parse, ranking, page build and file write; the
   main thread (M) never parses and never walks more than SLICE_US of the library or the old id map per tick.
     M: recs_tick (decides, gathers seeds in slices, pumps requests), on_resp (copies a body, posts it), the
        getters (copy a few cards out of the page under the mutex), recs_dismiss
     W: worker_main, do_load, run (the pipeline), fetch (cache reads, waits for answers), pass-1/2 id lookups,
        page_make / page_save / the id table
   They talk through two lists under one mutex: messages M -> W (load, run, a response, save, ...) and wants
   W -> M ("please ask TMDB for this"). The page W built is swapped in with a pointer under the same mutex;
   only W ever frees a page, so the pointer a getter holds is good while it holds the mutex.
   ===================================================================================================== */
typedef struct Want { struct Want *next; unsigned gen; int tag, low; char path[400]; } Want;
typedef struct Msg { struct Msg *next; int type, status, tag; unsigned gen; unsigned char *body; size_t len; void *p; } Msg;
enum { MG_NOP, MG_LOAD, MG_RUN, MG_RESP, MG_SAVE, MG_MIG, MG_DROP };
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static Msg *inH, *inT; static Want *wH; static unsigned gen = 1; static int workerOn;
static unsigned gen_now(void) { return __atomic_load_n(&gen, __ATOMIC_ACQUIRE); }
static void msg_post(Msg *m) { m->next = NULL; pthread_mutex_lock(&mu); if (inT) inT->next = m; else inH = m; inT = m; pthread_cond_signal(&cv); pthread_mutex_unlock(&mu); }
static void msg_post_type(int type, void *p) { Msg *m = calloc(1, sizeof *m); if (!m) return; m->type = type; m->p = p; msg_post(m); }
static Msg *msg_take(void) { pthread_mutex_lock(&mu); while (!inH) pthread_cond_wait(&cv, &mu); Msg *m = inH; inH = m->next; if (!inH) inT = NULL; pthread_mutex_unlock(&mu); return m; }
static void msg_free(Msg *m) { free(m->body); free(m); }
static void want_add(unsigned g, int tag, int low, const char *path) {
  Want *w = calloc(1, sizeof *w); if (!w) return; w->gen = g; w->tag = tag; w->low = low; snprintf(w->path, sizeof w->path, "%s", path);
  pthread_mutex_lock(&mu); Want **pp = &wH; while (*pp) pp = &(*pp)->next; *pp = w; pthread_mutex_unlock(&mu);
}
/* the run in flight is void (a setting changed, or the network is gone): its wants are dropped, its answers ignored, W woken */
static void cancel_run(void) {
  pthread_mutex_lock(&mu); __atomic_store_n(&gen, gen + 1, __ATOMIC_RELEASE); while (wH) { Want *w = wH; wH = w->next; free(w); } pthread_mutex_unlock(&mu);
  msg_post_type(MG_NOP, NULL);
}

/* ---- the IMDb ids: kind + TMDB id -> the number in "tt0816692" and its digit count (the zeros are kept). A file of
   12-byte records in the data folder (not the store: every store flush would carry it), the newest ID_KEEP. It lives
   in memory only while W works on it (about 1.3 ms to read, 0.3 MB at the bound). ---- */
#define ID_KEEP 20000
#define ID_MAGIC 0x31444952u   /* "RID1" */
typedef struct { unsigned key, num; unsigned char dig, pad[3]; } IdRec;
static IdRec *idr; static int idn, idcap, *idh, idLoaded, idDirty; static unsigned idhn;
static unsigned hash32(unsigned k) { return (k * 2654435761u) >> 11; }
static unsigned ckey(char kind, int tmdb) { return ((unsigned)tmdb << 1) | (kind == 't'); }   /* (TMDB ids are far below 2^31) */
static int tt_parse(const char *s, unsigned *num, int *dig) {
  if (!s || s[0] != 't' || s[1] != 't') return 0; const char *p = s + 2; unsigned long long v = 0; int d = 0;
  while (*p >= '0' && *p <= '9') { if (++d > 9) return 0; v = v * 10 + (unsigned)(*p++ - '0'); }
  if (!d || *p || !v) return 0; *num = (unsigned)v; *dig = d; return 1;
}
static void id_hash_build(void) {
  free(idh); idhn = 1024; while (idhn < (unsigned)(idn + 512) * 2) idhn <<= 1; idh = calloc(idhn, sizeof *idh); if (!idh) { idhn = 0; return; }
  for (int j = 0; j < idn; j++) { unsigned i = hash32(idr[j].key) & (idhn - 1); while (idh[i]) i = (i + 1) & (idhn - 1); idh[i] = j + 1; }
}
static IdRec *id_find(unsigned key) {
  if (!idhn) return NULL; unsigned m = idhn - 1, i = hash32(key) & m; int j;
  while ((j = idh[i])) { if (idr[j - 1].key == key) return &idr[j - 1]; i = (i + 1) & m; } return NULL;
}
static void id_add(unsigned key, unsigned num, int dig) {
  if (!idh) id_hash_build(); if (!idh || id_find(key)) return;
  if (idn == idcap) { int nc = idcap ? idcap * 2 : 2048; IdRec *n = realloc(idr, sizeof *n * (size_t)nc); if (!n) return; idr = n; idcap = nc; }
  idr[idn].key = key; idr[idn].num = num; idr[idn].dig = (unsigned char)dig; idn++; idDirty = 1;
  if ((unsigned)(idn + 1) * 2 > idhn) id_hash_build(); else { unsigned i = hash32(key) & (idhn - 1); while (idh[i]) i = (i + 1) & (idhn - 1); idh[i] = idn; }
}
static int id_tmdb_of(unsigned num, int tv) { for (int j = idn - 1; j >= 0; j--) if (idr[j].num == num && (int)(idr[j].key & 1) == tv) return (int)(idr[j].key >> 1); return 0; } /* (a seed: a scan of a few thousand at most, once per seed) */
static char pageName[220] = "recspage.bin"; /* (under mu: W reads it) */
static void page_file(char *out, size_t cap) { pthread_mutex_lock(&mu); snprintf(out, cap, "%s", pageName); pthread_mutex_unlock(&mu); }
static void page_file_set(void) { char n[220] = "recspage.bin"; if (acct_is_nuvio() && nv_profile_current()) { char s[160]; nv_scope(nv_profile_current(), s, sizeof s); snprintf(n, sizeof n, "acct/%s.page.bin", s); } pthread_mutex_lock(&mu); snprintf(pageName, sizeof pageName, "%s", n); pthread_mutex_unlock(&mu); }
static char *dpath(const char *name, char *out, size_t cap) { snprintf(out, cap, "%s/%s", store_data_dir(), name); return out; }
/* written beside and renamed over: a crash keeps the old file */
static int file_put(const char *path, const void *a, size_t na, const void *b, size_t nb, const void *c, size_t nc) {
  char tmp[1300]; snprintf(tmp, sizeof tmp, "%s.tmp", path); FILE *f = fopen(tmp, "wb"); if (!f) return 0;
  int ok = (!na || fwrite(a, 1, na, f) == na) && (!nb || fwrite(b, 1, nb, f) == nb) && (!nc || fwrite(c, 1, nc, f) == nc); ok = fclose(f) == 0 && ok;
  if (!ok) { remove(tmp); return 0; } return rename(tmp, path) == 0;
}
static void id_load(void) {
  if (idLoaded) return; idLoaded = 1; idDirty = 0; char p[1300]; FILE *f = fopen(dpath("recsids.bin", p, sizeof p), "rb"); unsigned h[2];
  if (f && fread(h, 4, 2, f) == 2 && h[0] == ID_MAGIC && h[1] <= 4 * ID_KEEP) { idcap = (int)h[1] + 2048; idr = malloc(sizeof *idr * (size_t)idcap); if (idr) idn = (int)fread(idr, sizeof *idr, h[1], f); }
  if (f) fclose(f); id_hash_build();
}
static void id_save(void) {
  if (!idLoaded || !idDirty) return; if (idn > ID_KEEP) { memmove(idr, idr + (idn - ID_KEEP), sizeof *idr * ID_KEEP); idn = ID_KEEP; id_hash_build(); }
  char p[1300]; unsigned h[2] = { ID_MAGIC, (unsigned)idn }; if (file_put(dpath("recsids.bin", p, sizeof p), h, sizeof h, idr, sizeof *idr * (size_t)idn, NULL, 0)) idDirty = 0;
}
static void id_free(void) { free(idr); free(idh); idr = NULL; idh = NULL; idn = idcap = 0; idhn = 0; idLoaded = 0; idDirty = 0; }

/* ---- the page: what stays in memory. A card is 40 bytes plus its strings in one shared blob (a title shown twice
   shares its strings). Rows are ranges of cards. The cards of a row whose id is not known yet (imdb 0) are skipped
   by the getters, and become visible when W finds the id (no version bump: Home asks again when a row comes into
   its window). One malloc block: header, rows, cards, blob, then a small hash from the IMDb number to the card. ---- */
typedef struct { int tmdb; unsigned imdb, title, poster, backdrop, overview; char date[12]; unsigned char kind, dig, flags, rank; } PCard;   /* kind 0 movie, 1 series */
typedef struct { unsigned title; unsigned short first, n; unsigned char rank, fam, pad[2]; } PRow;
typedef struct { unsigned magic, ver, sig; int nrows, ncards, nbill; unsigned blobLen; long long at; } PHdr;
typedef struct { PHdr h; PRow *rows; PCard *cards; char *blob; unsigned short *ix; unsigned ixn; } Page;
enum { CF_LOVE = 1, CF_HIDE = 2, CF_NOID = 4 };
#define PG_MAGIC 0x31475052u   /* "RPG1" */
#define PG_CARDS (BILL + RECS_PAGE_MAX * RECS_PAGE_CARDS)
static Page *cur; static int shown = 1; static unsigned ver; static long pageAt;   /* cur, shown, pageAt: under mu */
static Page *page_alloc(int R, int N, unsigned blob) {
  unsigned ixn = 64; while (ixn < (unsigned)N * 2) ixn <<= 1;
  size_t o1 = (sizeof(Page) + 7) & ~(size_t)7, o2 = o1 + sizeof(PRow) * (size_t)R, o3 = o2 + sizeof(PCard) * (size_t)N, o4 = o3 + blob, o5 = (o4 + 1) & ~(size_t)1, total = o5 + 2 * (size_t)ixn;
  Page *p = calloc(1, total); if (!p) return NULL; char *b = (char *)p;
  p->rows = (PRow *)(b + o1); p->cards = (PCard *)(b + o2); p->blob = b + o3; p->ix = (unsigned short *)(b + o5); p->ixn = ixn; return p;
}
static int page_find(const Page *p, unsigned num, int dig) {
  unsigned m = p->ixn - 1, h = hash32(num) & m; unsigned short j;
  while ((j = p->ix[h])) { const PCard *c = &p->cards[j - 1]; if (c->imdb == num && c->dig == dig) return j - 1; h = (h + 1) & m; } return -1;
}
static void page_ix_add(Page *p, int i) {
  const PCard *s = &p->cards[i]; unsigned m = p->ixn - 1, h = hash32(s->imdb) & m; unsigned short j;
  while ((j = p->ix[h])) { const PCard *c = &p->cards[j - 1]; if (c->imdb == s->imdb && c->dig == s->dig) return; h = (h + 1) & m; } p->ix[h] = (unsigned short)(i + 1);
}
static int vis(const PCard *c) { return c->imdb && !(c->flags & (CF_HIDE | CF_NOID)); }
static size_t page_bytes(const Page *p) { return sizeof(PHdr) + sizeof(PRow) * (size_t)p->h.nrows + sizeof(PCard) * (size_t)p->h.ncards + p->h.blobLen; }
/* a copy of the live page's file image, taken under the mutex (a memcpy), written outside it */
static void page_save_cur(void) {
  pthread_mutex_lock(&mu); Page *p = cur; size_t n = p ? page_bytes(p) : 0; unsigned char *b = n ? malloc(n) : NULL;
  if (b) { unsigned char *q = b; memcpy(q, &p->h, sizeof(PHdr)); q += sizeof(PHdr); memcpy(q, p->rows, sizeof(PRow) * (size_t)p->h.nrows); q += sizeof(PRow) * (size_t)p->h.nrows;
    memcpy(q, p->cards, sizeof(PCard) * (size_t)p->h.ncards); q += sizeof(PCard) * (size_t)p->h.ncards; memcpy(q, p->blob, p->h.blobLen); }
  pthread_mutex_unlock(&mu);
  if (b) { char path[1300], name[220]; page_file(name, sizeof name); file_put(dpath(name, path, sizeof path), b, n, NULL, 0, NULL, 0); free(b); }
}
static Page *page_load(unsigned sig, long maxAge) {
  char path[1300], name[220]; page_file(name, sizeof name); FILE *f = fopen(dpath(name, path, sizeof path), "rb"); if (!f) return NULL; PHdr h; Page *p = NULL;
  if (fread(&h, sizeof h, 1, f) != 1 || h.magic != PG_MAGIC || h.ver != 1 || h.sig != sig || h.nrows < 0 || h.nrows > RECS_PAGE_MAX || h.ncards < 0 || h.ncards > PG_CARDS || h.nbill < 0 || h.nbill > h.ncards ||
      h.blobLen < 2 || h.blobLen > (8u << 20) || h.at > (long long)time(NULL) + 86400 || (long long)time(NULL) - h.at > maxAge) goto out;
  p = page_alloc(h.nrows, h.ncards, h.blobLen); if (!p) goto out; p->h = h;
  if (fread(p->rows, sizeof(PRow), (size_t)h.nrows, f) != (size_t)h.nrows || fread(p->cards, sizeof(PCard), (size_t)h.ncards, f) != (size_t)h.ncards || fread(p->blob, 1, h.blobLen, f) != h.blobLen || p->blob[h.blobLen - 1]) { free(p); p = NULL; goto out; }
  { int bad = 0; for (int i = 0; i < h.nrows; i++) bad |= p->rows[i].title >= h.blobLen || p->rows[i].first + p->rows[i].n > h.ncards;
    for (int i = 0; i < h.ncards; i++) { const PCard *c = &p->cards[i]; bad |= c->title >= h.blobLen || c->poster >= h.blobLen || c->backdrop >= h.blobLen || c->overview >= h.blobLen || c->kind > 1 || c->date[11]; }
    if (bad) { free(p); p = NULL; goto out; } }
out: fclose(f); return p;
}
static void ver_bump(void) { __atomic_add_fetch(&ver, 1, __ATOMIC_RELAXED); }
unsigned recs_version(void) { return __atomic_load_n(&ver, __ATOMIC_RELAXED); }

/* the page builder's working copy: cards still pointing at their RecCand, with the ids found so far */
typedef struct { const RecCand *c; unsigned imdb; unsigned char dig, flags, rank; } BCard;
typedef struct { const RecRowDef *def; int first, n; } BRow;
typedef struct { BCard c[PG_CARDS]; BRow r[RECS_PAGE_MAX]; int nc, nr, nbill; } PB;
typedef struct { char *b; unsigned n, cap; } Blob;
static unsigned blob_add(Blob *B, const char *s) {
  if (!s[0]) return 0; size_t l = strlen(s) + 1;
  if (B->n + l > B->cap) { unsigned nc = B->cap ? B->cap * 2 : 65536; while (nc < B->n + l) nc *= 2; char *nb = realloc(B->b, nc); if (!nb) return 0; B->b = nb; B->cap = nc; }
  unsigned o = B->n; memcpy(B->b + o, s, l); B->n += (unsigned)l; return o;
}
/* the compact page from the builder: rows that kept >= SHOWN_MIN known cards, no card without an id answer (NOID) */
static Page *page_make(const PB *b, unsigned sig) {
  int *emit = malloc(sizeof(int) * (size_t)(b->nc + 1)); int N = 0, R = 0; PRow rows[RECS_PAGE_MAX]; if (!emit) return NULL;
  for (int i = 0; i < b->nbill; i++) if (!(b->c[i].flags & CF_NOID)) emit[N++] = i;
  int nbill = N;
  for (int r = 0; r < b->nr && R < RECS_PAGE_MAX; r++) { const BRow *br = &b->r[r]; int first = N;
    for (int q = 0; q < br->n; q++) if (!(b->c[br->first + q].flags & CF_NOID)) emit[N++] = br->first + q;
    memset(&rows[R], 0, sizeof rows[R]); rows[R].first = (unsigned short)first; rows[R].n = (unsigned short)(N - first); rows[R].rank = (br->def->flags & RECS_F_RANK) != 0; rows[R].fam = br->def->family; R++; }
  Blob B = { 0 }; blob_add(&B, " "); B.n = 1; B.b[0] = 0;   /* offset 0 is "" */
  PCard *tc = calloc((size_t)N + 1, sizeof *tc); int *rep = malloc(sizeof(int) * ((size_t)N + 1)); unsigned kn = 64; while (kn < (unsigned)N * 2) kn <<= 1; int *km = calloc(kn, sizeof *km);
  Page *pg = NULL; if (!tc || !rep || !km || !B.b) goto out;
  for (int k = 0; k < N; k++) {
    const BCard *s = &b->c[emit[k]]; const RecCand *c = s->c; PCard *t = &tc[k]; unsigned key = ckey(c->kind, c->tmdb), h = hash32(key) & (kn - 1); int at = -1;
    while (km[h]) { const PCard *o = &tc[km[h] - 1]; if (o->tmdb == c->tmdb && o->kind == (c->kind == 't')) { at = km[h] - 1; break; } h = (h + 1) & (kn - 1); }
    t->tmdb = c->tmdb; t->kind = c->kind == 't'; t->imdb = s->imdb; t->dig = s->dig; snprintf(t->date, sizeof t->date, "%.10s", c->date);
    if (at >= 0) { t->title = tc[at].title; t->poster = tc[at].poster; t->backdrop = tc[at].backdrop; t->overview = tc[at].overview; rep[k] = at; }
    else { t->title = blob_add(&B, c->title); t->poster = blob_add(&B, c->poster); t->backdrop = blob_add(&B, c->backdrop); t->overview = blob_add(&B, c->overview); km[h] = k + 1; rep[k] = k; }
  }
  /* LOVE (billboard or a Because-you-watched row) and the Top 10 numeral belong to the title: all its cards carry the best of them */
  for (int k = 0; k < nbill; k++) tc[rep[k]].flags |= CF_LOVE;
  for (int r = 0; r < R; r++) for (int q = 0; q < rows[r].n; q++) { int k = rows[r].first + q, o = rep[k];
    if (rows[r].fam == RF_BYW) tc[o].flags |= CF_LOVE; if (rows[r].rank && q < 10 && (!tc[o].rank || q + 1 < tc[o].rank)) tc[o].rank = (unsigned char)(q + 1); }
  for (int k = 0; k < N; k++) { tc[k].flags = tc[rep[k]].flags; tc[k].rank = tc[rep[k]].rank; }
  for (int r = 0; r < R; r++) rows[r].title = blob_add(&B, b->r[r].def->title);
  pg = page_alloc(R, N, B.n); if (!pg) goto out;
  pg->h.magic = PG_MAGIC; pg->h.ver = 1; pg->h.sig = sig; pg->h.nrows = R; pg->h.ncards = N; pg->h.nbill = nbill; pg->h.blobLen = B.n; pg->h.at = (long long)time(NULL);
  memcpy(pg->rows, rows, sizeof(PRow) * (size_t)R); memcpy(pg->cards, tc, sizeof(PCard) * (size_t)N); memcpy(pg->blob, B.b, B.n);
  for (int k = 0; k < N; k++) if (pg->cards[k].imdb) page_ix_add(pg, k);
out: free(emit); free(tc); free(rep); free(km); free(B.b); return pg;
}

/* ---- the getters (main thread): a few cards copied out under the mutex ---- */

static void inflate(Item *it, const Page *p, const PCard *c) {
  snprintf(it->id, sizeof it->id, "tt%0*u", c->dig, c->imdb); snprintf(it->type, sizeof it->type, "%s", c->kind ? "series" : "movie");
  snprintf(it->name, sizeof it->name, "%s", p->blob + c->title); snprintf(it->poster, sizeof it->poster, IMG_P "%s", p->blob + c->poster); snprintf(it->background, sizeof it->background, IMG_B "%s", p->blob + c->backdrop);
  snprintf(it->description, sizeof it->description, "%s", p->blob + c->overview); if (strlen(c->date) >= 4) snprintf(it->metaLine, sizeof it->metaLine, "%.4s", c->date);   /* (epName stays empty on a card) */
}
static Page *pg_lock(void) { pthread_mutex_lock(&mu); if (!cur || !shown) { pthread_mutex_unlock(&mu); return NULL; } return cur; }
static void pg_unlock(void) { pthread_mutex_unlock(&mu); }
static int items_of(const Page *p, int first, int n, ItemList *l) {
  memset(l, 0, sizeof *l); int k = 0; for (int i = 0; i < n; i++) k += vis(&p->cards[first + i]); if (!k) return 0;
  l->items = calloc((size_t)k, sizeof(Item)); if (!l->items) return 0;
  for (int i = 0; i < n; i++) if (vis(&p->cards[first + i])) inflate(&l->items[l->n++], p, &p->cards[first + i]);
  l->total = l->n; return l->n;
}
int recs_billboard(ItemList *l) { memset(l, 0, sizeof *l); Page *p = pg_lock(); if (!p) return 0; int n = items_of(p, 0, p->h.nbill, l); pg_unlock(); return n; }
/* DEVELOPMENT AID: SB_RECS_FAKE=36 makes that many "Test row N" rows of the cards of the page, cycled (no network, nothing kept) */
static int fake_rows(void) { static int n = -1; if (n < 0) { const char *e = getenv("SB_RECS_FAKE"); n = e ? atoi(e) : 0; if (n > 200) n = 200; if (n < 0) n = 0; } return n; }
static int fake_total(const Page *p) { int k = 0; for (int i = 0; i < p->h.ncards; i++) k += vis(&p->cards[i]); return k; }
static const PCard *fake_card(const Page *p, int k) { for (int i = 0; i < p->h.ncards; i++) if (vis(&p->cards[i]) && k-- == 0) return &p->cards[i]; return NULL; }
int recs_rows(void) { Page *p = pg_lock(); if (!p) return 0; int n = fake_rows() ? (fake_total(p) ? fake_rows() : 0) : p->h.nrows; pg_unlock(); return n; }
int recs_row_info(int i, char *t, size_t cap, int *rank, int *count) {
  Page *p = pg_lock(); if (!p) return 0; int ok = 0;
  if (fake_rows()) { int tot = fake_total(p); if (i >= 0 && i < fake_rows() && tot) { snprintf(t, cap, "Test row %d", i + 1); *rank = i % 6 == 2; *count = tot < 20 ? tot : 20; ok = 1; } }
  else if (i >= 0 && i < p->h.nrows) { const PRow *r = &p->rows[i]; int k = 0; for (int q = 0; q < r->n; q++) k += vis(&p->cards[r->first + q]);
    snprintf(t, cap, "%s", p->blob + r->title); *rank = r->rank; *count = k; ok = 1; }
  pg_unlock(); return ok;
}
int recs_row_items(int i, ItemList *l) {
  memset(l, 0, sizeof *l); Page *p = pg_lock(); if (!p) return 0; int n = 0;
  if (fake_rows()) { int tot = fake_total(p), m = tot < 20 ? tot : 20; if (i >= 0 && i < fake_rows() && tot && (l->items = calloc((size_t)m, sizeof(Item)))) {
      for (int c = 0; c < m; c++) inflate(&l->items[c], p, fake_card(p, (i * 3 + c) % tot)); l->n = l->total = n = m; } }
  else if (i >= 0 && i < p->h.nrows) n = items_of(p, p->rows[i].first, p->rows[i].n, l);
  pg_unlock(); return n;
}
static const PCard *card_of(const Page *p, const Item *it) { unsigned num; int dig; if (!tt_parse(it->id, &num, &dig)) return NULL; int i = page_find(p, num, dig); return i >= 0 ? &p->cards[i] : NULL; }
int recs_rank_of(const Item *it) { Page *p = pg_lock(); if (!p) return 0; const PCard *c = card_of(p, it); int r = c ? c->rank : 0; pg_unlock(); return r; }
int recs_badges(const Item *it) {
  if (it->id[0] != 't' || it->id[1] != 't') return 0;
  Page *p = pg_lock(); if (!p) return 0; const PCard *c = card_of(p, it); int b = 0;
  if (c) { if (c->flags & CF_LOVE) b |= RECS_LOVE;
    /* "Recently added": out in the last 60 days (the year alone says too little) */
    long d = recs_days(c->date), t = today_days(); if (d && t - d >= 0 && t - d <= 60) b |= RECS_NEW; }
  pg_unlock(); return b;
}
void recs_dismiss(const Item *it) {
  unsigned num; int dig, key = 0; if (!tt_parse(it->id, &num, &dig)) return;
  pthread_mutex_lock(&mu);
  if (cur) { int i = page_find(cur, num, dig); if (i >= 0) { int tm = cur->cards[i].tmdb, k = cur->cards[i].kind; key = recs_key(tm, k ? 't' : 'm');
      for (int j = 0; j < cur->h.ncards; j++) if (cur->cards[j].tmdb == tm && cur->cards[j].kind == k) cur->cards[j].flags |= CF_HIDE; } }
  pthread_mutex_unlock(&mu);
  if (!key) return;
  cJSON *dm = store_node("recsDismissed"); if (!cJSON_IsArray(dm)) { dm = cJSON_CreateArray(); store_set_node("recsDismissed", dm); }
  cJSON_AddItemToArray(dm, cJSON_CreateNumber(key)); store_set_node("recsDismissed", dm);
  ver_bump(); msg_post_type(MG_SAVE, NULL);
}

/* =====================================================================================================
   W: the run
   ===================================================================================================== */
static unsigned loadWant; /* the saved page asked for last (another profile since: an older answer is dropped) */
static int wbusy, wquiet, wlastOk, loadState;   /* set by W, read by M with atomics (loadState: 0 asked, 1 nothing usable, 2 page in) */
typedef struct { char imdb[16]; char tk; double w, age; } GSeed;
typedef struct { unsigned gen, sig; long today; char region[8], explicitL[48], regionL[48], audio[4]; int hideCinema, lowMem, nseeds, ndis, *dis; GSeed seeds[RECS_MAX_SEEDS]; } RunCfg;
static struct { int requests, cached, fails, netfail; } st;
typedef struct { const char *path, *key; long maxAge; } Req;
typedef void (*Got)(int i, int status, const unsigned char *b, size_t n, void *ud);
/* Asks for every request (the disk cache first, then TMDB through M), calls got for each answer as it comes, returns
   when all are in. 0: the run was cancelled, or the network is gone (6 failures in a row: the rest would each wait 20 s). */
static int fetch(unsigned G, const Req *rq, int n, Got got, void *ud, int low) {
  char *seen = calloc((size_t)(n ? n : 1), 1); int wait = 0, ok = 1; if (!seen) return 0;
  for (int i = 0; i < n; i++) {
    if (rq[i].key) { size_t len = 0; unsigned char *b = cache_read(rq[i].key, rq[i].maxAge, &len); if (b) { st.cached++; got(i, 200, b, len, ud); free(b); seen[i] = 1; continue; } }
    want_add(G, i, low, rq[i].path); wait++;
  }
  while (wait > 0 && ok) {
    Msg *m = msg_take();
    if (gen_now() != G) { msg_free(m); ok = 0; break; }
    if (m->type == MG_SAVE) page_save_cur();
    else if (m->type == MG_RESP && m->gen == G && m->tag >= 0 && m->tag < n && !seen[m->tag]) {
      seen[m->tag] = 1; wait--; st.requests++;
      if (m->status == 200 && m->body) { st.netfail = 0; if (rq[m->tag].key) cache_write(rq[m->tag].key, m->body, m->len); got(m->tag, 200, m->body, m->len, ud); }
      else { st.fails++; st.netfail = m->status == 0 ? st.netfail + 1 : 0; got(m->tag, m->status, NULL, 0, ud); if (st.netfail >= 6) ok = 0; }
    }
    msg_free(m);
  }
  free(seen); if (!ok && gen_now() == G) cancel_run(); return ok;
}

/* ---- one run, W only ---- */
typedef struct { unsigned key; int tmdb; char tk; } Need;
static struct {
  RunCfg *c; RecSeedX *X; RecCand *rb, *sb; int stm[RECS_MAX_SEEDS], slot[RECS_PAGE_IN + RECS_MAX_SEEDS];
  unsigned char okS[RECS_MAX_SEEDS]; RecRowDef *defs; RecCand **rc; int *rn; RecCand *tmp; int listsOK;
  PB *pb; Need *need; int nneed, pageMode;
} rs;
static const char *tr_cb(const char *s) { return T(s); }   /* (a dictionary entry: valid until the next language change) */
static void got_find(int i, int status, const unsigned char *b, size_t n, void *ud) {
  (void)ud; if (status != 200) return; int s = rs.slot[i]; const GSeed *g = &rs.c->seeds[s]; unsigned num; int dig;
  cJSON *j = cJSON_ParseWithLength((const char *)b, n); if (!j) return;
  const cJSON *e = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j, g->tk == 'm' ? "movie_results" : "tv_results"), 0); int id = e ? (int)rjn(e, "id") : 0;
  if (id > 0 && tt_parse(g->imdb, &num, &dig)) { rs.stm[s] = id; id_add(ckey(g->tk, id), num, dig); }
  cJSON_Delete(j);
}
static void got_seed(int i, int status, const unsigned char *b, size_t n, void *ud) {
  (void)ud; if (status != 200) return; int s = rs.slot[i]; const GSeed *g = &rs.c->seeds[s]; RecSeedX *x = &rs.X[s]; unsigned num; int dig;
  cJSON *j = cJSON_ParseWithLength((const char *)b, n); if (!j) return; memset(x, 0, sizeof *x);
  if (recs_parse_seed(j, g->tk, x, rs.rb + (size_t)s * RECS_LIST, RECS_LIST, rs.sb + (size_t)s * RECS_LIST, RECS_LIST)) {
    rs.okS[s] = 1; x->w = g->w; x->ageDays = g->age;
    if (tt_parse(x->imdb[0] == 't' ? x->imdb : g->imdb, &num, &dig)) id_add(ckey(g->tk, x->tmdb), num, dig); }   /* the details carry the seed's own id */
  cJSON_Delete(j);
}
static void got_row(int i, int status, const unsigned char *b, size_t n, void *ud) {
  (void)ud; if (status != 200) return; int d = rs.slot[i]; cJSON *j = cJSON_ParseWithLength((const char *)b, n); if (!j) return;
  int k = recs_parse(cJSON_GetObjectItemCaseSensitive(j, "results"), rs.defs[d].kind, rs.tmp, RECS_LIST); rs.listsOK++;
  if (k > 0 && (rs.rc[d] = malloc(sizeof(RecCand) * (size_t)k))) { memcpy(rs.rc[d], rs.tmp, sizeof(RecCand) * (size_t)k); rs.rn[d] = k; }
  cJSON_Delete(j);
}
/* an id found (or a title with none) goes to every card of that title: in the builder's copy before the page is out, in the live page after */
static void ids_apply(unsigned key, unsigned num, int dig, int noid) {
  if (rs.pageMode) {
    pthread_mutex_lock(&mu); Page *p = cur;
    for (int k = 0; p && k < p->h.ncards; k++) { PCard *c = &p->cards[k]; if (c->imdb || ckey(c->kind ? 't' : 'm', c->tmdb) != key) continue;
      if (noid) c->flags |= CF_NOID; else { c->imdb = num; c->dig = (unsigned char)dig; page_ix_add(p, k); } }
    pthread_mutex_unlock(&mu);
  } else for (int k = 0; k < rs.pb->nc; k++) { BCard *c = &rs.pb->c[k]; if (c->imdb || ckey(c->c->kind, c->c->tmdb) != key) continue;
    if (noid) c->flags |= CF_NOID; else { c->imdb = num; c->dig = (unsigned char)dig; } }
}
static void got_ext(int i, int status, const unsigned char *b, size_t n, void *ud) {
  (void)ud; unsigned key = rs.need[i].key, num; int dig;
  if (status == 200) { cJSON *j = cJSON_ParseWithLength((const char *)b, n); if (!j) return;
    if (tt_parse(rjs(j, "imdb_id"), &num, &dig)) { id_add(key, num, dig); ids_apply(key, num, dig, 0); } else ids_apply(key, 0, 0, 1);   /* TMDB has no IMDb id for it */
    cJSON_Delete(j); }
  else if (status == 404) ids_apply(key, 0, 0, 1);
}
static void need_add(char kind, int tmdb) {
  unsigned key = ckey(kind, tmdb); for (int i = 0; i < rs.nneed; i++) if (rs.need[i].key == key) return;
  rs.need[rs.nneed].key = key; rs.need[rs.nneed].tmdb = tmdb; rs.need[rs.nneed].tk = kind; rs.nneed++;
}
static int ext_fetch(unsigned G, int low) {
  Req *rq = calloc((size_t)rs.nneed, sizeof *rq); char (*ep)[40] = calloc((size_t)rs.nneed, sizeof *ep); if (!rq || !ep) { free(rq); free(ep); return 0; }
  for (int i = 0; i < rs.nneed; i++) { snprintf(ep[i], 40, "/%s/%d/external_ids", rs.need[i].tk == 'm' ? "movie" : "tv", rs.need[i].tmdb); rq[i].path = ep[i]; }
  int ok = fetch(G, rq, rs.nneed, got_ext, NULL, low); free(rq); free(ep); return ok;
}
/* what the first wave must ask: the billboard, and for every row the first SHOWN_MIN cards that did not come back without an id */
static void pb_wave(void) {
  const PB *b = rs.pb; rs.nneed = 0;
  for (int i = 0; i < b->nbill; i++) if (!b->c[i].imdb && !(b->c[i].flags & CF_NOID)) need_add(b->c[i].c->kind, b->c[i].c->tmdb);
  for (int r = 0; r < b->nr; r++) { int taken = 0; for (int q = 0; q < b->r[r].n && taken < SHOWN_MIN; q++) { const BCard *c = &b->c[b->r[r].first + q];
      if (c->flags & CF_NOID) continue; taken++; if (!c->imdb) need_add(c->c->kind, c->c->tmdb); } }
}
static int pb_known(const PB *b, const BRow *r) { int k = 0; for (int q = 0; q < r->n; q++) k += b->c[r->first + q].imdb != 0; return k; }

/* returns 1 when a page went out; the stats and the reason are logged once */
static int run(RunCfg *c) {
  unsigned G = c->gen; int ok = 0, ns = c->nseeds, nd = 0, nx = 0, nl = 0, np = 0, nb = 0, rows = 0, missing = 0, kb = 0, nex = 0; const char *why = "cancelled";
  long long w0 = us_now(), cpu0 = cpu_ms(); memset(&st, 0, sizeof st); memset(&rs, 0, sizeof rs); rs.c = c;
  fprintf(stderr, "[recs] run: %d seeds, region %s\n", ns, c->region);
  id_load();
  rs.X = calloc(RECS_MAX_SEEDS, sizeof *rs.X); rs.rb = calloc(RECS_MAX_SEEDS * RECS_LIST, sizeof(RecCand)); rs.sb = calloc(RECS_MAX_SEEDS * RECS_LIST, sizeof(RecCand));
  RecSeedX *D = calloc(RECS_MAX_SEEDS, sizeof *D); RecTaste *taste = calloc(1, sizeof *taste); rs.defs = calloc(RECS_PAGE_IN, sizeof *rs.defs);
  rs.rc = calloc(RECS_PAGE_IN, sizeof *rs.rc); rs.rn = calloc(RECS_PAGE_IN, sizeof *rs.rn); rs.tmp = calloc(RECS_LIST, sizeof(RecCand));
  RecCand *picks = calloc(60, sizeof(RecCand)); RecRowIn *in = calloc(RECS_PAGE_IN, sizeof *in); RecCand **fin = calloc(RECS_PAGE_IN, sizeof *fin);
  RecPage *pgb = calloc(1, sizeof *pgb); rs.pb = calloc(1, sizeof *rs.pb); rs.need = malloc(sizeof(Need) * PG_CARDS); int *ex = malloc(sizeof(int) * (size_t)(RECS_MAX_SEEDS + c->ndis + 1));
  RecLang L[RECS_MAX_LANGS]; RecPrefs P;
  if (!rs.X || !rs.rb || !rs.sb || !D || !taste || !rs.defs || !rs.rc || !rs.rn || !rs.tmp || !picks || !in || !fin || !pgb || !rs.pb || !rs.need || !ex) { why = "out of memory"; goto out; }
  { /* 1. each seed's TMDB id: the table, else /find */
    Req rq[RECS_MAX_SEEDS]; char keys[RECS_MAX_SEEDS][40], paths[RECS_MAX_SEEDS][80]; int nr = 0;
    for (int i = 0; i < ns; i++) { const GSeed *g = &c->seeds[i]; unsigned num; int dig; if (!tt_parse(g->imdb, &num, &dig)) continue;
      if ((rs.stm[i] = id_tmdb_of(num, g->tk == 't'))) continue;
      snprintf(paths[nr], sizeof paths[nr], "/find/%s?external_source=imdb_id", g->imdb); snprintf(keys[nr], sizeof keys[nr], "recs:find:%s", g->imdb);
      rq[nr].path = paths[nr]; rq[nr].key = keys[nr]; rq[nr].maxAge = 30 * 86400; rs.slot[nr++] = i; }
    if (nr && !fetch(G, rq, nr, got_find, NULL, 0)) goto out;
    /* 2. their details (keywords, credits, recommendations, similar, external ids) */
    char dk[RECS_MAX_SEEDS][40], dp[RECS_MAX_SEEDS][160]; nr = 0;
    for (int i = 0; i < ns; i++) { const GSeed *g = &c->seeds[i]; if (!rs.stm[i]) continue;
      snprintf(dp[nr], sizeof dp[nr], "/%s/%d?append_to_response=keywords,credits,recommendations,similar,external_ids", g->tk == 'm' ? "movie" : "tv", rs.stm[i]);
      snprintf(dk[nr], sizeof dk[nr], "recs:seed:%c%d", g->tk, rs.stm[i]); rq[nr].path = dp[nr]; rq[nr].key = dk[nr]; rq[nr].maxAge = RECS_AGE_SEED; rs.slot[nr++] = i; }
    if (nr && !fetch(G, rq, nr, got_seed, NULL, 0)) goto out;
    for (int i = 0; i < ns; i++) if (rs.okS[i] && (!rs.X[i].year || rs.X[i].year >= 1930)) D[nx++] = rs.X[i];   /* (the lists they point to stay where they are; a silent-era title in a library is a curiosity or a test, and its rows would be nothing but 1890s shorts) */
    if (ns > 0 && nx == 0) { why = "no seed details"; goto out; } }
  /* 3. taste, languages, candidate rows */
  recs_taste_build(D, nx, taste);
  nl = recs_langs_allowed(c->explicitL, c->regionL, c->audio, taste, L);
  { RecRowCtx cx = { .region = c->region, .today = c->today, .langs = L, .nlangs = nl, .taste = taste, .seeds = D, .nseeds = nx, .lowMem = c->lowMem, .tr = tr_cb };
    i18n_lock(); nd = recs_rows_build(&cx, rs.defs, RECS_PAGE_IN); i18n_unlock(); }
  /* 4. every def with a path, 3 at a time (M), the disk cache first */
  { Req *rq = calloc((size_t)(nd ? nd : 1), sizeof *rq); int nr = 0, f; if (!rq) { why = "out of memory"; goto out; }
    for (int d = 0; d < nd; d++) if (rs.defs[d].path[0]) { rq[nr].path = rs.defs[d].path; rq[nr].key = rs.defs[d].key; rq[nr].maxAge = rs.defs[d].maxAge; rs.slot[nr++] = d; }
    f = fetch(G, rq, nr, got_row, NULL, 0); free(rq); if (!f) goto out;
    if (nr && !rs.listsOK) { why = "no list answered"; goto out; } }
  /* 5. each Because-you-watched row's own list feeds Top picks; Top picks (window 28: 8 billboard + the row); then the rows themselves */
  for (int d = 0; d < nd; d++) if (rs.defs[d].family == RF_BYW && rs.defs[d].seed >= 0 && rs.defs[d].seed < nx) { D[rs.defs[d].seed].own = rs.rc[d]; D[rs.defs[d].seed].nown = rs.rn[d]; }
  for (int k = 0; k < nx; k++) ex[nex++] = recs_key(D[k].tmdb, D[k].kind);
  for (int k = 0; k < c->ndis; k++) ex[nex++] = c->dis[k];
  memset(&P, 0, sizeof P); for (int l = 0; l < nl; l++) memcpy(P.langs[P.nlangs++], L[l].code, 4);
  P.exclude = ex; P.nexclude = nex; P.today = c->today; P.minMovieAge = c->hideCinema ? 30 : 0;
  np = nx ? recs_top_picks(D, nx, taste, &P, 28, picks, 60) : 0;
  for (int d = 0; d < nd; d++) { RecRowIn *r = &in[d]; r->def = &rs.defs[d];
    if (rs.defs[d].family == RF_PERSONAL) { r->cards = picks + BILL; r->n = np > BILL ? (np - BILL < RECS_ROW_CARDS ? np - BILL : RECS_ROW_CARDS) : 0; }
    else if (rs.defs[d].family == RF_BYW) { r->n = 0; r->cards = NULL;
      if (rs.defs[d].seed >= 0 && rs.defs[d].seed < nx && (fin[d] = malloc(sizeof(RecCand) * RECS_ROW_CARDS))) { r->cards = fin[d]; r->n = recs_byw_rank(&D[rs.defs[d].seed], &P, rs.rc[d], rs.rn[d], fin[d], RECS_ROW_CARDS); } }
    else { r->cards = rs.rc[d]; r->n = rs.rn[d] < RECS_ROW_CARDS ? rs.rn[d] : RECS_ROW_CARDS; } }
  /* 6. the page: rows chosen and ordered by the builder, then the working copy with the ids we already have */
  { int bill[BILL]; nb = np < BILL ? np : BILL; for (int i = 0; i < nb; i++) bill[i] = recs_key(picks[i].tmdb, picks[i].kind);
    RecPageCtx pc = { .taste = taste, .today = c->today, .maxRows = c->lowMem ? 20 : 36, .bill = bill, .nbill = nb, .exclude = ex, .nexclude = nex, .allowAdult = 0, .minMovieAge = P.minMovieAge };
    recs_page_build(&pc, in, nd, pgb); }
  { PB *b = rs.pb; for (int i = 0; i < nb; i++) b->c[b->nc++].c = &picks[i]; b->nbill = nb;
    for (int r = 0; r < pgb->n && b->nr < RECS_PAGE_MAX; r++) { const RecPageRow *pr = &pgb->rows[r]; BRow *br = &b->r[b->nr++]; br->def = in[pr->in].def; br->first = b->nc; br->n = pr->n;
      for (int q = 0; q < pr->n; q++) b->c[b->nc++].c = &in[pr->in].cards[pr->idx[q]]; }
    for (int k = 0; k < b->nc; k++) { const IdRec *rec = id_find(ckey(b->c[k].c->kind, b->c[k].c->tmdb)); if (rec) { b->c[k].imdb = rec->num; b->c[k].dig = (unsigned char)rec->dig; } } }
  /* 7. IMDb ids, pass 1: the billboard and the first 8 cards of every row; a title with none is replaced by the next card; a row that cannot reach 8 is dropped */
  for (int wave = 0; wave < 4; wave++) { pb_wave(); if (!rs.nneed) break; if (!ext_fetch(G, 0)) goto out; }
  { PB *b = rs.pb; int k = 0, bk = 0; for (int r = 0; r < b->nr; r++) if (pb_known(b, &b->r[r]) >= SHOWN_MIN) b->r[k++] = b->r[r]; b->nr = k;
    for (int i = 0; i < b->nbill; i++) bk += b->c[i].imdb != 0;
    if (!b->nr && !bk) { why = "no row with enough known titles"; goto out; } }
  { Page *pg = page_make(rs.pb, c->sig); if (!pg) { why = "out of memory"; goto out; }
    if (gen_now() != G) { free(pg); goto out; }
    kb = (int)((sizeof(Page) + page_bytes(pg) + 2 * (size_t)pg->ixn + 1023) / 1024); rows = pg->h.nrows;
    pthread_mutex_lock(&mu); Page *old = cur; cur = pg; pageAt = (long)time(NULL); pthread_mutex_unlock(&mu); free(old); ver_bump(); ok = 1; }
  id_save(); page_save_cur();
  /* 8. pass 2, quietly and without a version bump: the rest of the ids, 2 at a time and only while nobody is pressing keys */
  rs.pageMode = 1; rs.nneed = 0; __atomic_store_n(&wquiet, 1, __ATOMIC_RELEASE);
  pthread_mutex_lock(&mu); for (int k = 0; cur && k < cur->h.ncards; k++) { const PCard *pc = &cur->cards[k]; if (!pc->imdb && !(pc->flags & CF_NOID) && rs.nneed < PG_CARDS) need_add(pc->kind ? 't' : 'm', pc->tmdb); } pthread_mutex_unlock(&mu);
  if (rs.nneed) ext_fetch(G, 1);
  pthread_mutex_lock(&mu); for (int k = 0; cur && k < cur->h.ncards; k++) missing += cur->cards[k].imdb == 0 && !(cur->cards[k].flags & CF_NOID); pthread_mutex_unlock(&mu);
  page_save_cur(); id_save();
  why = "";
out:
  if (st.netfail >= 6) why = "network unreachable";
  fprintf(stderr, "[recs] %s: seeds %d, candidate rows %d, rows on page %d, requests %d, cached %d, failed %d, ids missing %d, page %d KB, worker %lld ms cpu / %lld ms wall%s%s\n",
    ok ? "ready" : "kept the old page", nx, nd, rows, st.requests, st.cached, st.fails, missing, kb, cpu_ms() - cpu0, (us_now() - w0) / 1000, why[0] ? ": " : "", why);
  for (int d = 0; d < RECS_PAGE_IN; d++) { if (rs.rc) free(rs.rc[d]); if (fin) free(fin[d]); }
  free(rs.X); free(rs.rb); free(rs.sb); free(D); free(taste); free(rs.defs); free(rs.rc); free(rs.rn); free(rs.tmp); free(picks); free(in); free(fin); free(pgb); free(rs.pb); free(rs.need); free(ex);
  id_save(); id_free(); memset(&rs, 0, sizeof rs);
  return ok;
}

/* ---- the other jobs W gets ---- */
typedef struct { IdRec *r; int n; } Mig;
static void do_load(const Msg *m) {
  Page *p = page_load(m->gen, m->tag);
  if (m->gen != __atomic_load_n(&loadWant, __ATOMIC_ACQUIRE)) { free(p); return; }
  if (p) { id_load(); for (int k = 0; k < p->h.ncards; k++) { PCard *c = &p->cards[k]; if (!c->imdb && !(c->flags & CF_NOID)) { const IdRec *r = id_find(ckey(c->kind ? 't' : 'm', c->tmdb)); if (r) { c->imdb = r->num; c->dig = (unsigned char)r->dig; } } if (c->imdb) page_ix_add(p, k); }
    id_free();
    pthread_mutex_lock(&mu); Page *old = cur; cur = p; pageAt = (long)p->h.at; pthread_mutex_unlock(&mu); free(old); ver_bump(); }
  __atomic_store_n(&loadState, p ? 2 : 1, __ATOMIC_RELEASE);
}
static void do_mig(Mig *g) { id_load(); for (int i = 0; i < g->n; i++) id_add(g->r[i].key, g->r[i].num, g->r[i].dig); id_save(); id_free(); free(g->r); free(g); }
static void *worker_main(void *a) {
  plat_thread_background(10); (void)a;
  for (;;) {
    Msg *m = msg_take();
    switch (m->type) {
      case MG_LOAD: do_load(m); break;
      case MG_DROP: { pthread_mutex_lock(&mu); Page *old = cur; cur = NULL; pageAt = 0; pthread_mutex_unlock(&mu); free(old); ver_bump(); break; }
      case MG_RUN: { RunCfg *c = m->p; int ok = c->gen == gen_now() ? run(c) : 0; __atomic_store_n(&wlastOk, ok, __ATOMIC_RELEASE); free(c->dis); free(c); __atomic_store_n(&wquiet, 0, __ATOMIC_RELEASE); __atomic_store_n(&wbusy, 0, __ATOMIC_RELEASE); break; }
      case MG_SAVE: page_save_cur(); break;
      case MG_MIG: do_mig(m->p); break;
      default: break;   /* a late answer of a run that is over, or a wake-up */
    }
    msg_free(m);
  }
  return NULL;
}

/* =====================================================================================================
   M: the requests W asks for, gathering the seeds, the old id map, the tick
   ===================================================================================================== */
static int inflight, inflightLow;
typedef struct { unsigned gen; int tag, low; } RCtx;
static void on_resp(int status, const unsigned char *body, size_t len, void *ud) {
  RCtx *c = ud; inflight--; inflightLow -= c->low;
  if (c->gen == gen_now()) { Msg *m = calloc(1, sizeof *m);   /* (the body is freed after this returns: a copy goes to W, which parses it) */
    if (m) { m->type = MG_RESP; m->gen = c->gen; m->tag = c->tag; m->status = status;
      if (status == 200 && body) { m->body = malloc(len + 1); if (m->body) { memcpy(m->body, body, len); m->body[len] = 0; m->len = len; } else m->status = 0; }
      msg_post(m); } }
  free(c);
}
static void pump_wants(void) {
  while (inflight < INFLIGHT) {
    int idle = app_idle() >= 0.5f; Want *w = NULL;
    pthread_mutex_lock(&mu);
    for (Want **pp = &wH; *pp;) { Want *x = *pp; if (x->gen != gen) { *pp = x->next; free(x); continue; }
      if (x->low && (inflightLow >= 2 || !idle)) { pp = &x->next; continue; }   /* the quiet lookups wait for a moment nobody is pressing keys */
      *pp = x->next; w = x; break; }
    pthread_mutex_unlock(&mu);
    if (!w) return;
    RCtx *c = malloc(sizeof *c); if (!c) { free(w); return; } c->gen = w->gen; c->tag = w->tag; c->low = w->low;
    inflight++; inflightLow += c->low;
    if (!tmdb_request(w->path, on_resp, c)) on_resp(0, NULL, 0, c);
    free(w);
  }
}
/* the library's strongest seeds, in slices of SLICE_US (a big library is hundreds of entries). If the library or the store changes
   under the walk it starts again (the walk holds a pointer into the library) */
static GSeed gAll[256]; static cJSON *gIt; static int gN, gRestart; static unsigned gLib, gSer, curSig;
static int cmp_seed(const void *a, const void *b) { double x = ((const GSeed *)a)->w, y = ((const GSeed *)b)->w; return x < y ? 1 : x > y ? -1 : 0; }
static void gather_begin(void) { const cJSON *l = lib_all(); gIt = l ? l->child : NULL; gN = 0; gLib = lib_version(); gSer = store_serial(); }
static void run_post(void) {
  qsort(gAll, (size_t)gN, sizeof gAll[0], cmp_seed);
  RunCfg *c = calloc(1, sizeof *c); if (!c) return;
  c->gen = gen_now(); c->sig = curSig; c->today = today_days(); snprintf(c->region, sizeof c->region, "%.7s", recs_region());
  snprintf(c->explicitL, sizeof c->explicitL, "%.47s", store_get("recsLangs", "")); snprintf(c->regionL, sizeof c->regionL, "%.47s", region_langs(c->region));
  audio_code(c->audio); c->hideCinema = recs_hide_cinema(); { int mb = plat_mem_total_mb(); c->lowMem = mb > 0 && mb < 1200; }
  c->nseeds = gN < RECS_MAX_SEEDS ? gN : RECS_MAX_SEEDS; memcpy(c->seeds, gAll, sizeof(GSeed) * (size_t)c->nseeds);
  const cJSON *dm = store_node("recsDismissed"), *d; int n = cJSON_IsArray(dm) ? cJSON_GetArraySize(dm) : 0, skip = n > 600 ? n - 600 : 0;
  c->dis = malloc(sizeof(int) * (size_t)(n + 1)); if (c->dis) cJSON_ArrayForEach(d, dm) { if (skip > 0) { skip--; continue; } if (cJSON_IsNumber(d)) c->dis[c->ndis++] = d->valueint; }
  __atomic_store_n(&wbusy, 1, __ATOMIC_RELEASE); msg_post_type(MG_RUN, c);
}
/* 1 when the walk is finished and the run has been handed to W, -1 to give up for now */
static int gather_step(double now, double *startAt) {
  if (lib_version() != gLib || store_serial() != gSer) { if (++gRestart > 3) { gRestart = 0; *startAt = now + 30; return -1; } gather_begin(); return 0; }
  long long t0 = us_now(); long today = today_days(); int k = 0;
  while (gIt && gN < 256) { const cJSON *it = gIt; gIt = gIt->next;
    const char *id = rjs(it, "_id"), *ty = rjs(it, "type");
    if (!strncmp(id, "tt", 2) && (!strcmp(ty, "movie") || !strcmp(ty, "series")) && strlen(id) < sizeof gAll[0].imdb) {
      const cJSON *s = cJSON_GetObjectItemCaseSensitive(it, "state"); double dur = rjn(s, "duration"), off = rjn(s, "timeOffset"), overall = rjn(s, "overallTimeWatched");
      int marked = rjn(s, "flaggedWatched") > 0 || rjn(s, "timesWatched") > 0, series = !strcmp(ty, "series");
      double frac = marked ? 1 : dur > 0 ? off / dur : 0; int eps = series && dur > 0 ? (int)(overall / dur) : 0;
      const char *lw = rjs(s, "lastWatched"); if (!lw[0]) lw = rjs(it, "_mtime"); long d = recs_days(lw); double age = d ? (double)(today - d) : 365;
      double w = recs_seed_weight(age, frac, marked, series, eps);
      if (w > 0.001) { GSeed *x = &gAll[gN++]; memset(x, 0, sizeof *x); snprintf(x->imdb, sizeof x->imdb, "%s", id); x->tk = series ? 't' : 'm'; x->w = w; x->age = age; } }
    if ((++k & 7) == 0 && us_now() - t0 > SLICE_US) return 0; }
  gRestart = 0; run_post(); return 1;
}
/* the old `recsIds` node of the store, moved into the id file: taken apart a few entries per tick (each is detached and freed, so the
   store keeps shrinking and its last flush is small), then handed to W in one piece */
static int migState; static cJSON *migNode; static IdRec *migBuf; static int migN, migCap;
static void mig_step(void) {
  if (migState == 0) { cJSON *n = store_node("recsIds"); if (!cJSON_IsObject(n)) { migState = 2; return; } migNode = n; migState = 1; }
  long long t0 = us_now(); int k = 0; cJSON *c;
  while ((c = migNode->child)) {
    cJSON_DetachItemViaPointer(migNode, c);
    if (cJSON_IsString(c) && c->valuestring && c->string && (c->string[0] == 'm' || c->string[0] == 't')) { unsigned num; int dig;   /* "m123": "tt0816692" (the objects keyed by tt carry nothing new) */
      if (tt_parse(c->valuestring, &num, &dig) && atoi(c->string + 1) > 0) {
        if (migN == migCap) { int nc = migCap ? migCap * 2 : 1024; IdRec *nb = realloc(migBuf, sizeof *nb * (size_t)nc); if (!nb) { cJSON_Delete(c); continue; } migBuf = nb; migCap = nc; }
        migBuf[migN].key = ckey(c->string[0], atoi(c->string + 1)); migBuf[migN].num = num; migBuf[migN].dig = (unsigned char)dig; migN++; } }
    cJSON_Delete(c);
    if ((++k & 15) == 0 && us_now() - t0 > SLICE_US) return; }
  store_remove("recsIds"); migState = 2;
  if (migN) { Mig *g = malloc(sizeof *g); if (g) { g->r = migBuf; g->n = migN; msg_post_type(MG_MIG, g); } else free(migBuf); } else free(migBuf);
  migBuf = NULL; migN = migCap = 0;
}

static int phase, loaded, force; static double startAt, libAt, loadT0; static unsigned libSeen;   /* phase: 0 idle, 1 gathering seeds, 2 W has the run */
static void worker_start(void) { if (workerOn) return; pthread_t t; if (!pthread_create(&t, NULL, worker_main, NULL)) { pthread_detach(t); workerOn = 1; } }
void recs_refresh(void) { force = 1; startAt = app_now(); if (phase == 2) cancel_run(); else phase = 0; }
int recs_region_count(void) { return NREG; }
const char *recs_region_code(int i) { return i >= 0 && i < NREG ? REGIONS[i].cc : ""; }
void recs_status(char *out, size_t cap) {
  if (!tmdb_has_key()) { snprintf(out, cap, "%s", T("Needs a TMDB key: add it below")); return; }
  if (!recs_on()) { snprintf(out, cap, "%s", T("Off: Home shows your addons' rows only")); return; }
  if (phase == 1 || (phase == 2 && !__atomic_load_n(&wquiet, __ATOMIC_ACQUIRE))) { snprintf(out, cap, "%s", T("On. Getting your picks from TMDB...")); return; }
  int picks = 0; long at = 0; pthread_mutex_lock(&mu);
  if (cur) { at = pageAt; for (int i = 0; i < cur->h.nbill; i++) picks += vis(&cur->cards[i]); if (cur->h.nrows) { const PRow *r = &cur->rows[0]; for (int q = 0; q < r->n; q++) picks += vis(&cur->cards[r->first + q]); } }
  pthread_mutex_unlock(&mu);
  if (!picks) { snprintf(out, cap, "%s", T("On. Watch something and your picks appear on Home")); return; }
  long mins = at ? (long)(time(NULL) - at) / 60 : -1;
  if (mins < 0) snprintf(out, cap, "%s %d %s", T("On."), picks, T("picks on Home"));
  else if (mins < 60) snprintf(out, cap, "%s %d %s, %ld %s", T("On."), picks, T("picks on Home"), mins, T("min ago"));
  else snprintf(out, cap, "%s %d %s, %ld %s", T("On."), picks, T("picks on Home"), mins / 60, T("h ago"));
}
static unsigned sigSer; static char sigNow[200];
void recs_account_changed(void) {
  if (phase == 2) cancel_run(); phase = 0; force = 0; libAt = 0; loaded = 0; __atomic_store_n(&loadState, 0, __ATOMIC_RELEASE);
  sigSer = store_serial(); curSig = settings_sig(sigNow, sizeof sigNow); __atomic_store_n(&loadWant, curSig, __ATOMIC_RELEASE); page_file_set(); /* the new profile's own page is read in the next tick: not a change of settings */
  if (workerOn) msg_post_type(MG_DROP, NULL);
}
void recs_tick(void) {
  { if (store_serial() != sigSer) { sigSer = store_serial(); char s2[200]; unsigned h = settings_sig(s2, sizeof s2);
      if (sigNow[0] && strcmp(s2, sigNow)) recs_refresh(); snprintf(sigNow, sizeof sigNow, "%s", s2); curSig = h; } } /* a setting changed (Settings writes the store): ask again */
  double now = app_now();
  if (workerOn) pump_wants();
  if (migState != 2 && app_splash_done && app_idle() >= 0.5f) { worker_start(); if (workerOn) mig_step(); }
  int on = recs_on() && tmdb_has_key();
  { pthread_mutex_lock(&mu); int was = shown, has = cur != NULL; shown = on; pthread_mutex_unlock(&mu); if (was != on && has) ver_bump(); }   /* off: Home loses the rows, the page stays in memory */
  if (phase == 2 && !__atomic_load_n(&wbusy, __ATOMIC_ACQUIRE)) { phase = 0; if (!force) startAt = now + (__atomic_load_n(&wlastOk, __ATOMIC_ACQUIRE) ? RESULT_AGE : 300); }
  if (!on) { if (phase == 2) cancel_run(); else phase = 0; return; }
  if (!loaded) { worker_start(); if (!workerOn) return; loaded = 1; loadT0 = now; startAt = now + 4; libSeen = lib_version();
    Msg *m = calloc(1, sizeof *m); if (m) { m->type = MG_LOAD; m->gen = curSig; m->tag = RESULT_AGE * 4; page_file_set(); __atomic_store_n(&loadWant, curSig, __ATOMIC_RELEASE); msg_post(m); } else loadState = 1; }   /* the saved page: read by W, no parse here */
  if (loaded == 1 && __atomic_load_n(&loadState, __ATOMIC_ACQUIRE)) { loaded = 2; if (loadState == 2 && !force && startAt < loadT0 + 20) startAt = loadT0 + 20; }   /* a saved page: asking again waits for Home */
  if (!recs_paused() && lib_version() != libSeen) { libSeen = lib_version(); libAt = now + 30; } /* watched something: again in 30 s (one change, not each progress save) */
  if (libAt && now >= libAt && (phase != 2 || __atomic_load_n(&wquiet, __ATOMIC_ACQUIRE))) { libAt = 0; force = 1; startAt = now; if (phase == 2) cancel_run(); else phase = 0; }
  if (phase == 0) {
    if (now < startAt || !app_splash_done || app_idle() < 0.5f || loaded < 2 || migState != 2) return;
    pthread_mutex_lock(&mu); long at = pageAt; pthread_mutex_unlock(&mu);
    if (!force && at && time(NULL) - at < RESULT_AGE) { startAt = now + 600; return; } /* fresh enough */
    gather_begin(); phase = 1; }
  if (phase == 1) { int r = gather_step(now, &startAt); if (r < 0) phase = 0; else if (r > 0) { phase = 2; force = 0; __atomic_store_n(&wquiet, 0, __ATOMIC_RELEASE); } }
}

