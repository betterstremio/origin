/* The phone's frame around its pages: the tab bar at the bottom, the pieces the pages share. See phone.h. */
#include "screens/phone.h"
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
#include <stdio.h>
#include <string.h>

void phone_register(void) {
  app_register(SCR_HOME, &phoneHome); app_register(SCR_DISC, &phoneDisc); app_register(SCR_DETAIL, &phoneDetail); app_register(SCR_SEARCH, &phoneSearch); app_register(SCR_LIB, &phoneLib); app_register(SCR_COL, &phoneCol);
}
int phone_owns(int scr) { return scr == SCR_HOME || scr == SCR_DISC || scr == SCR_DETAIL || scr == SCR_SEARCH || scr == SCR_LIB || scr == SCR_SET || scr == SCR_LOGIN || scr == SCR_COL; }

/* ---- the tab bar ---- */
static const struct { int scr; const char *icon, *label; } TABS[] = {
  { SCR_HOME, "ph_home", "Home" }, { SCR_DISC, "ph_disc", "Discover" }, { SCR_SEARCH, "ph_search", "Search" }, { SCR_LIB, "ph_lib", "Library" } };
#define NTABS 4
static int tab_shown(void) { return !player_open() && (SCR == SCR_HOME || SCR == SCR_DISC || SCR == SCR_SEARCH || SCR == SCR_LIB || SCR == SCR_SET); }
float phone_bottom(void) { return SB_H - (tab_shown() ? PH_TABBAR : 0); }
void phone_home_top(int scr);
static void tab_act(int scr, int b) { (void)b;
  if (scr == (int)SCR) { phone_home_top(scr); return; } /* the page's own tab again: back to its top */
  phone_go_tab(scr); }
void phone_go_tab(int scr) { extern void app_go_tab(Scr s); app_go_tab((Scr)scr); }
void phone_bars_draw(void) {
  if (!tab_shown()) return;
  float y = SB_H - PH_TABBAR, w = SB_W / NTABS;
  gfx_rect(0, y, SB_W, PH_TABBAR, RGBA(12, 11, 17, 246)); gfx_rect(0, y, SB_W, 1, WHITE_A(18));
  hit_shield(0, y, SB_W, PH_TABBAR);
  for (int i = 0; i < NTABS; i++) { float x = i * w; int on = TABS[i].scr == (int)SCR || (TABS[i].scr == SCR_HOME && SCR == SCR_SET && 0);
    uint32_t c = on ? PH_TXT : RGBA(0xf2, 0xf1, 0xf6, 130);
    int pr = touch_pressed(NULL, tab_act, TABS[i].scr, 0); if (pr) c = WHITE_A(200);
    icon_draw(TABS[i].icon, x + w / 2 - 12, y + 9, 24, c);
    text_draw(T(TABS[i].label), x + w / 2, y + 37, 11, on ? 700 : 500, c, 0, w - 4, ALIGN_CENTER);
    hit_act(x, y, w, PH_TABBAR, tab_act, TABS[i].scr, 0); }
}

/* ---- shared pieces ---- */
int phone_art(const char *url, float x, float y, float w, float h, float r) {
  GLuint t = url && *url ? img_get_shaped(url, (int)(w + 0.5f), (int)(h + 0.5f), r, 1) : 0;
  return ph_picture(t, x, y, w, h, r, WHITE_A(255));
}
void phone_press_scale(int pressed, float x, float y, float w, float h) { float s = pressed ? 0.96f : 1.0f; gfx_scale_push(s, s, x + w / 2, y + h / 2); }
void phone_poster(const Item *it, float x, float y, float w, float h) {
  char pb[512]; const char *u = sb_poster_of(it, pb, sizeof pb);
  if (!phone_art(u, x, y, w, h, PH_R) && !(u && *u)) { /* no poster at all: its name on the card */
    gfx_rrect(x, y, w, h, PH_R, PH_SURF); text_draw_wrapped(it->name, x + 8, y + h / 2 - 18, w - 16, 3, PH_TX_SMALL, 600, PH_TXT2, 0, ALIGN_CENTER, 0); }
  if (lib_item_watched(it)) { gfx_circle(x + w - 13, y + 13, 9, RGBA(0, 0, 0, 160)); icon_draw("check", x + w - 20, y + 6, 14, WHITE_A(240)); }
}
void phone_wide(const Item *it, float x, float y, float w, float h) {
  char bb[512]; const char *u = sb_bg_card(it, bb, sizeof bb); if (!u || !*u) u = sb_bg_mid(it, bb, sizeof bb);
  phone_art(u, x, y, w, h, PH_R);
  gfx_rrect_v(x, y + h * 0.45f, w, h * 0.55f, PH_R, RGBA(0, 0, 0, 0), RGBA(0, 0, 0, 200));
  char nm[300]; if (it->ep && it->epName[0]) snprintf(nm, sizeof nm, "%s", it->epName); else snprintf(nm, sizeof nm, "%s", it->name);
  text_draw(nm, x + 10, y + h - 30, PH_TX_SMALL + 1, 700, PH_TXT, 0, w - 20, ALIGN_LEFT);
  float p = it->progress; if (p > 0.001f) { gfx_rrect(x + 10, y + h - 9, w - 20, 3, 1.5f, WHITE_A(70)); gfx_rrect(x + 10, y + h - 9, (w - 20) * (p > 1 ? 1 : p), 3, 1.5f, ACC | 0xff000000u); }
}
float phone_button(const char *label, const char *icon, float x, float y, float w, float h, int primary, HitFn act, int a, int b) {
  float tw = label && *label ? text_width(label, PH_TX_BODY, 700, 0) : 0, iw = icon ? 20 : 0, gap = icon && tw > 0 ? 6 : 0;
  if (w <= 0) w = tw + iw + gap + 32;
  int pr = touch_pressed(NULL, act, a, b);
  uint32_t bg = primary ? (pr ? WHITE_A(200) : WHITE_A(255)) : (pr ? WHITE_A(60) : WHITE_A(38)), fg = primary ? RGB(12, 11, 17) : PH_TXT;
  gfx_rrect(x, y, w, h, h > 40 ? 8 : h / 2, bg);
  float cx = x + w / 2 - (tw + iw + gap) / 2;
  if (icon) icon_draw(icon, cx, y + h / 2 - 10, 20, fg);
  if (tw > 0) text_draw(label, cx + iw + gap, y + h / 2 - text_line_height(PH_TX_BODY) / 2.0f, PH_TX_BODY, 700, fg, 0, 0, ALIGN_LEFT);
  hit_act(x, y, w, h, act, a, b);
  return w;
}
