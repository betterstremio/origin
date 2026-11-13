/* Home and Discover on a phone: a hero at the top (the billboard's pick, its colours washing the page behind it), then
   every row at once, each a strip of cards the finger flings sideways; the page scrolls up and down. A tap opens a
   title (Continue Watching resumes it), a long press opens its card with the preview inside (phone_peek.c). The rows
   are the TV's own (board.c builds them; the For You rows load their cards as they come near the screen). */
#include "screens/phone.h"
#include "screens/board.h"
#include <stdio.h>
#include "screens/player.h"
#include "core/form.h"
#include "core/i18n.h"
#include "core/hit.h"
#include "core/stremio.h"
#include "core/collections.h"
#include "ui/gfx.h"
#include "ui/text.h"
#include "ui/icon.h"
#include "ui/img.h"
#include "ui/widgets.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAXR 64
typedef struct { int kind; Scroller page, rows[MAXR]; unsigned gen; int r0, r1, entered; float lastX, lastY; Item hero; int hasHero; Tween heroA; } Ph;
static Ph P[3] = { { .kind = 0 }, { .kind = 1 }, { .kind = 2 } }; /* Home, Discover, a collection tile's page */
static Ph *cur(void) { return SCR == SCR_DISC ? &P[1] : SCR == SCR_COL ? &P[2] : &P[0]; }

/* the sizes, from the canvas: about three and a third posters across a phone, more on a tablet */
static float poster_w(void) { float w = (SB_W - 2 * PH_M - 16) / 3.3f; return w < 100 ? 100 : w > 150 ? 150 : w; }
#define GAP 8.0f
static float card_w(const BoardRow *r) { float pw = poster_w(); if (r->ntiles) return r->tiles[0].shape == COL_POSTER ? pw : r->tiles[0].shape == COL_LANDSCAPE ? pw * 1.9f : pw * 1.2f; return r->wide ? pw * 1.9f : pw; }
static float card_h(const BoardRow *r) { float pw = poster_w(); if (r->ntiles) return r->tiles[0].shape == COL_POSTER ? pw * 1.5f : r->tiles[0].shape == COL_LANDSCAPE ? pw * 1.9f * 9 / 16 : pw * 1.2f; return r->wide ? pw * 1.9f * 9 / 16 : pw * 1.5f; }
static float rank_w(const BoardRow *r) { return r->rank ? poster_w() * 0.55f : 0; }
static float step_of(const BoardRow *r) { return card_w(r) + GAP + rank_w(r); }
static int n_cards(const BoardRow *r) { return r->ntiles ? r->ntiles : r->n; } /* (no See all card: a row loads its next page as it is flung near its end) */
#define ROW_TITLE 32.0f
#define ROW_GAP 22.0f
static float row_h(const BoardRow *r) { if (r->hidden && !r->closing) return 0; float h = ROW_TITLE + card_h(r) + ROW_GAP; return r->closing > 0 ? h * r->closing : h; }

/* ---- the cards: a tap opens, a long press holds ---- */
static void card_tap(int row, int col) { board_ph_open(cur()->kind, row, col); }
static void card_hold(int row, int col) {
  Ph *p = cur(); BoardRow r; if (!board_ph_row(p->kind, row, &r) || col < 0 || col >= r.n) return;
  float x, y, w, h; touch_hold_rect(&x, &y, &w, &h); phone_peek_open(&r.items[col], x, y, w, h); }
static void hero_tap(int a, int b) { (void)a; (void)b; Ph *p = cur(); if (p->hasHero) app_open_item(&p->hero); }
static void hero_hold(int a, int b) { (void)a; (void)b; Ph *p = cur(); if (!p->hasHero) return; float x, y, w, h; touch_hold_rect(&x, &y, &w, &h); phone_peek_open(&p->hero, x, y, w, h); }
static void hero_play(int a, int b) { (void)a; (void)b; Ph *p = cur(); if (!p->hasHero) return; DetailParams d; memset(&d, 0, sizeof d); d.it = p->hero; d.openStreams = 1; app_go(SCR_DETAIL, &d, 0); }
static void hero_list(int a, int b) { (void)a; (void)b; Ph *p = cur(); if (!p->hasHero) return; int had = lib_has(p->hero.id); lib_toggle(&p->hero); toast(T(had ? "Removed from your Library" : "Added to your Library"), 1800, NULL); }
static void open_settings(int a, int b) { (void)a; (void)b; app_go(SCR_SET, NULL, 0); }

