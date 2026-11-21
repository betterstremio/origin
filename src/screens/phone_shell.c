/* The phone's frame around its pages: the tab bar or tablet dock, the pieces the pages share. See phone.h. */
#include "screens/phone.h"
#include "screens/board.h"
#include "screens/player.h"
#include "core/form.h"
#include "core/i18n.h"
#include "core/hit.h"
#include "core/stremio.h"
#include "ui/gfx.h"
#include "ui/text.h"
#include "ui/icon.h"
#include "ui/img.h"
#include "ui/widgets.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* The Android window is edge-to-edge while browsing. Keep the system's room in one place so every phone page,
   including overlays and Settings, starts below the same status-bar/cutout edge. form_safe() already throttles the
   platform query; this extra frame cache avoids asking it repeatedly when a page draws several shared pieces. */
static unsigned inset_frame = (unsigned)-1;
static float inset_top, inset_bottom;
static void phone_insets(void) {
  unsigned fr = gfx_frame();
  if (inset_frame == fr)
    return;
  inset_frame = fr;
  float left, right;
  form_safe(&inset_top, &inset_bottom, &left, &right);
  const char *dev = getenv("SB_TOP_INSET");
  if (dev && *dev) {
    float v = (float)atof(dev);
    if (v >= 0)
      inset_top = v;
  }
}

void phone_register(void) {
  app_register(SCR_HOME, &phoneHome);
  app_register(SCR_DISC, &phoneDisc);
  app_register(SCR_DETAIL, &phoneDetail);
  app_register(SCR_SEARCH, &phoneSearch);
  app_register(SCR_LIB, &phoneLib);
  app_register(SCR_COL, &phoneCol);
}
int phone_owns(int scr) {
  return scr == SCR_HOME || scr == SCR_DISC || scr == SCR_DETAIL || scr == SCR_SEARCH || scr == SCR_LIB ||
         scr == SCR_SET || scr == SCR_LOGIN || scr == SCR_COL;
}

int phone_is_tablet(void) {
  float shortSide = SB_W < SB_H ? SB_W : SB_H;
  return form_touch() && shortSide >= 600;
}

/* ---- the tab bar ---- */
static const struct {
  int scr;
  const char *icon, *label;
} TABS[] = { { SCR_HOME, "ph_home", "Home" },
             { SCR_SEARCH, "ph_search", "Search" },
             { SCR_LIB, "ph_lib", "My List" } };
#define NTABS 3
static int tab_shown(void) {
  return !player_open() &&
         (SCR == SCR_HOME || SCR == SCR_DISC || SCR == SCR_SEARCH || SCR == SCR_LIB || SCR == SCR_SET);
}
float phone_content_top(void) {
  return phone_top_inset() + (phone_is_tablet() ? 80 : 0); /* 12 + 56 dock + 12 breathing room */
}
float phone_bottom(void) {
  phone_insets();
  return SB_H - inset_bottom - (tab_shown() && !phone_is_tablet() ? PH_TABBAR : 0);
}
float phone_top_inset(void) {
  phone_insets();
  return inset_top;
}
void phone_home_top(int scr);
static void tab_act(int scr, int b) {
  (void)b;
  if (scr == (int)SCR) {
    phone_home_top(scr);
    return;
  } /* the page's own tab again: back to its top */
  if (scr == SCR_HOME && SCR == SCR_DISC) {
    phone_go_tab(SCR_HOME);
    return;
  } /* Home is lit on its tab pages: tapped, it goes Home */
  phone_go_tab(scr);
}
void phone_go_tab(int scr) {
  extern void app_go_tab(Scr s);
  app_go_tab((Scr)scr);
}

enum { DOCK_SEARCH, DOCK_HOME, DOCK_SHOWS, DOCK_MOVIES, DOCK_ANIME, DOCK_CATALOGUE, DOCK_LIB, DOCK_PROFILE };
typedef struct {
  int id, type;
  const char *label, *icon;
} DockItem;

