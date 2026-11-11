/* The recommendation maths, with no app dependencies (cJSON only), so tests/recs can run it on fixtures.
   recs.c feeds it: the watched titles (seeds) with their TMDB ids, genres and language, each seed's
   TMDB /recommendations list, and the region's popular lists; it gives back ranked candidates.

   Score of a candidate c (all parts 0..1, weights in RecWeights):
     seed    sum over the seeds s listing c: w(s) * (1 - rank(c in s)/20), divided by the sum of all w(s)
     genre   how much of c's genres the user watches (the taste vector, normalised)
     lang    half for a language the user picked, half for the share of their watching in that language
     quality vote_average/10, damped below 200 votes
     fresh   1 for a title out today, falling to 0 at two years; old: a penalty growing past 8 years
   A seed's weight w = recency (half-life RECS_HALF_LIFE_D days) x completion (1 if >= 70% watched or
   marked watched, 0.5 for 20-70%, 0.25 for 5-20%, 0 under 5%; a series once started 0.5 at least), x1.3 for a
   series with 3+ episodes watched. */
#ifndef SB_RECS_CORE_H
#define SB_RECS_CORE_H
#include "cJSON.h"

#define RECS_MAX_SEEDS 15
#define RECS_LIST 20          /* one /recommendations page */
#define RECS_MAX_CAND 400
#define RECS_GENRES 64        /* TMDB genre ids are < 11000; they are folded into this many slots */
#define RECS_HALF_LIFE_D 21.0
#define RECS_ROW_FLOOR 0.72     /* a because-you-watched row stops (after 8) below this share of its best score */

typedef struct {
  int tmdb; char kind;                /* 'm' movie, 't' tv */
  char title[128], poster[64], backdrop[64], date[12], lang[4], overview[600];
  int genres[8], ng, adult;
  float vote; int votes; float pop;
  /* filled by recs_rank */
  double score, seedPart; int bestSeed; int nSeeds;
} RecCand;

typedef struct {
  int tmdb; char kind; char title[128];
  int genres[8], ng; char lang[4];
  double w;                           /* recs_seed_weight */
  const RecCand *list; int n;         /* its /recommendations, in TMDB's order */
  const RecCand *disc; int nd;        /* popular titles in its language and main genre (/discover): TMDB's
                                         recommendations for Indian titles are mostly Western, this list is not */
} RecSeed;

typedef struct { double seed, genre, lang, quality, fresh, old, disc; } RecWeights;
extern const RecWeights RECS_W;       /* see recs_core.c */

typedef struct {
  char langs[8][4]; int nlangs;       /* the languages picked in Settings ("hi", "en") */
  float langPick[8];                  /* per langs[i]: 1 main, 0.6 second, 0 not chosen by the user (Top picks gives a chosen one a floor) */
  const int *exclude; int nexclude;   /* TMDB ids (kind folded in: recs_key) already watched or dismissed */
  long today;                         /* days since 1970, for "fresh" */
  int allowAdult;
  int minMovieAge;                    /* days: films out in cinemas more recently than this are left out (0: none) */
} RecPrefs;

/* a seed's weight from what the library says */
double recs_seed_weight(double ageDays, double fracWatched, int markedWatched, int isSeries, int episodesWatched);
/* one key for a TMDB id and its kind (movie and tv ids overlap) */
int  recs_key(int tmdb, char kind);
/* a TMDB result array (/recommendations, /similar, /discover, /trending) into candidates; kind 0 = read
   "media_type" per entry. Returns the count kept (people and entries without art are skipped). */
int  recs_parse(const cJSON *results, char kind, RecCand *out, int max);
/* "2026-03-14" -> days since 1970 (0 when unreadable) */
long recs_days(const char *ymd);

/* merges the seeds' lists, scores, drops excluded / adult / duplicate titles, and sorts best first.
   Diversity: within the first 20, at most 4 come from the same best seed. Returns the count in out. */
