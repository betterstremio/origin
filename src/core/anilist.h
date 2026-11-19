/* AniList (graphql.anilist.co, no key): anime rows, per-title recommendations, id mapping and taste data for the Anime
   tab. Everything is asked from the main thread and answered on it (callbacks from net_pump); answers are cached on
   disk (12 h lists, 7 days recommendations and taste data) and a cached answer is read and parsed on the network
   workers, never on the drawing thread. At most 2 requests are out at once and under 25 a minute (AniList allows 30);
   asks beyond 16 waiting drop the oldest (its callback gets a failure). */
#ifndef SB_ANILIST_H
#define SB_ANILIST_H
#include "core/stremio.h"

enum {
  AL_ROW_TRENDING,
  AL_ROW_THIS_SEASON, /* the season of today's date */
  AL_ROW_NEXT_SEASON, /* announced, not released yet */
  AL_ROW_POPULAR,
  AL_ROW_TOP_RATED,
  AL_ROW_GENRE, /* arg: the genre ("Action") */
  AL_ROW_MOVIES,
  AL_ROW_STUDIO, /* arg: the studio's name */
  AL_ROW_RECS_FOR, /* arg: an AniList id as text ("21") */
  AL_ROW_FINISHED_BINGE, /* finished, 24+ episodes, best scored */
  AL_ROW_COUNT
};

/* list: malloc'd for the callback's owner (itemlist_free); on failure still a list, with n 0 and total -1.
   cached: 1 when the answer came from the disk. Items: id "anilist:<id>", type series / movie, metaLine "year,
   episodes, genre, studio" (anilist_meta_line). */
typedef void (*AlCb)(ItemList *list, int cached, void *ud);
/* returns an id (never 0 for a known kind) */
unsigned anilist_row(int kind, const char *arg, AlCb cb, void *ud);

/* a title's AniList id from "anilist:N" (at once, inside the call), "mal:N" or "kitsu:N[:ep]"; 0 when it is unknown.
   Pairs are kept on disk for good (alids.txt in the data folder, read on the first use). */
typedef void (*AlIdCb)(int anilistId, void *ud);
void anilist_resolve(const char *id, AlIdCb cb, void *ud);

/* taste data of one title (cached 7 days): info is NULL when the answer failed */
typedef struct {
  int id;
  char format[16]; /* TV, MOVIE, OVA ... */
  char genres[8][24];
  int ngenres;
  char tags[12][32]; /* those ranked 60 or more */
  int ntags;
  char studio[48]; /* the main studio, "" when none */
} AlInfo;
typedef void (*AlInfoCb)(const AlInfo *info, int cached, void *ud);
unsigned anilist_media_info(int id, AlInfoCb cb, void *ud);

/* what a list answer said about a title (kept for the newest 400): ranking and the details line */
typedef struct {
  int id, year, episodes, aired, popularity, score, movie; /* aired: episodes out so far; score 0-100 */
  char status[16]; /* RELEASING, FINISHED, NOT_YET_RELEASED ... */
  char genres[3][24];
  int ng;
  char studio[40];
  char yt[16]; /* the YouTube trailer id, when AniList supplied one */
} AlFacts;
int anilist_facts(int id, AlFacts *out); /* 1 when known */
/* the Anime tab's details line from facts (imdb: "8.9" or NULL): parts separated by 0x1f, as widgets.c draws them */
void anilist_meta_line(const AlFacts *f, const char *imdb, char *out, size_t cap);
int anilist_request_count(void); /* requests sent to AniList since the start (a development counter) */

void anilist_tick(void); /* once a frame: paces the queue, runs the SB_ANILIST_TEST hook; never blocks */
#endif