static int dock_shown(void) {
  return tab_shown() && !app_overlay && !phone_peek_on();
}
static int dock_current(int id) {
  if (id == DOCK_SEARCH)
    return SCR == SCR_SEARCH;
  if (id == DOCK_HOME)
    return SCR == SCR_HOME;
  if (id == DOCK_LIB)
    return SCR == SCR_LIB;
  if (id == DOCK_PROFILE)
    return SCR == SCR_SET;
  if (SCR != SCR_DISC)
    return 0;
  return (id == DOCK_SHOWS && board_disc_type() == 1) || (id == DOCK_MOVIES && board_disc_type() == 2) ||
         (id == DOCK_ANIME && board_disc_type() == 4) ||
         (id == DOCK_CATALOGUE && board_disc_type() == 3);
}
static void dock_act(int id, int b) {
  (void)b;
  if (id == DOCK_PROFILE) {
    app_open_settings();
    return;
  }
  if (id == DOCK_SEARCH) {
    phone_go_tab(SCR_SEARCH);
    return;
  }
  if (id == DOCK_HOME) {
    phone_go_tab(SCR_HOME);
    return;
  }
  if (id == DOCK_LIB) {
    phone_go_tab(SCR_LIB);
    return;
  }
  int type = id == DOCK_MOVIES ? 2 : id == DOCK_ANIME ? 4 : id == DOCK_CATALOGUE ? 3 : 1;
  if (SCR == SCR_DISC && board_disc_type() == type)
    return;
  board_disc_type_set(type);
  if (SCR == SCR_DISC)
    app_go(SCR_DISC, NULL, 1);
  else
    phone_go_tab(SCR_DISC);
}
static float dock_item_width(const DockItem *it) {
  if (it->id == DOCK_PROFILE)
    return 44;
  if (it->icon)
    return 56;
  return text_width(T(it->label), 15, 600, 0) + 32;
}
static void dock_draw(void) {
  static const DockItem all[] = {
      { DOCK_SEARCH, 0, "Search", "ph_search" }, { DOCK_HOME, 0, "Home", NULL },
      { DOCK_SHOWS, 1, "Shows", NULL },          { DOCK_MOVIES, 2, "Movies", NULL },
      { DOCK_ANIME, 4, "Anime", NULL },          { DOCK_CATALOGUE, 3, "Catalogue", NULL },
      { DOCK_LIB, 0, "My List", NULL },          { DOCK_PROFILE, 0, NULL, NULL } };
  DockItem items[sizeof all / sizeof *all];
  int n = 0, rtl = app_rtl();
  for (size_t i = 0; i < sizeof all / sizeof *all; i++) {
    if ((all[i].id == DOCK_ANIME && !app_anime_on()) || (all[i].id == DOCK_CATALOGUE && !app_catalogue_on()))
      continue;
    items[n++] = all[i];
  }
  float total = 8, y = phone_top_inset() + 12, h = 56;
  for (int i = 0; i < n; i++)
    total += dock_item_width(&items[i]);
  float x = SB_W / 2 - total / 2;
  gfx_rrect(x, y, total, h, 28, RGBA(40, 40, 44, 215));
  gfx_ring(x, y, total, h, 28, 1, WHITE_A(22));
  hit_shield(x, y, total, h);
  for (int i = 0; i < n; i++) {
    const DockItem *it = &items[rtl ? n - i - 1 : i];
    float w = dock_item_width(it), ix = x + 4;
    int pr = touch_pressed(NULL, dock_act, it->id, 0);
    gfx_rrect(ix, y + 6, w, 44, 22, pr ? WHITE_A(30) : dock_current(it->id) ? WHITE_A(46) : RGBA(0, 0, 0, 0));
    if (it->id == DOCK_PROFILE) {
      app_profile_pic(ix + 4, y + 10, 36, 18);
    } else if (it->icon) {
      icon_draw(it->icon, ix + w / 2 - 12, y + 16, 24, pr ? WHITE_A(30) : dock_current(it->id) ? WHITE_A(255) : WHITE_A(204));
    } else {
      uint32_t c = pr ? WHITE_A(30) : dock_current(it->id) ? WHITE_A(255) : WHITE_A(204);
      text_draw(T(it->label), ix + w / 2, y + 28 - text_line_height(15) / 2.0f, 15, 600, c, 0, w - 8, ALIGN_CENTER);
    }
    hit_act(ix, y + 6, w, 44, dock_act, it->id, 0);
    x += w;
  }
}
void phone_bars_draw(void) {
  if (!tab_shown())
    return;
  if (phone_is_tablet()) {
    if (dock_shown())
      dock_draw();
    return;
  }
  float y = phone_bottom(), w = SB_W / NTABS;
  y -= PH_TABBAR;
  gfx_rect(0, y, SB_W, PH_TABBAR, RGBA(18, 18, 20, 246));
  gfx_rect(0, y, SB_W, 1, WHITE_A(18));
  hit_shield(0, y, SB_W, PH_TABBAR);
  for (int i = 0; i < NTABS; i++) {
    float x = i * w;
    int on = TABS[i].scr == (int)SCR || (TABS[i].scr == SCR_HOME && SCR == SCR_DISC);
    uint32_t c = on ? PH_TXT : RGBA(0xf2, 0xf1, 0xf6, 130);
    int pr = touch_pressed(NULL, tab_act, TABS[i].scr, 0);
    if (pr)
      c = WHITE_A(200);
    icon_draw(TABS[i].icon, x + w / 2 - 12, y + 9, 24, c);
    text_draw(T(TABS[i].label), x + w / 2, y + 37, 11, on ? 700 : 500, c, 0, w - 4, ALIGN_CENTER);
    hit_act(x, y, w, PH_TABBAR, tab_act, TABS[i].scr, 0);
  }
}