int  recs_rank(const RecSeed *seeds, int ns, const RecPrefs *p, RecCand *out, int max);
/* "Because you watched <seed>": that seed's own list, scored the same way, minus the ids in skip
   (e.g. the billboard's). */
int  recs_rank_seed(const RecSeed *seed, const RecSeed *all, int ns, const RecPrefs *p, const int *skip, int nskip, RecCand *out, int max);
/* a popular list (Top 10) filtered by the prefs' exclusions and adult flag, kept in its own order */
int  recs_filter(const RecCand *in, int n, const RecPrefs *p, RecCand *out, int max);
/* Top 10 from one popular list per language (main language first): taken in turn, so the main language
   is not drowned by globally popular English titles; duplicates and excluded titles are skipped */
int  recs_interleave(const RecCand *const *lists, const int *n, int nlists, const RecPrefs *p, RecCand *out, int max);

/* =====================================================================================================
   Home at Netflix size (recs-PLAN-v2.md): the whole page is decided here, from pure functions.
   The caller (recs.c) runs the pipeline; recs_core never does I/O, has no global state and no threads:

     1. seeds     for each watched title fetch  /{movie|tv}/{id}?append_to_response=keywords,credits,
                  recommendations,similar,external_ids   (max age RECS_AGE_SEED) -> recs_parse_seed()
     2. taste     recs_taste_build()                                    (once per refresh)
     3. languages recs_langs_allowed()                                  (once per refresh)
     4. rows      recs_rows_build() -> RecRowDef[]  (about 70-110 candidate rows, ONE request each).
                  Fetch every def whose path[0] != 0 (cache key + max age are in the def), parse the answer
                  with recs_parse(results, def.kind, ...)  (def.kind 0 = "media_type" per entry).
                  Specials, computed locally with no request (path[0] == 0 or fed by an earlier step):
                    RF_PERSONAL  recs_top_picks()  -> cards; the caller takes the first 8 as the billboard and
                                 hands the next RECS_ROW_CARDS as the "Top picks for you" row
                    RF_BYW       the def's path is the seed's own-language list; run recs_byw_rank() on it
     5. page      recs_page_build() over all rows with their cards -> RecPage (indices only, no card copies)

   Memory: RecCand is about 1 KB. The caller keeps ONE array of cards per row (up to RECS_ROW_CARDS) and the
   page refers to them by (row, card index). recs_page_build allocates one scratch block (about 180 KB)
   per call and frees it before returning; nothing else here allocates except recs_top_picks (one pool).
   Titles: every user-visible English phrase is a literal in recs_core.c. They are passed through the
   RecRowCtx.tr callback (NULL = identity):
     - a title with no variable part ("Top 10 films this week", "Someone is lying") is tr(whole title)
     - a title with one variable part is tr(pattern) with its single "%s" replaced by the argument:
         "More with %s", "Directed by %s", "New on %s", "Because you watched %s" -> argument is a name/title
         "%s drama films", "New %s series", "The best-rated %s films" ...  -> argument is tr(language name)
       There is one whole pattern per genre and kind ("%s drama films" / "%s drama series", "%s sci-fi and
       fantasy series"), so a translator can reorder the whole phrase. Only the first "%s" is replaced and
       the translation is never used as a printf format. tr() must return a string that stays valid until
       the next call (a static table lookup is fine).
   ===================================================================================================== */

#define RECS_KW 24            /* keyword ids kept per seed */
#define RECS_ROW_CARDS 40     /* most cards a row may hand to the page builder (the page shows at most RECS_PAGE_CARDS) */
#define RECS_PAGE_CARDS 20
#define RECS_PAGE_MAX 64      /* most rows on a page */
#define RECS_PAGE_IN 128      /* most candidate rows the page builder looks at */
#define RECS_MAX_LANGS 3
#define RECS_AGE_LIST (24 * 3600)   /* max ages for the defs' caches */
#define RECS_AGE_CHART (12 * 3600)
#define RECS_AGE_SEED (7 * 24 * 3600) /* the caller uses this for the seed-details request */

