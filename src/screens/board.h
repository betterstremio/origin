/* Home / Discover / Library: one engine (a hero above rows of cards) with three data sources. */
#ifndef SB_BOARD_H
#define SB_BOARD_H
#include "core/app.h"
#include "core/collections.h"
extern Screen homeScreen, discScreen, libScreen, colScreen;
int  board_home_ready(void);
void tabs_warm_tick(void);    /* after start: the other tabs' first screens made ready in the background */    /* the first rows' posters are in, the hero is set, the backdrop is up */
void board_invalidate_all(void); /* another account: Discover too */
void board_invalidate(void);    /* the library changed: Home and Library rebuild when shown */
void board_rebuild(void);       /* the board on screen reloads its rows in place, focus kept */
int  board_spot_up(void);
float board_open_wait(void);       /* how much longer than its quickest the focused cinematic card waits to open */
int  board_on_billboard(void);       /* the focus is on Home's cinematic billboard */
int  board_near_items(const Item **out, int max); /* the cards around the focus: same row, the rows above and below */
const Item *board_next_item(void);   /* the card right of the focus on Home / Discover, NULL when none */
void board_col_tick(void);      /* an export the phone sent: taken in, Home rebuilt */
unsigned board_col_gen(void);   /* how many times the collections have changed */
int  board_sample(Item *out, int max, char *rowTitle, int cap); /* titles from Home's first poster rows (Discover's when Home has none) and the first one's title, for the guide's pictures */       /* Discover's spotlight has the focus (it moves on by itself: no background trailer) */
/* phones (screens/phone_home.c): kind 0 Home, 1 Discover, 2 a collection tile's page */
typedef struct { const char *title; const Item *items; int n, wide, rank, hidden, label, seeAll, loading; float closing; const ColFolder *tiles; int ntiles; } BoardRow;
void board_ph_enter(int kind);
void board_ph_tick(int kind, int firstRow, int lastRow, float dt); /* the rows on screen: their cards loaded */
unsigned board_ph_gen(int kind);    /* changes when the rows are made again */
int  board_ph_nrows(int kind);
int  board_ph_row(int kind, int i, BoardRow *out);
int  board_ph_feat(int kind, const Item **out, int max); /* the hero's picks (Home: the billboard's; Discover: the spotlight's) */
void board_ph_near_end(int kind, int row, int col);
void board_ph_open(int kind, int row, int col);
#endif
