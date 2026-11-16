/* The profile panel (3.0.0, so/header-SPEC.md section 6 and so/header-INTERACTION.md): a column out of the left edge
   over the page, Netflix's "Choose a Profile" shape. A Nuvio account's profiles (the focused one larger, its name
   beside it), Add profile, a thin line, then Settings, Share logs and Exit pinned at the bottom. A Stremio account: its
   initial, then the three. Opened by Left from Search or a click on the bar's picture; Right or Back closes it, the
   focus back on Search. The PIN pad and the add form are profiles.c's, over it; Back out of them returns here. */
#include "screens/profpanel.h"
#include "screens/profiles.h"
#include "screens/logcard.h"
#include "core/app.h"
#include "core/keys.h"
#include "core/hit.h"
#include "core/i18n.h"
#include "core/nuvio.h"
#include "core/update.h"
#include "core/stremio.h"
#include "ui/gfx.h"
#include "ui/text.h"
#include "ui/icon.h"
#include "ui/sound.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAXP 8
#define COL_X 294.0f /* the column's centre */
#define TILE 112.0f /* a profile; the focused one FOC */
#define FOC 160.0f
#define GAP 24.0f
#define LIST_TOP 172.0f
#define ROW_H 72.0f
#define ROWS_END (SB_H - 60) /* the last row's foot */
#define ROWS_TOP (ROWS_END - 3 * ROW_H)
#define LIST_END (ROWS_TOP - 56) /* the line sits 28 above the rows, the list 28 above the line */
#define PILL_ON RGB(0xd9, 0xd9, 0xd9)
#define INK_ON RGB(0x14, 0x14, 0x14)

enum { R_SET, R_LOGS, R_EXIT };
static struct {
  int open, idx; /* idx: 0..np-1 the profiles, np Add profile (when it can), then the three rows */
  Tween a; /* 0 closed .. 1 open */
  Tween grow[MAXP + 1], scroll, rowY, rowA;
  Nudge nudge;
} PP;
static Screen ov;

static int nuvio(void) {
  return acct_is_nuvio() && nv_profile_count() > 0;
}
static int np(void) {
  int n = nuvio() ? nv_profile_count() : 0;
  return n > MAXP ? MAXP : n;
}
static int has_add(void) {
  return nuvio() && profiles_can_add();
}
static int tiles(void) {
  return np() + has_add();
} /* the focusable tiles above the line */
static int count(void) {
  return tiles() + 3;
}
static int row_of(int i) {
  return i - tiles();
} /* R_SET.. for a row, negative for a tile */
static int cur_pos(void) {
  NvProfile p;
  for (int i = 0; i < np(); i++)
    if (nv_profile_get(i, &p) && p.index == nv_profile_current())
      return i;
  return 0;
}
static int live(void) {
  return PP.open && app_overlay == &ov;
} /* open and taking the keys (not under the PIN pad or the add form) */
static float colx(void) {
  return app_rtl() ? SB_W - COL_X : COL_X;
}

static void close_to_search(void) {
  if (!PP.open)
    return;
  PP.open = 0;
  tween_to(&PP.a, 0, 0.2f, EASE_CSS_EASE);
  if (app_overlay == &ov)
    app_overlay = NULL;
  topNav.active = 1;
  topNav.idx = NAV_SEARCH;
  topnav_focus();
}
static void close_now(void) {
  PP.open = 0;
  tween_set(&PP.a, 0);
  if (app_overlay == &ov)
    app_overlay = NULL;
} /* gone at once: the page changes under it anyway */
static void back_here(void) {
  PP.open = 1;
  app_overlay = &ov;
} /* Back out of the PIN pad or the add form: the panel takes the keys again (it stayed drawn under them) */

