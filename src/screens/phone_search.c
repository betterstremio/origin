/* Search on a phone: a field at the top typed on the system's keyboard (it opens when the page does, and when the
   field is tapped; scrolling the results or Enter puts it away), the names IMDb suggests as chips, then the results
   in a grid of posters. The search itself is the TV's (search.c: IMDb first, then every catalog that searches,
   remembered for Backspace). A tap opens a title, a long press its card. */
#include "screens/phone.h"
#include "screens/search.h"
#include "core/form.h"
#include "core/i18n.h"
#include "core/hit.h"
#include "core/keys.h"
#include "core/stremio.h"
#include "ui/gfx.h"
#include "ui/text.h"
#include "ui/icon.h"
#include "ui/widgets.h"
#include <SDL2/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static struct {
  Scroller page, chips;
  int typing;
  float lastOff;
} Q;
static void kb(int on) {
  if (on && !SDL_IsTextInputActive()) {
    SDL_Rect r = { 0, 0, 10, 10 };
    SDL_SetTextInputRect(&r);
    SDL_StartTextInput();
  } else if (!on && SDL_IsTextInputActive())
    SDL_StopTextInput();
  Q.typing = on;
}
static void field_act(int a, int b) {
  (void)a;
  (void)b;
  kb(1);
}
static void clear_act(int a, int b) {
  (void)a;
  (void)b;
  search_set_query("");
  kb(1);
  scroller_set(&Q.page, 0);
}
static void sug_act(int i, int b) {
  (void)b;
  const char *s[8];
  int n = search_sug_list(s, 8);
  if (i < n) {
    search_set_query(s[i]);
    search_now();
    kb(0);
    scroller_set(&Q.page, 0);
  }
}
static void res_tap(int i, int b) {
  (void)b;
  const ItemList *l = search_results();
  if (i < l->n) {
    kb(0);
    app_open_item(&l->items[i]);
  }
}
static void res_hold(int i, int b) {
  (void)b;
  const ItemList *l = search_results();
  if (i >= l->n)
    return;
  kb(0);
  float x, y, w, h;
  touch_hold_rect(&x, &y, &w, &h);
  phone_peek_open(&l->items[i], x, y, w, h);
}

