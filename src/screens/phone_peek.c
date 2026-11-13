/* The card a long press opens on a phone: it grows out of the card that was held, over the dimmed page, and plays
   the title's preview in its picture (the TV's previews: core/bgtrailer.c picks a clip or the trailer, btrailer.c
   plays it behind the rectangle this card makes see-through). Under the picture: the name, the year and length, the
   description, and Play, My List, Details. A tap outside or Back closes it. */
#include "screens/phone.h"
#include "screens/btrailer.h"
#include "core/form.h"
#include "core/i18n.h"
#include "core/hit.h"
#include "core/keys.h"
#include "core/stremio.h"
#include "ui/gfx.h"
#include "ui/text.h"
#include "ui/icon.h"
#include "ui/img.h"
#include "ui/widgets.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

static struct { int open, closing; Item it; float fx, fy, fw, fh; Tween k; double at; unsigned req; } K;
static Screen peekScreen;

int phone_peek_on(void) { return K.open && !K.closing; }
const Item *phone_peek_item(void) { return phone_peek_on() ? &K.it : NULL; }
float phone_peek_since(void) { return K.open ? (float)(app_now() - K.at) : 0; }

static void on_meta(cJSON *m, void *ud) { /* the description and the year line, when the row's card came without them */
  if (!m || (unsigned)(uintptr_t)ud != K.req || !K.open) return;
  Item t = K.it; sb_fill_item(&t, m, K.it.type);
  if (!K.it.description[0]) snprintf(K.it.description, sizeof K.it.description, "%s", t.description);
  if (!K.it.metaLine[0]) snprintf(K.it.metaLine, sizeof K.it.metaLine, "%s", t.metaLine);
  if (!K.it.logo[0]) snprintf(K.it.logo, sizeof K.it.logo, "%s", t.logo);
  if (!K.it.background[0]) snprintf(K.it.background, sizeof K.it.background, "%s", t.background);
}
void phone_peek_open(const Item *it, float x, float y, float w, float h) {
  K.it = *it; K.fx = x; K.fy = y; K.fw = w; K.fh = h; K.open = 1; K.closing = 0; K.at = app_now(); K.req++;
  tween_set(&K.k, 0); tween_to(&K.k, 1, 0.28f, EASE_STD);
  if (!K.it.description[0] || !K.it.metaLine[0]) sb_meta(K.it.type, K.it.id, on_meta, (void *)(uintptr_t)K.req);
  app_overlay = &peekScreen;
}
void phone_peek_shut_now(void) { if (!K.open) return; K.open = K.closing = 0; tween_set(&K.k, 0); if (app_overlay == &peekScreen) app_overlay = NULL; }
static void shut(void) { if (!K.open || K.closing) return; K.closing = 1; tween_to(&K.k, 0, 0.2f, EASE_CSS_EASEIN); }
static void close_act(int a, int b) { (void)a; (void)b; shut(); }
static void play_act(int a, int b) { (void)a; (void)b; Item it = K.it; shut(); app_overlay = NULL; K.open = 0;
  if (it.cw) { app_open_item(&it); return; }
  DetailParams d; memset(&d, 0, sizeof d); d.it = it; d.openStreams = 1; app_go(SCR_DETAIL, &d, 0); }
static void info_act(int a, int b) { (void)a; (void)b; Item it = K.it; shut(); app_overlay = NULL; K.open = 0; DetailParams d; memset(&d, 0, sizeof d); d.it = it; app_go(SCR_DETAIL, &d, 0); }
static void list_act(int a, int b) { (void)a; (void)b; int had = lib_has(K.it.id); lib_toggle(&K.it); toast(T(had ? "Removed from your Library" : "Added to your Library"), 1800, NULL); }