/* ---- A. seed details ---- */
typedef struct { int id; char name[40]; } RecPerson;
typedef struct {
  int tmdb; char kind;                /* kind 'm' | 't' (set by the caller before recs_parse_seed) */
  char title[96]; char lang[4];       /* original_language */
  int genres[8], ng;                  /* TMDB genre ids of THIS kind (tv genres for a series) */
  int year;                           /* release / first air year, 0 if unknown */
  char imdb[16];                      /* "tt..." or "" */
  int kw[RECS_KW], nkw;               /* keyword ids */
  RecPerson cast[3]; int ncast;       /* the first three billed */
  RecPerson dir[2]; int ndir;         /* directors (crew job "Director"); for a series created_by */
  double w;                           /* recs_seed_weight, set by the caller */
  double ageDays;                     /* days since last watched, set by the caller (orders "most recent") */
  RecCand *recs; int nrecs;           /* recommendations.results (TMDB order), points into the caller's buffer */
  RecCand *sim; int nsim;             /* similar.results */
  const RecCand *own; int nown;       /* optional: the seed's own-language list = the parsed answer of its RF_BYW row (set by the
                                         caller once fetched); recs_top_picks uses it, because TMDB's lists for Indian titles are mostly Western */
} RecSeedX;
/* Parses the answer for one seed. kind 'm' or 't' (the JSON differs: keywords.keywords vs keywords.results,
   title vs name). Fills everything above except w and ageDays (left as the caller set them). The lists are
   written into recsBuf / simBuf (caller-owned, at least maxRecs / maxSim entries, RECS_LIST each is enough).
   Returns 1 on success, 0 if root is not an object with an id. */
int recs_parse_seed(const cJSON *root, char kind, RecSeedX *out, RecCand *recsBuf, int maxRecs, RecCand *simBuf, int maxSim);

/* ---- B. taste ---- */
#define RECS_T_KW 64
#define RECS_T_PEOPLE 48
typedef struct {
  double wsum;                                    /* sum of the seeds' weights */
  int gid[RECS_GENRES]; double gw[RECS_GENRES]; int ng; double gmax;
  char lang[16][4]; double lw[16]; int nl;
  double kindw[2];                                /* [0] movies, [1] series */
  int kw[RECS_T_KW]; double kww[RECS_T_KW]; int nkw;
  int pid[RECS_T_PEOPLE]; double pw[RECS_T_PEOPLE]; char pname[RECS_T_PEOPLE][40]; char pdir[RECS_T_PEOPLE]; int np;
                                                  /* a person is one entry per (id, director or not) */
} RecTaste;
/* shares by genre / language / kind / keyword / person, all weighted by the seeds' w (seeds with w <= 0 are
   ignored). Cast get w * (1 - 0.15 * billing position), directors w. */
void   recs_taste_build(const RecSeedX *s, int ns, RecTaste *t);
double recs_taste_lang(const RecTaste *t, const char *lang);   /* share of the weighted history in this language, 0..1 */
double recs_taste_genre(const RecTaste *t, int genre);         /* this genre's weight relative to the top genre, 0..1 */
double recs_taste_kind(const RecTaste *t, char kind);          /* share of the history that is 'm' or 't', 0..1 */
/* per-title taste score (higher is better; about -0.2 .. 1): 0.45 genre fit + 0.3 language share +
   0.15 quality damped under 200 votes + 0.1 freshness (0 at 2 years) - 0.02 per year past 12 (max 10 years) */
double recs_title_score(const RecTaste *t, const RecCand *c, long today);

/* ---- C. which languages get rows (PLAN section 5) ---- */
typedef struct { char code[4]; int strong; float pick; } RecLang;  /* "hi"; strong = full set of row kinds, else the weak set;
                                                                    pick = the user chose it: 1 main, 0.6 second, 0 not chosen */
