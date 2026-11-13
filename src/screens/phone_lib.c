/* The Library on a phone: chips for its categories (All, Movies, Series, In progress, Watched, Not watched, each with
   its count) and the order (a tap moves to the next one), then the titles as a grid of posters with their progress.
   A tap resumes or opens a title, a long press opens its card. The data is the TV Library's (lib.c). */
#include "screens/phone.h"
#include "screens/lib.h"
#include "core/form.h"
#include "core/i18n.h"
#include "core/hit.h"
#include "core/stremio.h"
#include "ui/gfx.h"
#include "ui/text.h"
#include "ui/icon.h"
#include "ui/widgets.h"
#include <stdio.h>
#include <string.h>

static struct {
  Scroller page, chips;
} B;
static void chip_act(int f, int b) {
  (void)b;
  lib_ph_set_filter(f);
  scroller_set(&B.page, 0);
}
static void sort_act(int a, int b) {
  (void)a;
  (void)b;
  lib_ph_next_sort();
  scroller_set(&B.page, 0);
}
static void tap(int i, int b) {
  (void)b;
  const Item *it = lib_ph_item(i);
  if (it)
    app_open_item(it);
}
static void hold(int i, int b) {
  (void)b;
  const Item *it = lib_ph_item(i);
  if (!it)
    return;
  float x, y, w, h;
  touch_hold_rect(&x, &y, &w, &h);
  phone_peek_open(it, x, y, w, h);
}
static void enter(const void *p) {
  (void)p;
  lib_ph_refresh();
}
static void tick(float dt) {
  lib_ph_refresh();
  scroller_tick(&B.page, dt);
  scroller_tick(&B.chips, dt);
}
static void draw(void) {
  float bottom = phone_bottom();
  gfx_rect(0, 0, SB_W, SB_H, PAGE);
  text_draw(T("Library"), PH_M, PH_TOPBAR / 2 - text_line_height(22) / 2.0f, 22, 800, PH_TXT, -0.4f, SB_W / 2,
            ALIGN_LEFT);
  {
    const char *sl = lib_ph_sort_label();
    float w = text_width(sl, PH_TX_SMALL + 1, 600, 0) + 40, x = SB_W - PH_M - w;
    int pr = touch_pressed(NULL, sort_act, 0, 0);
    gfx_rrect(x, PH_TOPBAR / 2 - 16, w, 32, 16, pr ? WHITE_A(60) : WHITE_A(20));
    icon_draw("lib_sort", x + 8, PH_TOPBAR / 2 - 9, 18, PH_TXT2);
    text_draw(sl, x + 30, PH_TOPBAR / 2 - text_line_height(PH_TX_SMALL + 1) / 2.0f, PH_TX_SMALL + 1, 600, PH_TXT, 0,
              w - 34, ALIGN_LEFT);
    hit_act(x, PH_TOPBAR / 2 - 22, w, 44, sort_act, 0, 0);
  }
  float y = PH_TOPBAR + 2;
  /* the categories */
  {
    const char *lb[8];
    int cnt[8], n = lib_ph_filters(lb, cnt, 8), cur = lib_ph_filter();
    float cx = PH_M - B.chips.off, c0 = cx;
    touch_region(0, y, SB_W, 36, &B.chips, NULL);
    gfx_clip_push(0, y - 2, SB_W, 40);
    for (int i = 0; i < n; i++) {
      char t[64];
      snprintf(t, sizeof t, "%s  %d", lb[i], cnt[i]);
      float w = text_width(t, PH_TX_SMALL + 1, 700, 0) + 26;
      int on = i == cur, pr = touch_pressed(NULL, chip_act, i, 0);
      gfx_rrect(cx, y, w, 32, 16, on ? WHITE_A(255) : pr ? WHITE_A(60) : WHITE_A(20));
      text_draw(t, cx + w / 2, y + 16 - text_line_height(PH_TX_SMALL + 1) / 2.0f, PH_TX_SMALL + 1, 700,
                on ? RGB(12, 11, 17) : PH_TXT, 0, 0, ALIGN_CENTER);
      hit_act(cx, y, w, 32, chip_act, i, 0);
      cx += w + 8;
    }
    gfx_clip_pop();
    scroller_limits(&B.chips, 0, cx - c0 - 8 + 2 * PH_M - SB_W);
    y += 46;
  }
  /* the titles */
  float top = y;
  touch_region(0, top, SB_W, bottom - top, NULL, &B.page);
  gfx_clip_push(0, top, SB_W, bottom - top);
  int cols = (int)((SB_W - 2 * PH_M + 8) / 126);
  if (cols < 3)
    cols = 3;
  float gap = 8, cw = (SB_W - 2 * PH_M - (cols - 1) * gap) / cols, ch = cw * 1.5f, rowH = ch + 34;
  int n = lib_ph_count();
  float gy = top + 2 - B.page.off;
  for (int i = 0; i < n; i++) {
    int r = i / cols, c = i % cols;
    float x = PH_M + c * (cw + gap), yy = gy + r * rowH;
    if (yy + rowH < top || yy > bottom)
      continue;
    const Item *it = lib_ph_item(i);
    if (!it)
      continue;
    int pr = touch_pressed(NULL, tap, i, 0);
    phone_press_scale(pr, x, yy, cw, ch);
    phone_poster(it, x, yy, cw, ch);
    CwInfo ci;
    if (lib_cw_info(it, &ci) && ci.pct > 0.01f) {
      gfx_rrect(x + 6, yy + ch - 8, cw - 12, 3, 1.5f, WHITE_A(80));
      gfx_rrect(x + 6, yy + ch - 8, (cw - 12) * (ci.pct > 1 ? 1 : ci.pct), 3, 1.5f, ACC | 0xff000000u);
    }
    gfx_scale_pop();
    text_draw(it->name, x, yy + ch + 6, PH_TX_SMALL, 600, PH_TXT2, 0, cw, ALIGN_LEFT);
    hit_add_hold(x, yy, cw, ch + 24, NULL, tap, hold, i, 0);
  }
  float end = gy + ((n + cols - 1) / cols) * rowH;
  if (!n)
    text_draw(T("Nothing here yet"), SB_W / 2, top + 40, PH_TX_BODY, 600, PH_TXT2, 0, SB_W - 32, ALIGN_CENTER);
  gfx_clip_pop();
  scroller_limits(&B.page, 0, end + B.page.off - top + 24 - (bottom - top));
}
static int key(int k) {
  (void)k;
  return 0;
}
Screen phoneLib = { enter, NULL, key, tick, draw, NULL, NULL, NULL, NULL };
