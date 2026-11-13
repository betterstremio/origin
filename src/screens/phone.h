/* The phone and tablet UI (core/form.h): its own screens, drawn in dp, on the same data, addons, sync and player as
   the TV's. A finger scrolls, taps and holds (core/touch.h); there is no focus to move. The TV screens stay as they
   are and are registered instead on a TV.
     phone_shell.c  the tab bar, Back, what the phone draws itself
     phone_home.c   Home and Discover: a hero, then every row, each a strip the finger flings
     phone_peek.c   the card a long press opens: bigger, its preview playing inside it
     phone_detail.c a title's page: Play, the story, seasons and episodes
     phone_streams.c the streams sheet */
#ifndef SB_PHONE_H
#define SB_PHONE_H
#include "core/app.h"
#include "core/touch.h"
#include <stdint.h>

/* the grid of the phone's pages, in dp */
#define PH_M 16.0f        /* the page's side margin */
#define PH_TOPBAR 56.0f   /* the top bar */
#define PH_TABBAR 60.0f   /* the tab bar at the bottom */
#define PH_R 8.0f         /* a card's corners */
/* text sizes */
#define PH_TX_H1 26
#define PH_TX_ROW 17
#define PH_TX_BODY 14
#define PH_TX_SMALL 12
/* colours */
#define PH_TXT RGB(0xf2, 0xf1, 0xf6)
#define PH_TXT2 RGBA(0xf2, 0xf1, 0xf6, 168)
#define PH_SURF RGB(0x1d, 0x1b, 0x26)

void  phone_register(void);         /* main.c: the phone's screens in place of the TV's (a phone or a tablet only) */
int   phone_owns(int scr);          /* that screen is the phone's own (else a TV page shown as it is, for now) */
void  phone_bars_draw(void);        /* the tab bar, over the page (app.c) */
float phone_bottom(void);           /* where the page ends: the tab bar's top */
int   phone_back(void);             /* Back on a phone (app.c): 1 when handled */
void  phone_go_tab(int scr);        /* a tab: the page stack starts again from it */

/* pieces every phone page draws */
int   phone_art(const char *url, float x, float y, float w, float h, float r); /* a picture (cover-cropped), the waiting look until it is in; 1 when in */
void  phone_poster(const Item *it, float x, float y, float w, float h);        /* a title's poster card */
void  phone_wide(const Item *it, float x, float y, float w, float h);          /* a Continue Watching card: its backdrop, name and progress */
float phone_button(const char *label, const char *icon, float x, float y, float w, float h, int primary, HitFn act, int a, int b); /* a pill button; its width (w 0: fits) */
void  phone_press_scale(int pressed, float x, float y, float w, float h);       /* push: a pressed card sinks a little (pop with gfx_scale_pop) */

/* the long-press card */
void  phone_peek_open(const Item *it, float x, float y, float w, float h); /* it grows out of that card */
int   phone_peek_on(void);
const Item *phone_peek_item(void);  /* the title whose preview the card plays (btrailer.c), NULL when shut */
float phone_peek_since(void);        /* seconds it has been open */
void  phone_peek_shut_now(void);     /* gone at once, no motion (the app sent to the back) */

/* the streams sheet */
void  phone_streams_show(const char *type, const char *id, const char *vid, const char *title, const char *label);
void  phone_streams_close(void);
int   phone_streams_on(void);

extern Screen phoneHome, phoneDisc, phoneCol, phoneDetail, phoneSearch, phoneLib;
#endif
