/* "For You": recommendations from the watch history, with the user's own TMDB key (recs_core.c does the maths).
   recs_tick() and every getter are for the main thread and never block on the network or the disk: ONE worker thread owned
   by recs.c does every file read, JSON parse, ranking, page build and file write; the main thread only decides when,
   sends the TMDB requests the worker asks for (3 at a time, started well after launch so Home's own rows go first) and swaps
   in the page the worker finished. TMDB answers are cached on disk (lists 12-24 h, seed details 7 days), the IMDb ids in
   their own file (recsids.bin), and the finished page too (recspage.bin: it appears at once on the next launch, read by the
   worker, nothing is parsed on the main thread). What stays in memory is one compact page (about 250 KB for 36 rows).
   Without a TMDB key, or with Settings > Recommendations off, everything here is empty and nothing is asked. */
#ifndef SB_RECS_H
#define SB_RECS_H
#include "core/stremio.h"

void     recs_tick(void);
unsigned recs_version(void);              /* bumps when the lists below change: Home rebuilds */
/* copies (free with itemlist_clear); 0 when there is nothing */
int  recs_billboard(ItemList *out);       /* Top picks: the rotating billboard, 8 at most */
/* the "For You" rows below the billboard (up to 36 of up to 20 cards, 20 rows on a small device): Home keeps only the few near the focus loaded */
int  recs_rows(void);                     /* how many rows the page has now (changes only with recs_version()) */
int  recs_row_info(int i, char *title, size_t cap, int *rank, int *count); /* 1 if row i exists: its title, a Top 10 row (numerals), its card count */
int  recs_row_items(int i, ItemList *out); /* row i's cards inflated into Items (free with itemlist_clear); the count. Only the cards whose IMDb id is known now: a row grows by itself the next time it is asked for */
int  recs_rank_of(const Item *it);        /* 1-10 in a Top 10 row (for its numeral), else 0 */
int  recs_badges(const Item *it);         /* RECS_NEW | RECS_LOVE: the wide card's badges */
enum { RECS_NEW = 1, RECS_LOVE = 2 };
void recs_dismiss(const Item *it);        /* "Not interested": hidden and kept out from now on */

/* settings (kept on this device: the Stremio account has nowhere for them) */
int  recs_on(void);
void recs_set_on(int on);
int  recs_paused(void);                   /* "Pause history": what is watched from now on is not learnt from */
void recs_set_paused(int on);
int  recs_hide_cinema(void);             /* "Hide films still in cinemas": films out less than 30 days ago are left out (off by default:
                                            many go straight to streaming) */
void recs_set_hide_cinema(int on);
const char *recs_region(void);            /* "IN": the TV's country unless chosen */
void recs_set_region(const char *cc);
const char *recs_langs(void);             /* "hi,en": Main and Second language as chosen, else the region's usual ones */
void recs_set_langs(const char *mainCode, const char *secondCode);   /* "" main = Automatic, "" second = None */
void recs_langs_migrate(void);            /* the old "Languages" choice into Main + Second (once; recs_tick calls it) */
const char *recs_region_name(const char *cc);   /* "India" */
void recs_status(char *out, size_t cap);  /* Settings' line: on/off, why not, how many picks, how fresh */
int  recs_region_count(void);
const char *recs_region_code(int i);
void recs_account_changed(void);          /* another account or profile: its own saved page, nothing asked for yet */
void recs_refresh(void);                  /* ask again now (a setting changed) */
#endif