/* explicitCsv: Settings > For You > Main and Second language ("hi,en", main first) or ""/NULL. regionCsv: the region's
   languages ("hi,en"). audio: the default audio language as 2 letters ("en") or ""/NULL. Rules:
     - explicit list non-empty: exactly those, in that order, all strong, at most 3; pick 1 for the first, 0.6 after
     - else strong = languages with >= 25% of the history (largest share first), then the audio language;
       weak = the region's languages not already in; the order is strong..., then weak...; at most 3.
   Weak languages get fewer row kinds: no "may have missed", no decade rows, fewer genre rows.
   Returns the count (0..3) written to out. */
int recs_langs_allowed(const char *explicitCsv, const char *regionCsv, const char *audio, const RecTaste *t, RecLang out[RECS_MAX_LANGS]);
/* a second language chosen while the main one is Automatic: added (or raised) with pick 0.6 and strong; when the list is
   full it takes the place of the last language nobody chose. Returns the new count. */
int recs_langs_add_pick(RecLang out[RECS_MAX_LANGS], int n, const char *code, float pick);
/* the languages the engine can name (the Settings lists): count, and code i ("hi") */
int recs_lang_count(void);
const char *recs_lang_code(int i);

/* ---- D. candidate rows ---- */
typedef enum {
  RF_LANG_GENRE, RF_ACCLAIM, RF_GEMS, RF_NEW, RF_DECADE, RF_PERSON, RF_MOOD, RF_SEASON, RF_PROVIDER,
  RF_CHART, RF_TREND, RF_BYW, RF_PERSONAL, RF_FAMILIES
} RecFamily;
#define RECS_F_RANK 1   /* Top 10 numerals: keep TMDB order, exempt from the "first six cards" rule (still counted) */
#define RECS_F_KEEP 2   /* keep the order the caller gives (Because you watched, Top picks): no re-ranking by taste */
typedef struct {
  unsigned char family; char kind;      /* RecFamily; 'm', 't' or 0 (mixed: the answer has media_type per entry) */
  unsigned char flags;                  /* RECS_F_* */
  char lang[4];                         /* the language a language row is for, else "" */
  float affinity;                       /* how well the row fits this person, 0..~2 (page builder's greedy input) */
  long maxAge;                          /* seconds the cached answer stays good */
  int seed;                             /* RF_BYW: index into RecRowCtx.seeds, else -1 */
  char title[96];                       /* already translated */
  char key[96];                         /* cache key; includes region / languages where the request depends on them */
  char path[400];                       /* what follows https://api.themoviedb.org/3 ; "" = no request (RF_PERSONAL) */
} RecRowDef;
typedef struct {
  const char *region;                   /* "IE" (watch_region and the provider table) */
  long today;                           /* days since 1970; the date filters and the month for seasonal rows come from it */
  const RecLang *langs; int nlangs;     /* from recs_langs_allowed */
  const RecTaste *taste;                /* from recs_taste_build */
  const RecSeedX *seeds; int nseeds;    /* for the Because-you-watched rows (needs recs/sim filled) */
  int lowMem;                           /* 1: fewer rows per family, at most 3 Because-you-watched, 12 mood rows */
  const char *(*tr)(const char *);      /* translate callback or NULL */
} RecRowCtx;
/* Fills out with the candidate rows, best families first: RF_PERSONAL, RF_BYW (up to 5 most recent seeds with
   >= 8 recs+similar), RF_CHART x2, RF_TREND, then lang-genre / acclaim / gems / new / decade, person (at most
   4 cast + 2 directors), mood (the best-fitting 26, or 12 low-memory), season, provider. Returns the count
   (never more than max; 110 is enough for everything). Animation / family / kids are excluded from every
   language and keyword row unless the taste includes animation (>= 15% of the history). */
int recs_rows_build(const RecRowCtx *cx, RecRowDef *out, int max);
/* the hand-written mood table, for tests and tools: count, and row i built for this context (0 if not buildable).
   Every keyword-based mood row has a with_genres gate. */