static void act(int i) {
  int r = row_of(i);
  if (r < 0) {
    if (i == np()) {
      profiles_open_add(back_here); /* Add profile: the form over the panel */
      return;
    }
    NvProfile p;
    if (!nv_profile_get(i, &p))
      return;
    if (p.index == nv_profile_current()) {
      close_to_search();
      return;
    } /* the one in use: nothing to switch */
    if (p.pin) {
      profiles_open_pin(i, back_here);
      return;
    }
    close_now();
    sound_play(SND_TAB);
    profiles_switch(i); /* Home of that profile, the focus on its billboard */
    return;
  }
  close_now();
  if (r == R_SET) {
    if (update_avail())
      update_open(); /* an update waiting: straight to Settings > About */
    else
      app_open_settings();
  } else if (r == R_LOGS)
    logcard_show();
  else
    app_quit(); /* chosen from a menu: no second question */
}
static void step(int d) {
  int n = count(), i = PP.idx + d;
  if (i < 0 || i >= n) {
    nudge_start(&PP.nudge, d < 0 ? K_UP : K_DOWN);
    return;
  }
  PP.idx = i;
  sound_focus();
}
static int key(int k) {
  if (!live())
    return 1;
  if (k == K_UP || k == K_DOWN)
    step(k == K_UP ? -1 : 1);
  else if (k == K_OK)
    act(PP.idx);
  else if (k == K_RIGHT || k == K_BACK)
    close_to_search();
  else if (k == K_LEFT)
    nudge_start(&PP.nudge, K_LEFT);
  else if (k == K_RED || k == K_GREEN) {
    close_now();
    app_color_key(k);
  } /* the colour keys: the panel goes, then Settings or Search */
  return 1;
}
static void tick(float dt) {
  if (PP.open && app_overlay != &ov && !profiles_is_open()) {
    PP.open = 0;
    tween_set(&PP.a, 0);
  } /* a profile chosen through its PIN: the panel goes with the pad */
  tween_tick(&PP.a, dt);
  for (int i = 0; i <= MAXP; i++) {
    tween_to(&PP.grow[i], PP.idx == i && i < tiles() ? 1 : 0, 0.2f, EASE_CSS_EASE);
    tween_tick(&PP.grow[i], dt);
  }
  tween_tick(&PP.scroll, dt);
  tween_tick(&PP.rowY, dt);
  tween_tick(&PP.rowA, dt);
  nudge_tick(&PP.nudge, dt);
}
/* the pointer */
static void h_foc(int i, int b) {
  (void)b;
  if (live() && PP.idx != i) {
    PP.idx = i;
    sound_focus();
  }
}
static void h_act(int i, int b) {
  (void)b;
  if (live()) {
    PP.idx = i;
    act(i);
  }
}
static void h_out(int a, int b) {
  (void)a;
  (void)b;
  close_to_search();
} /* a click beside the column closes it */

