/* A question before something with a cost, in the look of the other cards (the sheets' glass). OK says yes, Back says
   no, the other keys do nothing: a key flicking through a setting must not answer it by accident. */
#include "screens/confirmcard.h"
#include "core/app.h"
#include "core/keys.h"
#include "core/hit.h"
#include "ui/gfx.h"
#include "ui/text.h"
#include "ui/glass.h"
#include "ui/widgets.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static struct {
  int open;
  double at, outAt;
  char title[120], body[480], yes[64], no[64];
  ConfirmCb cb;
  void *ud;
} C;
static SGlass glass;
#define CW 1040.0f
#define CH 380.0f
static void finish(int yes) {
  if (!C.open)
    return;
  C.open = 0;
  C.outAt = app_now();
  ConfirmCb cb = C.cb;
  C.cb = NULL;
  if (cb)
    cb(yes, C.ud);
}
static int key(int k) {
  if (!C.open) {
    app_overlay = NULL;
    app_key(app_mirror_key(k), 1, 0);
    return 1;
  } /* on its way out: the key is the page's */
  if (k == K_OK)
    finish(1);
  else if (k == K_BACK)
    finish(0);
  return 1;
}
static void tick(float dt) {
  sglass_tick(&glass, dt);
  if (!C.open && app_now() - C.outAt >= 0.32 && app_overlay == &confirmCard)
    app_overlay = NULL;
}
static void on_click(int a, int b) {
  (void)b;
  finish(a);
}
static float ease_q(float x) {
  return x <= 0 ? 0 : x >= 1 ? 1 : 1 - powf(1 - x, 4);
}
static void draw(void) {
  float op, dy, sc;
  if (C.open) {
    float k = ease_q((float)(app_now() - C.at) / 0.3f);
    op = k;
    dy = 18 * (1 - k);
    sc = 0.97f + 0.03f * k;
  } else {
    float t = (float)(app_now() - C.outAt), k = ease_q(t / 0.22f), o = t / 0.18f;
    o = o > 1 ? 1 : o;
    op = 1 - (o < 0.5f ? 2 * o * o : 1 - powf(-2 * o + 2, 2) / 2);
    dy = 14 * k;
    sc = 1 - 0.03f * k;
  }
  if (op <= 0.001f)
    return;
  int rtl = app_rtl(), al = rtl ? ALIGN_RIGHT : ALIGN_LEFT;
  float x = SB_W / 2 - CW / 2, y = SB_H / 2 - CH / 2, P = 56, tw = CW - 2 * P, tx = rtl ? x + CW - P : x + P,
        ty = y + P;
  if (C.open)
    hit_act(0, 0, SB_W, SB_H, on_click, 0, 0); /* a click outside: no */
  gfx_opacity_push(op);
  gfx_offset_push(0, dy);
  gfx_scale_push(sc, sc, SB_W / 2, y + CH / 2);
  if (glass.ok && glass.a.v >= 0.999f)
    gfx_shadow_under(x, y + 30, CW, CH, 32, 80, BLACK_A(150), x, y, CW, CH, 32);
  else
    gfx_shadow(x, y + 30, CW, CH, 32, 80, BLACK_A(150));
  sglass_draw(&glass, x, y, CW, CH, 32);
  gfx_ring(x, y, CW, CH, 32, 1, WHITE_A(22));
  text_draw(C.title, tx, ty, 40, 700, WHITE_A(255), -0.5f, tw, al);
  ty += text_line_height(40) + 18;
  text_draw_wrapped(C.body, tx, ty, tw, 5, 28, 400, WHITE_A(215), 0, al, 28 * 1.42f);
  float hy = y + CH - P + 6, w1 = key_hint(tx, hy, K_OK, C.yes, WHITE_A(240), rtl), gap = 56,
        hx2 = rtl ? tx - w1 - gap : tx + w1 + gap, w2 = key_hint(hx2, hy, K_BACK, C.no, WHITE_A(200), rtl);
  if (C.open) {
    hit_act(x, y, CW, CH, NULL, 0, 0);
    hit_act(rtl ? tx - w1 : tx, hy - 30, w1, 60, on_click, 1, 0);
    hit_act(rtl ? hx2 - w2 : hx2, hy - 30, w2, 60, on_click, 0, 0);
  }
  gfx_scale_pop();
  gfx_offset_pop();
  gfx_opacity_pop();
}
Screen confirmCard = { NULL, NULL, key, tick, draw, NULL, NULL, NULL };
void confirm_show(const char *title, const char *body, const char *yes, const char *no, ConfirmCb cb, void *ud) {
  snprintf(C.title, sizeof C.title, "%s", title);
  snprintf(C.body, sizeof C.body, "%s", body);
  snprintf(C.yes, sizeof C.yes, "%s", yes);
  snprintf(C.no, sizeof C.no, "%s", no);
  C.cb = cb;
  C.ud = ud;
  C.at = app_now();
  C.open = 1;
  sglass_bake(&glass, SB_W / 2 - CW / 2, SB_H / 2 - CH / 2, CW, CH);
  app_overlay = &confirmCard;
}