/* the hero's pick: the billboard's first (Home) or the spotlight's (Discover); else the first poster of the first row */
static void pick_hero(Ph *p) {
  const Item *f[8]; int n = board_ph_feat(p->kind, f, 8); const Item *h = n ? f[0] : NULL;
  if (!h) for (int i = 0, nr = board_ph_nrows(p->kind); i < nr && !h; i++) { BoardRow r; if (board_ph_row(p->kind, i, &r) && !r.hidden && !r.wide && !r.ntiles && r.n) h = &r.items[0]; }
  if (!h) return;
  if (!p->hasHero || strcmp(p->hero.id, h->id)) { p->hero = *h; p->hasHero = 1; tween_set(&p->heroA, 0); tween_to(&p->heroA, 1, 0.35f, EASE_CSS_EASE); }
}

static void enter_k(int kind) {
  Ph *p = &P[kind]; board_ph_enter(kind);
  if (p->gen != board_ph_gen(kind)) { p->gen = board_ph_gen(kind); for (int i = 0; i < MAXR; i++) scroller_set(&p->rows[i], 0); }
  if (!p->entered) { p->entered = 1; scroller_set(&p->page, 0); }
}
static void enter_home(const void *a) { (void)a; enter_k(0); }
static void enter_disc(const void *a) { (void)a; enter_k(1); }
static char colTitle[96];
static void enter_col(const void *a) { if (a) { const ColFolder *f = *(const ColFolder *const *)a; snprintf(colTitle, sizeof colTitle, "%s", f ? f->title : ""); }
  colScreen.enter(a); P[2].entered = 0; P[2].gen = 0; enter_k(2); P[2].hasHero = 0; }
static void back_act(int a, int b) { (void)a; (void)b; app_back(); }
void phone_home_top(int scr) { if (scr == SCR_HOME || scr == SCR_DISC) scroller_to(&P[scr == SCR_DISC].page, 0); }

static void tick(float dt) {
  Ph *p = cur(); int kind = p->kind;
  if (p->gen != board_ph_gen(kind)) { p->gen = board_ph_gen(kind); for (int i = 0; i < MAXR; i++) scroller_set(&p->rows[i], 0); }
  board_ph_tick(kind, p->r0, p->r1, dt);
  scroller_tick(&p->page, dt); for (int i = 0; i < MAXR; i++) scroller_tick(&p->rows[i], dt);
  if (p->kind != 2) pick_hero(p); tween_tick(&p->heroA, dt); /* (a tile's page: its rows alone) */
}

/* ---- drawing ---- */
static float hero_draw(Ph *p, float y) {
  if (!p->hasHero) return 0;
  const Item *it = &p->hero; int land = SB_W >= 600 || SB_W > SB_H * 0.8f; /* a tablet or a phone turned: the wide picture */
  float x = 24, w = SB_W - 48, h = land ? w * 0.5f : w * 1.42f; float cap = (phone_bottom() - PH_TOPBAR) * (land ? 0.5f : 0.72f); if (h > cap) { h = cap; if (!land) { w = h / 1.42f; x = (SB_W - w) / 2; } }
  float top = y + PH_TOPBAR + 8, a = p->heroA.v;
  /* its colours wash down the page behind it */
  uint32_t tint[4]; char pb[512], lb[512]; const char *pu = sb_poster_of(it, pb, sizeof pb), *lu = sb_logo_of(it, lb, sizeof lb);
  if (accent_tint_of(it->id, pu, lu, tint)) { uint32_t c = tint[0] & 0x00ffffffu; gfx_opacity_push(a); gfx_rrect_v(0, y, SB_W, top + h + 40 - y, 0, c | 0x8c000000u, c & 0x00ffffffu); gfx_opacity_pop(); }
  int pr = touch_pressed(NULL, hero_tap, 0, 0);
  gfx_opacity_push(a); phone_press_scale(pr, x, top, w, h);
  gfx_shadow(x, top + 10, w, h, 14, 30, BLACK_A(150));
  char bb[512]; const char *art = land ? sb_bg_large(it, bb, sizeof bb) : pu; phone_art(art, x, top, w, h, 14);
  gfx_rrect_v(x, top + h * 0.55f, w, h * 0.45f, 14, BLACK_A(0), BLACK_A(215));
  hit_add_hold(x, top, w, h, NULL, hero_tap, hero_hold, 0, 0);
  float by = top + h - 16 - 42;
  if (land) { float lw, lh; GLuint lt = lu && *lu ? img_get(lu, 300, 100, NULL) : 0; (void)lw; (void)lh;
    if (lt) { float asp; img_get(lu, 300, 100, &asp); float mh = 64, mw = mh * asp; if (mw > w * 0.4f) { mw = w * 0.4f; mh = mw / asp; } gfx_image(lt, x + 20, by - 12 - mh, mw, mh, 0, WHITE_A(255), 0, 0, 1, 1); }
    else text_draw(it->name, x + 20, by - 44, 26, 800, PH_TXT, -0.5f, w * 0.5f, ALIGN_LEFT); }
  else if (it->metaLine[0]) text_draw(it->metaLine, x + w / 2, by - 26, PH_TX_SMALL, 600, PH_TXT2, 0, w - 32, ALIGN_CENTER);
  float bw = land ? 140 : (w - 32 - 10) / 2, bx = land ? x + 20 : x + 16;
  phone_button(T("Play"), "play", bx, by, bw, 42, 1, hero_play, 0, 0);
  phone_button(T("My List"), lib_has(it->id) ? "check" : "plus", bx + bw + 10, by, bw, 42, 0, hero_list, 0, 0);
  gfx_scale_pop(); gfx_opacity_pop();
  return top + h + 24 - y;
}
static void rank_num(int n, float x, float y, float w, float h) {
  char t[8]; snprintf(t, sizeof t, "%d", n); int px = (int)(h * 0.62f);
  text_draw(t, x + w + 6, y + h - text_line_height(px) * 0.92f, px, 800, RGBA(0xf2, 0xf1, 0xf6, 70), -px * 0.04f, 0, ALIGN_RIGHT); }
