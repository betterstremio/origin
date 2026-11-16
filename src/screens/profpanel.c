/* The profile menu (3.0.0): Netflix's own, a small dark card that grows out of the bar's profile picture (the picture
   stays where it is and becomes the card's first row). Rows: the profile in use ("Switch profiles": the full chooser,
   with Add profile and the PINs), the other profiles (one press switches; a PIN asks first), then Settings, Share logs
   and Exit. One light pill marks the focus. A Stremio account: its letter and address, then the three rows. Opened by
   Left from Search or a click on the picture; Right or Back closes it, the focus back on Search. */
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
#define CARD_X 36.0f /* the card: its corner just above and left of the picture */
#define CARD_Y 4.0f
#define CARD_R 24.0f
#define PAD 12.0f /* the card to its rows */
#define TOP_H 88.0f /* the first row (two lines); the picture centred on the bar's line, as in the bar */
#define ROW_H 68.0f /* the others */
#define PIC_X 68.0f /* the bar's picture: the column of pictures and icons */
#define PIC_D 48.0f
#define TXT_X 140.0f
#define CARD_C RGBA(0x31, 0x31, 0x34, 248)
#define PILL_ON RGB(0xe5, 0xe5, 0xe5)
#define INK_ON RGB(0x14, 0x14, 0x14)

enum { R_SET, R_LOGS, R_EXIT };
static struct {
  int open, idx; /* idx: the focusable rows, top to bottom */
  Tween a; /* 0 closed .. 1 open */
  Tween pillY, pillH;
  Nudge nudge;
} PP;
static Screen ov;

static int nuvio(void) {
  return acct_is_nuvio() && nv_profile_count() > 0;
}
/* the profile rows: [0] the one in use, then the others (their places in the account's list); how many */
static int profile_rows(int *at) {
  if (!nuvio())
    return 0;
  int n = nv_profile_count(), k = 1, cur = 0;
  NvProfile p;
  if (n > MAXP)
    n = MAXP;
  for (int i = 0; i < n; i++)
    if (nv_profile_get(i, &p) && p.index == nv_profile_current())
      cur = i;
  at[0] = cur;
  for (int i = 0; i < n; i++)
    if (i != cur)
      at[k++] = i;
  return k;
}
static int count(void) {
  int at[MAXP];
  return profile_rows(at) + 3;
}
static int row_of(int i) {
  int at[MAXP];
  return i - profile_rows(at);
} /* R_SET.. for one of the three, negative for a profile */
static int live(void) {
  return PP.open && app_overlay == &ov;
} /* open and taking the keys (not under the PIN pad) */
static float mx(float x, float w) {
  return app_rtl() ? SB_W - x - w : x;
} /* a box at x in the left-to-right layout, where it goes in a mirrored one */

static void close_to_search(void) {
  if (!PP.open)
    return;
  PP.open = 0;
  tween_to(&PP.a, 0, 0.18f, EASE_CSS_EASE);
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
  topnav_focus();
} /* gone at once: the page changes under it anyway */
static void back_here(void) {
  PP.open = 1;
  app_overlay = &ov;
} /* Back out of the PIN pad: the menu takes the keys again (it stayed drawn under it) */

static void act(int i) {
  int at[MAXP], np = profile_rows(at), r = i - np;
  if (r < 0) {
    if (i == 0) {
      close_now();
      profiles_open(PF_SWITCH); /* the full chooser: every profile, Add profile, the PINs */
      return;
    }
    NvProfile p;
    if (!nv_profile_get(at[i], &p))
      return;
    if (p.pin) {
      profiles_open_pin(at[i], back_here);
      return;
    }
    close_now();
    sound_play(SND_TAB);
    profiles_switch(at[i]);
    topnav_leave(); /* Home of that profile, the focus on its billboard */
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
  } /* the colour keys: the menu goes, then Settings or Search */
  return 1;
}
static void tick(float dt) {
  if (PP.open && app_overlay != &ov && !profiles_is_open()) {
    PP.open = 0;
    tween_set(&PP.a, 0);
    topnav_leave();
  } /* a profile chosen through its PIN: the menu goes with the pad, the focus to the new Home */
  tween_tick(&PP.a, dt);
  tween_tick(&PP.pillY, dt);
  tween_tick(&PP.pillH, dt);
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
} /* a click beside the card closes it */