/* ---- shared pieces ---- */
int phone_art(const char *url, float x, float y, float w, float h, float r) {
  GLuint t = url && *url ? img_get_shaped(url, (int)(w + 0.5f), (int)(h + 0.5f), r, 1) : 0;
  return ph_picture(t, x, y, w, h, r, WHITE_A(255));
}
void phone_press_scale(int pressed, float x, float y, float w, float h) {
  float s = pressed ? 0.96f : 1.0f;
  gfx_scale_push(s, s, x + w / 2, y + h / 2);
}
void phone_poster(const Item *it, float x, float y, float w, float h) {
  char pb[512];
  const char *u = sb_poster_of(it, pb, sizeof pb);
  if (!phone_art(u, x, y, w, h, PH_R) && !(u && *u)) { /* no poster at all: its name on the card */
    gfx_rrect(x, y, w, h, PH_R, PH_SURF);
    text_draw_wrapped(it->name, x + 8, y + h / 2 - 18, w - 16, 3, PH_TX_SMALL, 600, PH_TXT2, 0, ALIGN_CENTER, 0);
  }
  if (lib_item_watched(it)) {
    gfx_circle(x + w - 13, y + 13, 9, RGBA(0, 0, 0, 160));
    icon_draw("check", x + w - 20, y + 6, 14, WHITE_A(240));
  }
}
void phone_wide(const Item *it, float x, float y, float w, float h) {
  char bb[512];
  const char *u = sb_bg_card(it, bb, sizeof bb);
  if (!u || !*u)
    u = sb_bg_mid(it, bb, sizeof bb);
  phone_art(u, x, y, w, h, PH_R);
  gfx_rrect_v(x, y + h * 0.45f, w, h * 0.55f, PH_R, RGBA(0, 0, 0, 0), RGBA(0, 0, 0, 200));
  char nm[300];
  if (it->ep && it->epName[0])
    snprintf(nm, sizeof nm, "%s", it->epName);
  else
    snprintf(nm, sizeof nm, "%s", it->name);
  text_draw(nm, x + 10, y + h - 30, PH_TX_SMALL + 1, 700, PH_TXT, 0, w - 20, ALIGN_LEFT);
  float p = it->progress;
  if (p > 0.001f) {
    gfx_rrect(x + 10, y + h - 9, w - 20, 3, 1.5f, WHITE_A(70));
    gfx_rrect(x + 10, y + h - 9, (w - 20) * (p > 1 ? 1 : p), 3, 1.5f, WHITE_A(235));
  }
}
float phone_button(const char *label, const char *icon, float x, float y, float w, float h, int primary, HitFn act,
                   int a, int b) {
  float tw = label && *label ? text_width(label, PH_TX_BODY, 700, 0) : 0, iw = icon ? 20 : 0,
        gap = icon && tw > 0 ? 6 : 0;
  if (w <= 0)
    w = tw + iw + gap + 32;
  int pr = touch_pressed(NULL, act, a, b);
  uint32_t bg = primary ? (pr ? WHITE_A(200) : WHITE_A(255)) : (pr ? WHITE_A(60) : WHITE_A(38)),
           fg = primary ? RGB(18, 18, 20) : PH_TXT;
  gfx_rrect(x, y, w, h, h > 40 ? 8 : h / 2, bg);
  float cx = x + w / 2 - (tw + iw + gap) / 2;
  if (icon)
    icon_draw(icon, cx, y + h / 2 - 10, 20, fg);
  if (tw > 0)
    text_draw(label, cx + iw + gap, y + h / 2 - text_line_height(PH_TX_BODY) / 2.0f, PH_TX_BODY, 700, fg, 0, 0,
              ALIGN_LEFT);
  hit_act(x, y, w, h, act, a, b);
  return w;
}
