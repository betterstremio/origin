/* Catgrid: a paged grid of one addon catalog (a row's See All). The first page comes the same way as the others
   (skip=0, not sb_catalog): that one keeps 16 items of what the addon sent and its skip passes over the rest. */
#include "screens/catgrid.h"
#include "core/keys.h"
#include "core/i18n.h"
#include "core/json.h"
#include "ui/gfx.h"
#include "ui/text.h"
#include "ui/widgets.h"
#include "ui/img.h"
#include "screens/sheet.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "core/hit.h"
#include "core/net.h"

#define COLS 6
static struct {
  CatParams p;
  ItemList items;
  int idx, loading, empty, paging, more, skipAt;
  double loadT0, pageAt;
  struct {
    float x, y;
    int shown;
    Timer1 pop;
  } ring;
  Tween gridY;
  Timer1 fade;
  Nudge nudge;
  unsigned gen;
} G;
#define MAXITEMS 1000
static void on_page(ItemList *l, int more, void *ud) {
  if ((unsigned)(uintptr_t)ud != G.gen) {
    itemlist_free(l);
    return;
  }
  G.paging = 0;
  if (!l) {
    if (G.items.n == 0) {
      G.loading = 0;
      G.empty = 1;
      G.more = 0;
    } else
      G.pageAt = app_now() + 10;
    return;
  }
  int wasLoading = G.loading;
  Item *a = realloc(G.items.items, (G.items.n + l->n + 1) * sizeof(Item));
  if (!a) {
    itemlist_free(l);
    G.more = 0;
    G.loading = 0;
    G.empty = G.items.n == 0;
    return;
  }
  G.items.items = a;
  int add = 0;
  for (int i = 0; i < l->n && G.items.n < MAXITEMS; i++) {
    const Item *it = &l->items[i];
    if (!it->id[0])
      continue;
    int dup = 0;
    for (int j = 0; j < G.items.n; j++)
      if (!strcmp(G.items.items[j].id, it->id)) {
        dup = 1;
        break;
      }
    if (dup)
      continue;
    G.items.items[G.items.n++] = *it;
    add++;
  }
  (void)more;
  G.skipAt += l->total;
  if (!l->total || !add || G.items.n >= MAXITEMS)
    G.more = 0; /* the end: nothing new. hasMore is not believed: Cinemeta says false on any page it filtered to 49,
                   then has hundreds more */
  if (getenv("SB_SLOWLOG"))
    fprintf(stderr, "[cat] grid page of %s: %d new, %d on the grid%s\n", G.p.title, add, G.items.n,
            G.more ? "" : " (the end)");
  if (wasLoading) {
    G.loading = 0;
    G.empty = G.items.n == 0;
    timer_start(&G.fade, 0.3f);
  }
  itemlist_free(l);
}
static void page_ask(void) {
  G.paging = 1;
  sb_catalog_page(&G.p.def, G.skipAt, on_page, (void *)(uintptr_t)G.gen);
}
static void load(void) {
  G.gen++;
  itemlist_clear(&G.items);
  G.idx = 0;
  G.skipAt = 0;
  G.more = 1;
  G.paging = 0;
  G.pageAt = 0;
  G.loading = 1;
  G.empty = 0;
  G.loadT0 = app_now();
  G.ring.shown = 0;
  tween_set(&G.gridY, 0);
  page_ask();
}
static void enter(const void *pp) {
  if (pp) {
    G.p = *(const CatParams *)pp;
    load();
    return;
  }
  if (!G.items.n && !G.loading && G.p.def.path[0])
    load(); /* back from a title's page: everything stays, the focus too */
}
static const Item *focused(void) {
  return G.idx < G.items.n ? &G.items.items[G.idx] : NULL;
}
static void open_it(const Item *it) {
  DetailParams p;
  memset(&p, 0, sizeof p);
  p.it = *it;
  app_go(SCR_DETAIL, &p, 0);
}
static int key(int k) {
  if (k == K_BACK) {
    G.gen++;
    itemlist_clear(&G.items);
    G.loading = 0;
    G.paging = 0;
    return 0;
  }
  if (!G.items.n) {
    if (k == K_UP)
      topnav_focus();
    return 1;
  }
  int i0 = G.idx;
  if (k == K_RIGHT)
    G.idx = G.idx + 1 < G.items.n ? G.idx + 1 : G.idx;
  else if (k == K_LEFT) {
    if (G.idx > 0)
      G.idx--;
  } else if (k == K_DOWN)
    G.idx = G.idx + COLS < G.items.n
                ? G.idx + COLS
                : (G.items.n - 1 > G.idx && G.idx / COLS < (G.items.n - 1) / COLS ? G.items.n - 1 : G.idx);
  else if (k == K_UP) {
    if (G.idx - COLS >= 0)
      G.idx -= COLS;
    else {
      topnav_focus();
      return 1;
    }
  } else if (k == K_OK) {
    open_it(&G.items.items[G.idx]);
    return 1;
  }
  if (G.idx == i0)
    nudge_start(&G.nudge, k);
  return 1;
}
#define ROWH (CARD_H + 26.0f)
/* the text art of the titles around the focus, for their pages (drawn there at 520x175): ready when one opens */
static void logos_ahead(void) {
  static double at0;
  double now = app_now();
  if (now - at0 < 0.5)
    return;
  at0 = now;
  for (int i = G.idx - COLS; i <= G.idx + COLS; i++) {
    if (i < 0 || i >= G.items.n)
      continue;
    char lu[512];
    sb_logo_of(&G.items.items[i], lu, sizeof lu);
    if (lu[0])
      img_prefetch_pri(lu, 520, 175);
  }
}
static void tick(float dt) {
  logos_ahead();
  if (G.more && !G.paging && !G.loading && G.items.n - G.idx <= COLS * 3 && app_now() >= G.pageAt)
    page_ask();
  timer_tick(&G.ring.pop, dt);
  tween_tick(&G.gridY, dt);
  timer_tick(&G.fade, dt);
  nudge_tick(&G.nudge, dt);
  int row = G.idx / COLS;
  float y = row > 1 ? (row - 1) * ROWH : 0;
  if (!app_scroll_hold())
    tween_to(&G.gridY, -y, 0.3f, EASE_OUT); /* (a desktop's hover: the grid stays put) */
  /* the ring jumps to the card and lands with a small pop (never glides across the others) */
  if (G.items.n) {
    int rtl = app_rtl();
    float cx = 48 + 14 + (G.idx % COLS) * (CARD_W + CARD_GAP), cy = 16 + row * ROWH;
    if (rtl)
      cx = SB_W - cx - CARD_W;
    if (!G.ring.shown || G.ring.x != cx || G.ring.y != cy) {
      static double last;
      int pop = !G.ring.shown || focus_pops(&last);
      G.ring.x = cx;
      G.ring.y = cy;
      G.ring.shown = 1;
      if (pop)
        timer_start(&G.ring.pop, 0.2f);
      else
        G.ring.pop.on = 0;
    }
  } /* held: it jumps, lit */
}
static int origin(float *x, float *y) {
  if (topNav.active || G.idx >= G.items.n)
    return 0;
  int row = G.idx / COLS;
  float cx = 48 + 14 + (G.idx % COLS) * (CARD_W + CARD_GAP);
  if (app_rtl())
    cx = SB_W - cx - CARD_W;
  *x = cx + CARD_W / 2;
  *y = 120 + 104 + G.gridY.v + 16 + row * ROWH + CARD_H / 2;
  return 1;
}
static void cat_focus(int i, int b) {
  (void)b;
  int was = topNav.active;
  topnav_leave();
  if (was || G.idx != i)
    G.idx = i;
}
static void draw(void) {
  int rtl = app_rtl();
  text_draw(G.p.title, rtl ? SB_W - 48 : 48, 120, 44, 800, WHITE_A(255), -0.8f, SB_W - 96,
            rtl ? ALIGN_RIGHT : ALIGN_LEFT);
  float top = 120 + 104;
  gfx_clip_push(34, top, SB_W - 34, SB_H - top);
  float fa = G.fade.on ? 1 - powf(1 - timer_x(&G.fade), 2) : 1;
  gfx_opacity_push(fa);
  gfx_offset_push(nudge_x(&G.nudge), nudge_y(&G.nudge) + top + G.gridY.v);
  if (G.loading) { /* loading: a thin line under the title, a short accent stroke sweeping along it */
    double t = app_now() - G.loadT0;
    float in = (float)((t - 0.15) / 0.3);
    in = in < 0 ? 0 : in > 1 ? 1 : in;
    if (in > 0) {
      float W = 360, x0 = rtl ? SB_W - 48 - 40 - W : 48 + 40, ph = (float)fmod(t, 1.4) / 1.4f,
            e = ph * ph * (3 - 2 * ph), sw = W * 0.32f, sx = x0 - sw + (W + sw) * e;
      gfx_opacity_push(in);
      gfx_rrect(x0, 52, W, 3, 1.5f, WHITE_A(22));
      float l = sx < x0 ? x0 : sx, r = sx + sw > x0 + W ? x0 + W : sx + sw;
      if (r - l > 1)
        gfx_rrect(l, 52, r - l, 3, 1.5f, ACC);
      gfx_opacity_pop();
    }
  } /* the stroke enters and leaves at the line's ends */
  if (G.empty && !G.loading)
    text_draw(T("Nothing found."), rtl ? SB_W - 48 - 40 : 48 + 40, 40, 27, 400, WHITE_A(191), 0, 0,
              rtl ? ALIGN_RIGHT : ALIGN_LEFT);
  /* the cards; the focused one last, over its neighbours as it grows */
  int fi = !topNav.active && G.idx < G.items.n ? G.idx : -1;
  float hp = app_hold_progress();
  int r0 = (int)((-G.gridY.v - 16) / ROWH) - 1;
  if (r0 < 0)
    r0 = 0;
  int r1 = (int)((-G.gridY.v + SB_H - top) / ROWH) + 1;
  int a = r0 * COLS, z = (r1 + 1) * COLS;
  if (z > G.items.n)
    z = G.items.n;
  for (int pass = 0; pass < 2; pass++)
    for (int i = a; i < z; i++) {
      if ((i == fi) != pass)
        continue;
      int row = i / COLS;
      float y = 16 + row * ROWH;
      if (y + G.gridY.v > SB_H - top || y + CARD_H + G.gridY.v < 0)
        continue;
      float x = 48 + 14 + (i % COLS) * (CARD_W + CARD_GAP);
      if (rtl)
        x = SB_W - x - CARD_W;
      unsigned long long lk = lift_key(&G, i);
      lift_begin(lk, pass, pass ? hp : 0, x, y, CARD_W, CARD_H, CARD_R);
      card_draw(&G.items.items[i], x, y, 0, 1, abs(row - G.idx / COLS) <= 2);
      if (pass && hp > 0)
        hold_trace_draw(x, y, CARD_W, CARD_H, CARD_R, ease_apply(EASE_IN_OUT, hp));
      lift_end(lk, x, y, CARD_W, CARD_H, CARD_R);
      hit_add(x, y, CARD_W, CARD_H, cat_focus, NULL, i, 0);
    }
  gfx_offset_pop();
  gfx_opacity_pop();
  gfx_clip_pop();
}
Screen catScreen = { enter, NULL, key, tick, draw, focused, open_it, NULL, origin };