int recs_mood_count(void);
int recs_mood_row(const RecRowCtx *cx, int i, RecRowDef *out);
/* English name for a language code ("hi" -> "Hindi"), NULL if the table does not know it */
const char *recs_lang_name(const char *code);

/* ---- E. Because you watched ---- */
/* Ranks one Because-you-watched row: that seed's recommendations (weight 1.0, by rank), similar (0.5) and own
   (the def's fetched list, 0.7), plus 0.9 for the seed's language, 0.5 x genre overlap, 0.1 x quality. Titles
   excluded by p (watched / dismissed / adult / unreleased) and the seed itself are left out. Sorted best first;
   the page builder keeps this order (RF_BYW rows carry RECS_F_KEEP). Returns the count (at most max). */
int recs_byw_rank(const RecSeedX *seed, const RecPrefs *p, const RecCand *own, int nown, RecCand *out, int max);

/* ---- F. Top picks ---- */
/* The merged lists over all seeds (recs_rank: recommendations 1.0, and the seed's own-language list 0.6 where the
   seed has one, else its similar list) with a soft language balance: at every place p inside the first `window`
   a language may hold at most floor(share * (p + 1)) + 1 places (share = its share of the history; all languages
   under 5% share one bucket worth 10%), so every prefix is balanced, not only the total. If no title of an
   allowed language is left the slot goes to the next best anyway. After the window the order is the plain
   ranking. The caller uses window = 28 (8 billboard + 20 row) and splits the result. */
int recs_top_picks(const RecSeedX *s, int ns, const RecTaste *t, const RecPrefs *p, int window, RecCand *out, int max);

/* ---- G. page builder ---- */
typedef struct { const RecRowDef *def; const RecCand *cards; int n; } RecRowIn;   /* a candidate row and what was fetched for it */
typedef struct { short in; short n; unsigned char idx[RECS_PAGE_CARDS]; } RecPageRow; /* in = index into the input rows, idx = card indices in display order */
typedef struct { RecPageRow rows[RECS_PAGE_MAX]; int n; } RecPage;
typedef struct {
  const RecTaste *taste; long today;
  int maxRows;                           /* 36 normally, 20 on low-memory devices (clamped to RECS_PAGE_MAX) */
  const int *bill; int nbill;            /* recs_key of the billboard's titles: counted as shown once, first-six */
  const int *exclude; int nexclude;      /* recs_key of watched / dismissed titles: never shown */
  int allowAdult; int minMovieAge;       /* as RecPrefs */
  const RecLang *langs; int nlangs;      /* optional: the languages with pick > 0 lift their cards and keep their rows spread down the page */
} RecPageCtx;
/* Chooses and orders the rows. Rules:
   - cards that are excluded, adult (unless allowed), unreleased or in cinemas too recently are dropped (not
     for RF_CHART rows' release check); a row keeps its order if it has RECS_F_KEEP or RECS_F_RANK, else cards
     are ranked by recs_title_score
   - a title appears at most twice on the page (the billboard counts once) and, except in RANK rows, never in
     the first six cards of two rows; a title that is in an earlier row's first six moves behind the row's
     other cards; a row needs >= 8 cards and 6 clean first cards or it is dropped
   - slots (row index): 0 Top picks (RF_PERSONAL); a Because-you-watched row at 1 and every 4th after
     (3, 7, 11 is taken by trending, 15, 19 ...); charts at 2 and 5; RF_TREND at 11; any other slot goes to
     the greedy choice: affinity - 0.25 x (same family among the last 4 rows) - 0.8 x (share of its best six
     cards already in a first six) + 0.05 for series; but when a chosen language (pick > 0) has no row among the
     last 3 (main) or 5 (second) rows, the slot goes to its best unused row instead
   - cards of a chosen language score + 0.15 x pick when a row is ranked (a lift, not a filter)
   - at most maxRows rows; fewer if the candidates run out.
   Returns the number of rows. Cost: O(rows x cards), no per-card allocation. */
int recs_page_build(const RecPageCtx *cx, const RecRowIn *in, int nin, RecPage *out);
#endif