static void enter(const void *p) {
  searchScreen.enter(p);
  scroller_set(&Q.page, 0);
  if (!search_query()[0])
    kb(1);
}
static void leave(void) {
  kb(0);
  if (searchScreen.leave)
    searchScreen.leave();
}
static void text(const char *s) {
  searchScreen.text(s);
  scroller_set(&Q.page, 0);
}
static int key(int k) {
  if (k == K_OK) {
    search_now();
    kb(0);
    return 1;
  } /* the keyboard's Enter */
  if (k == K_BACK && Q.typing) {
    kb(0);
    return 1;
  }
  return 0;
}
static void tick(float dt) {
  searchScreen.tick(dt);
  scroller_tick(&Q.page, dt);
  scroller_tick(&Q.chips, dt);
  if (Q.typing && fabsf(Q.page.off - Q.lastOff) > 24)
    kb(0); /* scrolling the results: the keyboard goes */
  if (!Q.typing)
    Q.lastOff = Q.page.off;
}
static void draw(void) {
  float bottom = phone_bottom();
  gfx_rect(0, 0, SB_W, SB_H, PAGE);
  const char *q = search_query();
  const ItemList *l = search_results();
  /* the field */
  float fy = 10, fh = 46, fx = PH_M, fw = SB_W - 2 * PH_M;
  gfx_rrect(fx, fy, fw, fh, 12, Q.typing ? WHITE_A(34) : WHITE_A(22));
  if (Q.typing)
    gfx_ring(fx, fy, fw, fh, 12, 1.5f, WHITE_A(90));
  icon_draw("ph_search", fx + 12, fy + fh / 2 - 11, 22, PH_TXT2);
  float tx = fx + 44, ty = fy + fh / 2 - text_line_height(PH_TX_BODY + 2) / 2.0f;
  if (*q) {
    float w = text_draw(q, tx, ty, PH_TX_BODY + 2, 500, PH_TXT, 0, fw - 44 - 44, ALIGN_LEFT);
    if (Q.typing && fmod(app_now(), 1.0) < 0.6)
      gfx_rect(tx + w + 2, fy + 12, 2, fh - 24, ACC | 0xff000000u);
    int pr = touch_pressed(NULL, clear_act, 0, 0);
    gfx_circle(fx + fw - 22, fy + fh / 2, 12, pr ? WHITE_A(90) : WHITE_A(46));
    icon_draw("ph_close", fx + fw - 30, fy + fh / 2 - 8, 16, PAGE | 0xff000000u);
    hit_act(fx + fw - 44, fy, 44, fh, clear_act, 0, 0);
  } else {
    text_draw(T("Films, series, people"), tx, ty, PH_TX_BODY + 2, 500, PH_TXT2, 0, fw - 60, ALIGN_LEFT);
    if (Q.typing && fmod(app_now(), 1.0) < 0.6)
      gfx_rect(tx, fy + 12, 2, fh - 24, ACC | 0xff000000u);
  }
  hit_act(fx, fy, fw - (*q ? 44 : 0), fh, field_act, 0, 0);
  float y = fy + fh + 12;
  /* the suggested names */
  const char *sg[8];
  int ns = *q ? search_sug_list(sg, 8) : 0;
  if (ns) {
    float cx = PH_M - Q.chips.off, c0 = cx;
    touch_region(0, y, SB_W, 36, &Q.chips, NULL);
    gfx_clip_push(0, y - 2, SB_W, 40);
    for (int i = 0; i < ns; i++) {
      float w = text_width(sg[i], PH_TX_SMALL + 1, 600, 0) + 26;
      if (w > 220)
        w = 220;
      int pr = touch_pressed(NULL, sug_act, i, 0);
      gfx_rrect(cx, y, w, 32, 16, pr ? WHITE_A(60) : WHITE_A(20));
      text_draw(sg[i], cx + w / 2, y + 16 - text_line_height(PH_TX_SMALL + 1) / 2.0f, PH_TX_SMALL + 1, 600, PH_TXT, 0,
                w - 16, ALIGN_CENTER);
      hit_act(cx, y, w, 32, sug_act, i, 0);
      cx += w + 8;
    }
    gfx_clip_pop();
    scroller_limits(&Q.chips, 0, cx - c0 - 8 + 2 * PH_M - SB_W);
    y += 44;
  }
  /* the results */
  float top = y;
  touch_region(0, top, SB_W, bottom - top, NULL, &Q.page);
  gfx_clip_push(0, top, SB_W, bottom - top);
  int cols = (int)((SB_W - 2 * PH_M + 8) / 126);
  if (cols < 3)
    cols = 3;
  float gap = 8, cw = (SB_W - 2 * PH_M - (cols - 1) * gap) / cols, ch = cw * 1.5f, rowH = ch + 34;
  float gy = top + 4 - Q.page.off;
  for (int i = 0; i < l->n; i++) {
    int r = i / cols, c = i % cols;
    float x = PH_M + c * (cw + gap), yy = gy + r * rowH;
    if (yy + rowH < top || yy > bottom)
      continue;
    int pr = touch_pressed(NULL, res_tap, i, 0);
    phone_press_scale(pr, x, yy, cw, ch);
    phone_poster(&l->items[i], x, yy, cw, ch);
    gfx_scale_pop();
    text_draw(l->items[i].name, x, yy + ch + 6, PH_TX_SMALL, 600, PH_TXT2, 0, cw, ALIGN_LEFT);
    hit_add_hold(x, yy, cw, ch + 24, NULL, res_tap, res_hold, i, 0);
  }
  float end = gy + ((l->n + cols - 1) / cols) * rowH;
  if (!l->n) {
    const char *m = !*q ? T("Search your addons and IMDb") : search_busy() ? T("Searching…") : T("Nothing found.");
    text_draw(m, SB_W / 2, top + 40, PH_TX_BODY, 600, PH_TXT2, 0, SB_W - 32, ALIGN_CENTER);
  } else if (search_busy())
    text_draw(T("Searching…"), SB_W / 2, end + 8, PH_TX_SMALL, 600, PH_TXT2, 0, 0, ALIGN_CENTER);
  gfx_clip_pop();
  scroller_limits(&Q.page, 0, end + Q.page.off - top + 30 - (bottom - top));
}
Screen phoneSearch = { enter, leave, key, tick, draw, NULL, NULL, text, NULL };
