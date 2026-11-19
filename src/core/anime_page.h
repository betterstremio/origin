/* The Anime tab's page, as data: which rows it has (a plan made from the taste and what AniList answered), how a row
   is asked, the billboard's titles, and the Kitsu addon's meta (fanart background, logo, IMDb rating) kept per title.
   The board (screens/board.c) owns the rows and the drawing; this file owns what goes in them. Nothing here runs until
   the tab is opened (anime_page_open); with the tab off no request is ever made. */
#ifndef SB_ANIME_PAGE_H
#define SB_ANIME_PAGE_H
#include "core/stremio.h"
#include "core/anilist.h"

enum { AS_TOP, AS_AL, AS_LIST, AS_KITSU }; /* where a row's cards come from */
typedef struct {
  char title[96];
  int src; /* AS_* */
  int al; /* AS_AL: the AL_ROW_* kind */
  char arg[56]; /* AS_AL: the genre / studio / AniList id; AS_KITSU: the catalogue id */
} AnimeRow;
enum { AM_FORYOU, AM_POPULAR, AM_KITSU }; /* what the page is made of now */

void anime_page_open(void); /* the tab was entered: taste starts (when For You is on), the Kitsu addon is made ready */
/* the rows after Continue Watching, at most max of them; returns how many. mode: AM_* */
int anime_page_plan(AnimeRow *out, int max, int *mode);
unsigned anime_page_sig(void); /* a hash of the plan now: the board builds the rows again only when it moves */
/* the cards of a row, asked now (AniList: cached 12 h on the disk, paced); cb comes with the list (n 0 when none) */
void anime_page_ask(const AnimeRow *r, AlCb cb, void *ud);
/* a Kitsu catalogue row's definition (the fallback when AniList is down); 0 when the addon is not in */
int anime_page_def(const AnimeRow *r, CatalogDef *d);
void anime_page_close(void); /* the tab went off: what the page keeps is let go */
int anime_page_billboard(ItemList *out); /* top picks (or trending without a taste): up to 8; 0 = not yet */
int anime_page_mylist(ItemList *out); /* the anime of the library (My List), newest first */
/* the AniList cards of a list made IMDb cards where the map knows the id (see anime_map.h), seasons of one show folded
   into one card, the user's own native-id history kept as it is. In place, never waits. Returns how many cards
   changed or went (0: nothing) */
int anime_page_map_list(ItemList *l);

/* the Kitsu meta of a title: the fanart background and logo, the IMDb rating; asked once, kept 7 days */
typedef struct {
  char bg[256], logo[256], imdb[12];
} AnimeMeta;
void anime_meta_ask(const char *type, const char *id); /* no-op when known, asked, or the addon is not ready */
int anime_meta_get(const char *id, AnimeMeta *m); /* 1 when it has been answered (the fields may be empty) */
unsigned anime_meta_version(void); /* bumps when an answer comes */
/* put what is known about a title into its card: the fanart (else AniList's banner stays), logo and details line */
int anime_meta_apply(Item *it);
#endif