static void add_tile(float x, float y, float D, float k) {
  gfx_ring(x, y, D, D, 12 + 4 * k, 2, WHITE_A((int)(128 + 100 * k)));
  float l = D * 0.36f, t = 4;
  gfx_rrect(x + D / 2 - l / 2, y + D / 2 - t / 2, l, t, t / 2, WHITE_A(230));
  gfx_rrect(x + D / 2 - t / 2, y + D / 2 - l / 2, t, l, t / 2, WHITE_A(230));
}
static void draw(void) {
  float a = PP.a.v;
  if (a <= 0.001f)
    return;
  float e = ease_apply(GLIDE_EASE, a);
  int rtl = app_rtl();
  /* the page dims a quarter; the column's dark falls off to the right (left in a mirrored layout) */
  gfx_rect(0, 0, SB_W, SB_H, BLACK_A((int)(64 * a)));
  if (rtl) {
    gfx_rect(SB_W - 560, 0, 560, SB_H, BLACK_A((int)(250 * a)));
    gfx_rrect_h(SB_W - 1060, 0, 500, SB_H, 0, BLACK_A(0), BLACK_A((int)(250 * a)));
  } else {
    gfx_rect(0, 0, 560, SB_H, BLACK_A((int)(250 * a)));
    gfx_rrect_h(560, 0, 500, SB_H, 0, BLACK_A((int)(250 * a)), BLACK_A(0));
  } /* (solid under the words: nothing of the page reads through them) */
  if (live())
    hit_act(0, 0, SB_W, SB_H, h_out, 0, 0);
  gfx_offset_push((rtl ? 1 : -1) * 480 * (1 - e) + nudge_x(&PP.nudge), nudge_y(&PP.nudge));
  gfx_opacity_push(a);
  float cx = colx();
  int al = rtl ? ALIGN_RIGHT : ALIGN_LEFT;
  text_draw(nuvio() ? T("Profiles") : T("Account"), cx, 112, 26, 600, WHITE_A(178), 0, 0, ALIGN_CENTER);
  /* the tiles: the focused one larger; the list scrolls to keep it near y 420 when it is long */
  int nt = tiles();
  float hs[MAXP + 1], tot = 0;
  for (int i = 0; i < nt; i++) {
    hs[i] = TILE + (FOC - TILE) * PP.grow[i].v;
    tot += hs[i] + (i ? GAP : 0);
  }
  if (!nuvio())
    tot = TILE;
  {
    float want = 0, room = LIST_END - LIST_TOP;
    if (tot > room && PP.idx < nt) {
      float c = 0;
      for (int i = 0; i < PP.idx; i++)
        c += TILE + GAP;
      want = c + FOC / 2 - (420 - LIST_TOP);
      want = want < 0 ? 0 : want > tot - room ? tot - room : want;
    } else if (tot > room)
      want = PP.scroll.to; /* on the rows below: the list stays where it was */
    if (fabsf(want - PP.scroll.to) > 0.5f)
      tween_to(&PP.scroll, want, 0.2f, EASE_CSS_EASE);
  }
  gfx_clip_push(rtl ? SB_W - 600 : 0, LIST_TOP - 8, 600, LIST_END - LIST_TOP + 16);
  float y = LIST_TOP - PP.scroll.v;
  if (!nuvio()) {
    app_profile_pic(cx - TILE / 2, y, TILE, 14); /* a Stremio account: its letter, not something to choose */
  } else
    for (int i = 0; i < nt; i++) {
      float D = hs[i], x = cx - D / 2, k = PP.grow[i].v;
      if (i < np()) {
        NvProfile p;
        if (nv_profile_get(i, &p)) {
          gfx_rrect(x, y, D, D, 12 + 4 * k, RGB((p.color >> 16) & 255, (p.color >> 8) & 255, p.color & 255));
          if (!profile_avatar_draw_r(p.avatar, x, y, D, 12 + 4 * k)) {
            char ini[8] = { 0 };
            const unsigned char *u = (const unsigned char *)p.name;
            int len = !*u ? 0 : (*u & 0x80) ? ((*u & 0xe0) == 0xc0 ? 2 : (*u & 0xf0) == 0xe0 ? 3 : 4) : 1;
            memcpy(ini, p.name, (size_t)len);
            if (ini[0] >= 'a' && ini[0] <= 'z')
              ini[0] -= 32;
            int px = (int)(D * 0.46f);
            text_draw(ini, x + D / 2, y + D / 2 - text_line_height(px) / 2.0f, px, 600, WHITE_A(255), 0, 0,
                      ALIGN_CENTER);
          }
          if (k > 0.01f) { /* its name beside it, after the tile has grown a little */
            float nk = k < 0.4f ? 0 : (k - 0.4f) / 0.6f, nx = rtl ? x - 32 : x + D + 32;
            gfx_opacity_push(nk);
            float ny = y + D / 2 - text_line_height(36) / 2.0f - (p.pin ? 14 : 0);
            text_draw(p.name, nx, ny, 36, 600, WHITE_A(255), 0, 300, al);
            if (p.pin)
              text_draw(T("PIN"), nx, ny + text_line_height(36) + 2, 22, 600, WHITE_A(150), 1, 0, al);
            gfx_opacity_pop();
          }
        }
      } else {
        add_tile(x, y, D, k);
        if (k > 0.01f) {
          gfx_opacity_push(k < 0.4f ? 0 : (k - 0.4f) / 0.6f);
          text_draw(T("Add profile"), rtl ? x - 32 : x + D + 32, y + D / 2 - text_line_height(36) / 2.0f, 36, 600,
                    WHITE_A(255), 0, 300, al);
          gfx_opacity_pop();
        }
      }
      if (live())
        hit_add(rtl ? x - 340 : x, y, D + 340, D, h_foc, h_act, i, 0);
      y += D + GAP;
    }
  gfx_clip_pop();
  /* the line, then Settings / Share logs / Exit: one light pill glides to the focused row */
  gfx_rect(cx - 100, ROWS_TOP - 28, 200, 1, WHITE_A(51));
  int r = row_of(PP.idx);
  {
    float ty = ROWS_TOP + (r >= 0 ? r : 0) * ROW_H + (ROW_H - 60) / 2;
    if (PP.rowA.v < 0.01f)
      tween_set(&PP.rowY, ty);
    else
      tween_to(&PP.rowY, ty, 0.2f, EASE_CSS_EASE);
    tween_to(&PP.rowA, r >= 0 ? 1 : 0, 0.2f, EASE_CSS_EASE);
  }
  float rx = rtl ? SB_W - 214 : 214, rw = 300;
  if (PP.rowA.v > 0.001f)
    gfx_rrect(rtl ? rx - rw + 24 : rx - 24, PP.rowY.v, rw, 60, 30, col_alpha(PILL_ON, PP.rowA.v));
  static const char *const ic[3] = { "nav_set", "set_logs", "set_out" };
  const char *lb[3] = { T("Settings"), T("Share logs"), T("Exit") };
  for (int i = 0; i < 3; i++) {
    float yy = ROWS_TOP + i * ROW_H, cy = yy + ROW_H / 2;
    int f = r == i;
    uint32_t c = f ? col_mix(WHITE_A(217), INK_ON, PP.rowA.v) : WHITE_A(217);
    icon_draw(ic[i], rtl ? rx - 32 : rx, cy - 16, 32, c);
    float tx = rtl ? rx - 48 : rx + 48;
    text_draw(lb[i], tx, cy - text_line_height(30) / 2.0f, 30, 500, c, 0, 0, al);
    if (i == R_SET && update_avail()) { /* an update waiting: said on the row, the bar's dot points here */
      float tw = text_width(lb[i], 30, 500, 0), dx = rtl ? tx - tw - 18 : tx + tw + 18;
      gfx_circle(dx, cy, 5, f ? INK_ON : ACC);
      text_draw(T("Update available"), rtl ? dx - 14 : dx + 14, cy - text_line_height(22) / 2.0f, 22, 600,
                f ? col_alpha(INK_ON, 0.75f) : WHITE_A(170), 0, 0, al);
    }
    if (live())
      hit_add(rtl ? rx - rw + 24 : rx - 24, yy + (ROW_H - 60) / 2, rw + 160, 60, h_foc, h_act, nt + i, 0);
  }
  gfx_opacity_pop();
  gfx_offset_pop();
}
static Screen ov = { NULL, NULL, key, tick, draw, NULL, NULL, NULL };

int profpanel_open(void) {
  if (app_overlay || SCR == SCR_LOGIN || !sb_signed_in())
    return 0;
  PP.open = 1;
  PP.idx = nuvio() ? cur_pos() : tiles(); /* the profile in use; a Stremio account: Settings */
  tween_set(&PP.scroll, 0);
  tween_set(&PP.rowA, 0);
  for (int i = 0; i <= MAXP; i++)
    tween_set(&PP.grow[i], i == PP.idx && i < tiles() ? 1 : 0);
  tween_to(&PP.a, 1, GLIDE_DUR, GLIDE_EASE);
  topnav_leave(); /* the bar's pill fades while the column is out */
  app_overlay = &ov;
  sound_focus();
  return 1;
}
int profpanel_is_open(void) {
  return PP.open;
}