static int key(int k) { if (k == K_BACK) { shut(); return 1; } return 1; }
static void tick(float dt) {
  tween_tick(&K.k, dt);
  if (K.closing && K.k.v <= 0.001f) { K.open = K.closing = 0; if (app_overlay == &peekScreen) app_overlay = NULL; }
}
static float lerp(float a, float b, float k) { return a + (b - a) * k; }
static void draw(void) {
  if (!K.open) return; float k = K.k.v;
  gfx_rect(0, 0, SB_W, SB_H, BLACK_A((int)(175 * k)));
  hit_act(0, 0, SB_W, SB_H, close_act, 0, 0); /* beside the card: it closes */
  float W = SB_W - 32; if (W > 520) W = 520;
  float mh = W * 9 / 16, pad = 16;
  int dl = K.it.description[0] ? text_wrap_count(K.it.description, W - 2 * pad, 4, PH_TX_BODY, 400, 0) : 0;
  float H = mh + pad + 30 + (K.it.metaLine[0] ? 22 : 0) + (dl ? dl * 20 + 8 : 0) + 14 + 44 + pad;
  float X = (SB_W - W) / 2, Y = (SB_H - H) / 2; if (Y < 24) Y = 24;
  /* it grows out of the held card: the frame moves from there, the contents fade in once it is nearly there */
  float x = lerp(K.fx, X, k), y = lerp(K.fy, Y, k), w = lerp(K.fw, W, k), h = lerp(K.fh, H, k);
  gfx_shadow(x, y + 12, w, h, 16, 40, BLACK_A((int)(170 * k)));
  gfx_rrect(x, y, w, h, 16, PH_SURF);
  hit_shield(x, y, w, h);
  float ca = k < 0.6f ? 0 : (k - 0.6f) / 0.4f; if (ca <= 0) return;
  gfx_opacity_push(ca);
  /* the picture, then the preview over it once it plays */
  char bb[512]; const char *bu = sb_bg_mid(&K.it, bb, sizeof bb); gfx_clip_push(x, y, w, h);
  gfx_clip_push(X, Y, W, mh); phone_art(bu, X, Y, W, mh + 16, 16); gfx_clip_pop(); /* (cut square at the video's lower edge: none of it shows under the preview) */
  gfx_rrect_v(X, Y + mh - 50, W, 50, 0, PH_SURF & 0x00ffffffu, PH_SURF);
  if (k > 0.999f && !K.closing) { btr_card_rect(X, Y, W, mh); float va = btr_card_alpha(); if (va > 0.001f) gfx_hole(X, Y, W, mh, 16, va); }
  float ty = Y + mh + pad - 6;
  char lb[512]; const char *lu = sb_logo_of(&K.it, lb, sizeof lb); float asp; GLuint lt = lu && *lu ? img_get(lu, 300, 100, &asp) : 0;
  if (lt && asp > 0) { float lh = 30, lw = lh * asp; if (lw > W * 0.6f) { lw = W * 0.6f; lh = lw / asp; } gfx_image(lt, X + pad, ty + (30 - lh) / 2, lw, lh, 0, WHITE_A(255), 0, 0, 1, 1); }
  else text_draw(K.it.name, X + pad, ty, 22, 800, PH_TXT, -0.3f, W - 2 * pad, ALIGN_LEFT);
  ty += 34;
  if (K.it.metaLine[0]) { text_draw(K.it.metaLine, X + pad, ty, PH_TX_SMALL + 1, 600, PH_TXT2, 0, W - 2 * pad, ALIGN_LEFT); ty += 22; }
  if (dl) { text_draw_wrapped(K.it.description, X + pad, ty, W - 2 * pad, 4, PH_TX_BODY, 400, RGBA(0xf2, 0xf1, 0xf6, 205), 0, ALIGN_LEFT, 20); ty += dl * 20 + 8; }
  ty += 6;
  float bw = (W - 2 * pad - 16) / 3;
  phone_button(T(K.it.cw ? "Resume" : "Play"), "play", X + pad, ty, bw, 44, 1, play_act, 0, 0);
  phone_button(T("My List"), lib_has(K.it.id) ? "check" : "plus", X + pad + bw + 8, ty, bw, 44, 0, list_act, 0, 0);
  phone_button(T("Details"), "ph_info", X + pad + 2 * (bw + 8), ty, bw, 44, 0, info_act, 0, 0);
  gfx_clip_pop(); gfx_opacity_pop();
}
static Screen peekScreen = { NULL, NULL, key, tick, draw, NULL, NULL, NULL, NULL };