static void pic(const NvProfile *p, float x, float y) {
  gfx_rrect(x, y, PIC_D, PIC_D, 8, RGB((p->color >> 16) & 255, (p->color >> 8) & 255, p->color & 255));
  if (profile_avatar_draw_r(p->avatar, x, y, PIC_D, 8))
    return;
  char ini[8] = { 0 };
  const unsigned char *u = (const unsigned char *)p->name;
  int len = !*u ? 0 : (*u & 0x80) ? ((*u & 0xe0) == 0xc0 ? 2 : (*u & 0xf0) == 0xe0 ? 3 : 4) : 1;
  memcpy(ini, p->name, (size_t)len);
  if (ini[0] >= 'a' && ini[0] <= 'z')
    ini[0] -= 32;
  text_draw(ini, x + PIC_D / 2, y + PIC_D / 2 - text_line_height(24) / 2.0f, 24, 600, WHITE_A(255), 0, 0, ALIGN_CENTER);
}
static void draw(void) {
  float a = PP.a.v;
  if (a <= 0.001f)
    return;
  float e = ease_apply(GLIDE_EASE, a);
  int rtl = app_rtl(), al = rtl ? ALIGN_RIGHT : ALIGN_LEFT, at[MAXP], np = profile_rows(at), nuv = nuvio();
  static const char *const ic[3] = { "nav_set", "set_logs", "set_out" };
  const char *lb[3] = { T("Settings"), T("Share logs"), T("Exit") };
  /* its size: the rows' words decide the width (never narrower than 380), their number the height */
  float tw = 0, upd = update_avail() ? 18 + text_width(T("Update available"), 20, 500, 0) : 0;
  for (int i = 0; i < 3; i++) {
    float w = text_width(lb[i], 26, 500, 0) + (i == R_SET ? upd : 0);
    tw = w > tw ? w : tw;
  }
  for (int i = 0; i < np; i++) {
    NvProfile p;
    if (nv_profile_get(at[i], &p)) {
      float w = text_width(p.name, 26, i ? 500 : 600, 0);
      tw = w > tw ? w : tw;
    }
  }
  if (np) {
    float w = text_width(T("Switch profiles"), 22, 400, 0);
    tw = w > tw ? w : tw;
  } else {
    float w = text_width(sb_user_email(), 22, 400, 0);
    tw = w > tw ? (w > 520 ? 520 : w) : tw;
  }
  float W = TXT_X - CARD_X + tw + 48, H = 2 * PAD + TOP_H + (np ? np - 1 : 0) * ROW_H + 3 * ROW_H;
  W = W < 380 ? 380 : W;
  /* it grows out of the picture: from the picture's own box to its size, the rows fading in as it opens */
  float sw = PIC_D + 2 * (PIC_X - CARD_X), sh = PIC_D + 2 * (56 - PIC_D / 2 - CARD_Y);
  float cw = sw + (W - sw) * e, ch = sh + (H - sh) * e, cx = mx(CARD_X, cw);
  float nx = nudge_x(&PP.nudge), ny = nudge_y(&PP.nudge);
  if (live())
    hit_act(0, 0, SB_W, SB_H, h_out, 0, 0);
  gfx_offset_push(nx, ny);
  gfx_opacity_push(a < 0.25f ? a / 0.25f : 1);
  gfx_shadow(cx, CARD_Y + 18, cw, ch, CARD_R, 48, BLACK_A((int)(120 * e)));
  gfx_rrect(cx, CARD_Y, cw, ch, CARD_R, CARD_C);
  gfx_opacity_pop();
  gfx_clip_push(cx, CARD_Y, cw, ch);
  /* the focus: one light pill, gliding from row to row */
  float rows[MAXP + 3], hts[MAXP + 3], y = CARD_Y + PAD;
  int n = 0;
  if (!nuv) {
    y += TOP_H; /* a Stremio account: its letter and address, not a row to choose */
  }
  for (int i = 0; i < np; i++) {
    rows[n] = y;
    hts[n++] = i ? ROW_H : TOP_H;
    y += i ? ROW_H : TOP_H;
  }
  for (int i = 0; i < 3; i++) {
    rows[n] = y;
    hts[n++] = ROW_H;
    y += ROW_H;
  }
  int f = PP.idx < n ? PP.idx : 0;
  if (PP.pillH.v < 1 || a < 0.05f) {
    tween_set(&PP.pillY, rows[f]);
    tween_set(&PP.pillH, hts[f]);
  } else {
    tween_to(&PP.pillY, rows[f], 0.2f, EASE_CSS_EASE);
    tween_to(&PP.pillH, hts[f], 0.2f, EASE_CSS_EASE);
  }
  float ca = e < 0.35f ? 0 : (e - 0.35f) / 0.65f; /* the rows come in once the card has mostly opened */
  gfx_opacity_push(ca);
  gfx_rrect(mx(CARD_X + PAD, W - 2 * PAD), PP.pillY.v, W - 2 * PAD, PP.pillH.v, 16, PILL_ON);
  gfx_opacity_pop();
  /* the picture of the profile in use never fades: it is the bar's, now the card's first row */
  if (!nuv) {
    app_profile_pic(mx(PIC_X, PIC_D), 56 - PIC_D / 2, PIC_D, 8);
    gfx_opacity_push(ca);
    text_draw(T("Account"), mx(TXT_X, 0), 56 - text_line_height(26) + 2, 26, 600, WHITE_A(235), 0, 0, al);
    text_draw(sb_user_email(), mx(TXT_X, 0), 58, 22, 400, WHITE_A(160), 0, 520, al);
    gfx_opacity_pop();
  }
  for (int i = 0; i < n; i++) {
    int on = i == f;
    float cy = rows[i] + hts[i] / 2;
    uint32_t ink = on ? INK_ON : WHITE_A(235), sub = on ? col_alpha(INK_ON, 0.7f) : WHITE_A(160);
    if (i < np) {
      NvProfile p;
      if (!nv_profile_get(at[i], &p))
        continue;
      if (i)
        gfx_opacity_push(ca);
      pic(&p, mx(PIC_X, PIC_D), cy - PIC_D / 2);
      if (!i)
        gfx_opacity_push(ca);
      if (!i) {
        text_draw(p.name, mx(TXT_X, 0), cy - text_line_height(26) + 2, 26, 600, ink, 0, 0, al);
        text_draw(T("Switch profiles"), mx(TXT_X, 0), cy + 2, 22, 400, sub, 0, 0, al);
      } else
        text_draw(p.name, mx(TXT_X, 0), cy - text_line_height(26) / 2.0f, 26, 500, ink, 0, 0, al);
      gfx_opacity_pop();
    } else {
      int r = i - np;
      gfx_opacity_push(ca);
      icon_draw(ic[r], mx(PIC_X + 8, 32), cy - 16, 32, ink);
      text_draw(lb[r], mx(TXT_X, 0), cy - text_line_height(26) / 2.0f, 26, 500, ink, 0, 0, al);
      if (r == R_SET && upd > 0) { /* an update waiting: said here; the bar's dot pointed to it */
        float lw = text_width(lb[r], 26, 500, 0), dx = rtl ? SB_W - TXT_X - lw - 14 : TXT_X + lw + 14;
        gfx_circle(dx, cy, 5, on ? INK_ON : ACC);
        text_draw(T("Update available"), rtl ? dx - 12 : dx + 12, cy - text_line_height(20) / 2.0f, 20, 500, sub, 0, 0,
                  al);
      }
      gfx_opacity_pop();
    }
    if (live())
      hit_add(mx(CARD_X + PAD, W - 2 * PAD), rows[i], W - 2 * PAD, hts[i], h_foc, h_act, i, 0);
  }
  gfx_clip_pop();
  gfx_offset_pop();
}
static Screen ov = { NULL, NULL, key, tick, draw, NULL, NULL, NULL };

int profpanel_open(void) {
  if (app_overlay || SCR == SCR_LOGIN || !sb_signed_in())
    return 0;
  PP.open = 1;
  PP.idx = 0; /* the profile in use ("Switch profiles"); a Stremio account: Settings */
  tween_set(&PP.pillH, 0);
  tween_to(&PP.a, 1, 0.24f, GLIDE_EASE);
  app_overlay = &ov;
  topNav.idx = NAV_SEARCH;
  topnav_focus(); /* the focus stays with the bar (the page shows none), its pill gone while the menu is out */
  sound_focus();
  return 1;
}
int profpanel_is_open(void) {
  return PP.open;
}
float profpanel_k(void) {
  return PP.a.v;
}