static void row_draw(Ph *p, int ri, const BoardRow *r, float y) {
  float a = r->closing > 0 ? r->closing : 1; if (a < 1) gfx_opacity_push(a);
  text_draw(r->title, PH_M, y + 4, PH_TX_ROW, 700, PH_TXT, -0.2f, SB_W - 2 * PH_M, ALIGN_LEFT);
  float cy = y + ROW_TITLE, cw = card_w(r), ch = card_h(r), st = step_of(r), rk = rank_w(r); int n = n_cards(r);
  Scroller *s = &p->rows[ri < MAXR ? ri : MAXR - 1];
  scroller_limits(s, 0, n * st - GAP + 2 * PH_M - SB_W);
  touch_region(0, cy - 4, SB_W, ch + 8, s, NULL);
  if (!n && r->loading) { for (int i = 0; i * st < SB_W; i++) ph_box(PH_M + i * st + rk, cy, cw, ch, PH_R); if (a < 1) gfx_opacity_pop(); return; }
  float off = s->off; int i0 = (int)((off - PH_M) / st) - 1, i1 = (int)((off + SB_W) / st) + 1; if (i0 < 0) i0 = 0; if (i1 > n - 1) i1 = n - 1;
  for (int i = i0; i <= i1; i++) { float x = PH_M + i * st - off; int pr = touch_pressed(NULL, card_tap, ri, i);
    if (r->rank && i < 10 && i < r->n) rank_num(i + 1, x - 6, cy, rk, ch);
    float px = x + rk; phone_press_scale(pr, px, cy, cw, ch);
    if (r->ntiles) { const ColFolder *f = &r->tiles[i]; phone_art(f->cover, px, cy, cw, ch, PH_R); if (!f->hideTitle || !f->cover[0]) text_draw(f->title, px + cw / 2, cy + ch / 2 - 10, PH_TX_BODY, 700, PH_TXT, 0, cw - 12, ALIGN_CENTER); }
    else if (i < r->n) { if (r->wide) phone_wide(&r->items[i], px, cy, cw, ch); else phone_poster(&r->items[i], px, cy, cw, ch); }
    else { gfx_rrect(px, cy, cw, ch, PH_R, PH_SURF); text_draw(T("See all"), px + cw / 2, cy + ch / 2 - 10, PH_TX_BODY, 700, PH_TXT, 0, cw - 12, ALIGN_CENTER); }
    gfx_scale_pop();
    if (i < r->n) hit_add_hold(px, cy, cw, ch, NULL, card_tap, card_hold, ri, i); else hit_add(px, cy, cw, ch, NULL, card_tap, ri, i); }
  if (r->n && i1 >= r->n - 6) board_ph_near_end(p->kind, ri, i1);
  if (a < 1) gfx_opacity_pop();
}
static void topbar_draw(Ph *p) {
  float k = p->page.off / 120; if (k < 0) k = 0; if (k > 1) k = 1;
  gfx_rect(0, 0, SB_W, PH_TOPBAR, RGBA(12, 11, 17, (int)(255 * k))); if (k >= 1) gfx_rect(0, PH_TOPBAR - 1, SB_W, 1, WHITE_A(14));
  if (k < 1) gfx_rrect_v(0, 0, SB_W, PH_TOPBAR + 24, 0, BLACK_A((int)(120 * (1 - k))), BLACK_A(0));
  if (p->kind == 2) { int pr = touch_pressed(NULL, back_act, 0, 0); hit_shield(0, 0, SB_W, PH_TOPBAR); gfx_circle(PH_M + 18, PH_TOPBAR / 2, 18, pr ? WHITE_A(60) : BLACK_A(90)); icon_draw("ph_back", PH_M + 6, PH_TOPBAR / 2 - 12, 24, PH_TXT); hit_act(0, 0, 64, PH_TOPBAR, back_act, 0, 0);
    text_draw(colTitle, 60, PH_TOPBAR / 2 - text_line_height(20) / 2.0f, 20, 800, PH_TXT, -0.3f, SB_W - 76, ALIGN_LEFT); return; }
  float tw = text_draw(T(p->kind ? "Discover" : "Home"), PH_M, PH_TOPBAR / 2 - text_line_height(22) / 2.0f, 22, 800, PH_TXT, -0.4f, SB_W / 2, ALIGN_LEFT);
  { /* the phone app is a beta: said where it is seen first */ float bw = text_width("BETA", 10, 800, 0.8f) + 12, bx = PH_M + tw + 8, by = PH_TOPBAR / 2 - 9;
    gfx_rrect(bx, by, bw, 18, 9, col_alpha(ACC | 0xff000000u, 0.85f)); text_draw("BETA", bx + bw / 2, by + 9 - text_line_height(10) / 2.0f, 10, 800, ACC_FG | 0xff000000u, 0.8f, 0, ALIGN_CENTER); }
  float bx = SB_W - PH_M - 36; int pr = touch_pressed(NULL, open_settings, 0, 0);
  gfx_circle(bx + 18, PH_TOPBAR / 2, 17, pr ? WHITE_A(60) : WHITE_A(26)); icon_draw("nav_set", bx + 7, PH_TOPBAR / 2 - 11, 22, PH_TXT);
  hit_shield(0, 0, SB_W, PH_TOPBAR); /* (first: the button goes on top of it) */
  hit_act(bx - 6, 0, 48, PH_TOPBAR, open_settings, 0, 0);
}
static void draw(void) {
  Ph *p = cur(); int kind = p->kind; float bottom = phone_bottom();
  gfx_rect(0, 0, SB_W, SB_H, PAGE);
  touch_region(0, 0, SB_W, bottom, NULL, &p->page);
  gfx_clip_push(0, 0, SB_W, bottom);
  float y0 = -p->page.off, y = y0;
  y += hero_draw(p, y);
  if (!p->hasHero) y += PH_TOPBAR + 8;
  int nr = board_ph_nrows(kind), r0 = -1, r1 = -1;
  for (int i = 0; i < nr; i++) { BoardRow r; if (!board_ph_row(kind, i, &r)) continue; float h = row_h(&r); if (h <= 0) continue;
    if (y + h > 0 && y < bottom) { if (r0 < 0) r0 = i; r1 = i; row_draw(p, i, &r, y); }
    y += h; }
  if (!nr) { for (int i = 0; i < 3; i++) { ph_text(PH_M, y + 4, 140, 1, PH_TX_ROW, 0, ALIGN_LEFT); for (int c = 0; c < 4; c++) ph_box(PH_M + c * (poster_w() + GAP), y + ROW_TITLE, poster_w(), poster_w() * 1.5f, PH_R); y += ROW_TITLE + poster_w() * 1.5f + ROW_GAP; } }
  p->r0 = r0 < 0 ? 0 : r0; p->r1 = r1 < 0 ? 0 : r1;
  float contentH = y - y0 + 12;
  scroller_limits(&p->page, 0, contentH - bottom);
  gfx_clip_pop();
  topbar_draw(p);
}
static int key(int k) { (void)k; return 0; } /* (a keyboard or a remote on a phone: nothing to move between; Back leaves) */
static int origin(float *x, float *y) { (void)x; (void)y; return 0; }
Screen phoneHome = { enter_home, NULL, key, tick, draw, NULL, NULL, NULL, origin };
Screen phoneDisc = { enter_disc, NULL, key, tick, draw, NULL, NULL, NULL, origin };
Screen phoneCol = { enter_col, NULL, key, tick, draw, NULL, NULL, NULL, origin };
