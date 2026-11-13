#include "core/touch.h"
#include "core/app.h"
#include "core/form.h"
#include "ui/gfx.h"
#include "platform/plat.h"
#include <SDL2/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOP 8.0f /* dp a finger may wander and still tap */
#define HOLD_S 0.45 /* a long press */
#define FRICTION 2.1f /* a fling's slowing, per second (about iOS's normal rate) */
#define VMAX 7000.0f /* dp per second */

/* ---- the scroller ---- */
void scroller_limits(Scroller *s, float min, float max) {
  s->min = min;
  s->max = max < min ? min : max;
}
void scroller_set(Scroller *s, float off) {
  s->off = off;
  s->vel = 0;
  s->settling = 0;
}
void scroller_to(Scroller *s, float off) {
  if (fabsf(off - s->off) < 0.5f) {
    scroller_set(s, off);
    return;
  }
  s->from = s->off;
  s->settleTo = off;
  s->settleT = 0;
  s->settling = 1;
  s->vel = 0;
}
int scroller_moving(const Scroller *s) {
  return s->drag || s->settling || fabsf(s->vel) > 1 || s->off < s->min - 0.5f || s->off > s->max + 0.5f;
}
void scroller_tick(Scroller *s, float dt) {
  if (dt <= 0)
    return;
  if (s->drag) {
    touch_note_moving();
    return;
  }
  if (s->settling) {
    s->settleT += dt / 0.3f;
    float k = s->settleT >= 1 ? 1 : 1 - powf(1 - s->settleT, 3);
    s->off = s->from + (s->settleTo - s->from) * k;
    if (s->settleT >= 1)
      s->settling = 0;
    touch_note_moving();
    return;
  }
  int out = s->off < s->min || s->off > s->max;
  if (!out && fabsf(s->vel) <= 1)
    return;
  touch_note_moving();
  if (out) {
    float lim = s->off < s->min ? s->min : s->max;
    int outward = (s->off < s->min && s->vel < 0) || (s->off > s->max && s->vel > 0);
    s->vel *= expf(-dt * (outward ? 18.0f : 30.0f));
    s->off += s->vel * dt;
    s->off += (lim - s->off) * (1 - expf(-dt * 11.0f)); /* the rubber band pulls it back */
    if (fabsf(s->off - lim) < 0.4f && fabsf(s->vel) < 20) {
      s->off = lim;
      s->vel = 0;
    }
    return;
  }
  s->off += s->vel * dt;
  s->vel *= expf(-dt * FRICTION);
  if (fabsf(s->vel) < 12)
    s->vel = 0;
}

/* ---- where the scrollers are ---- */
typedef struct {
  float x, y, w, h;
  Scroller *sx, *sy;
} Region;
#define MAXREG 128
static Region reg[MAXREG];
static int nreg;
static int movingNow;
void touch_note_moving(void) {
  movingNow = 1;
}
void touch_begin_frame(void) {
  nreg = 0;
}
void touch_region(float x, float y, float w, float h, Scroller *sx, Scroller *sy) {
  if (nreg >= MAXREG)
    return;
  float x0 = x, y0 = y, x1 = x + w, y1 = y + h;
  gfx_xform_point(&x0, &y0);
  gfx_xform_point(&x1, &y1);
  float cx, cy, cw, ch;
  if (gfx_clip_get(&cx, &cy, &cw, &ch)) {
    if (x0 < cx)
      x0 = cx;
    if (y0 < cy)
      y0 = cy;
    if (x1 > cx + cw)
      x1 = cx + cw;
    if (y1 > cy + ch)
      y1 = cy + ch;
  }
  if (x1 <= x0 || y1 <= y0)
    return;
  Region *r = &reg[nreg++];
  r->x = x0;
  r->y = y0;
  r->w = x1 - x0;
  r->h = y1 - y0;
  r->sx = sx;
  r->sy = sy;
}
static int reg_has(const Region *r, float x, float y) {
  return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

/* ---- the finger ---- */
#define NS 8
static struct {
  int down, moved, held, caught, axis; /* axis 0 x, 1 y */
  float x0, y0, x, y;
  double t0;
  Hit hit;
  int hasHit;
  Scroller *drag;
  float dragOff0, dragExtent;
  float sp[NS], st[NS];
  int ns; /* recent positions along the dragged axis, and when */
} F;
static double now_s(void) {
  return SDL_GetTicks() / 1000.0;
}
static void sample(float p) {
  if (F.ns == NS) {
    memmove(F.sp, F.sp + 1, (NS - 1) * sizeof(float));
    memmove(F.st, F.st + 1, (NS - 1) * sizeof(float));
    F.ns--;
  }
  F.sp[F.ns] = p;
  F.st[F.ns] = (float)fmod(now_s(), 100000.0);
  F.ns++;
}
static float
velocity(void) { /* dp per second over the last 100 ms (a finger that stopped before lifting flings nothing) */
  if (F.ns < 2)
    return 0;
  float tN = F.st[F.ns - 1];
  int i = F.ns - 1;
  while (i > 0 && tN - F.st[i - 1] <= 0.1f)
    i--;
  if ((float)fmod(now_s(), 100000.0) - tN > 0.05f)
    return 0;
  float dt = tN - F.st[i];
  if (dt < 0.008f)
    return 0;
  float v = (F.sp[F.ns - 1] - F.sp[i]) / dt;
  return v > VMAX ? VMAX : v < -VMAX ? -VMAX : v;
}
/* past an end the content follows the finger less and less */
static float rubber(float d, float extent) {
  if (extent < 1)
    extent = 1;
  return (1 - 1 / (d * 0.55f / extent + 1)) * extent;
}

#include "screens/player.h"
void touch_down(float x, float y) {
  if (player_open()) {
    memset(&F, 0, sizeof F);
    F.caught = 1;
    player_touch(0, x, y);
    app_touch();
    return;
  } /* the film reads the finger itself */
  memset(&F, 0, sizeof F);
  F.down = 1;
  F.x0 = F.x = x;
  F.y0 = F.y = y;
  F.t0 = now_s();
  const Hit *h = hit_at(x, y);
  if (h) {
    F.hit = *h;
    F.hasHit = 1;
  }
  for (int i = nreg - 1; i >= 0; i--)
    if (reg_has(&reg[i], x, y)) { /* a finger on a moving page stops it, and is no tap */
      Scroller *s[2] = { reg[i].sx, reg[i].sy };
      for (int k = 0; k < 2; k++)
        if (s[k] && (fabsf(s[k]->vel) > 60 || s[k]->settling)) {
          s[k]->vel = 0;
          s[k]->settling = 0;
          F.caught = 1;
        }
    }
  app_touch();
}
static void begin_drag(void) {
  float dx = F.x - F.x0, dy = F.y - F.y0;
  F.axis = fabsf(dy) >= fabsf(dx) ? 1 : 0;
  for (int pass = 0; pass < 2 && !F.drag; pass++) {
    int ax = pass ? !F.axis : F.axis; /* the way it moved most; else the other way, when only that one scrolls here */
    for (int i = nreg - 1; i >= 0; i--) {
      Region *r = &reg[i];
      if (!reg_has(r, F.x0, F.y0))
        continue;
      Scroller *s = ax ? r->sy : r->sx;
      if (!s)
        continue;
      if (pass && s->max <= s->min)
        continue;
      F.drag = s;
      F.axis = ax;
      F.dragExtent = ax ? r->h : r->w;
      break;
    }
  }
  if (!F.drag)
    return;
  F.drag->drag = 1;
  F.drag->vel = 0;
  F.drag->settling = 0;
  F.dragOff0 = F.drag->off;
  F.ns = 0;
  /* from where the finger is now, so it does not jump by the slop */
  F.x0 = F.x;
  F.y0 = F.y;
  sample(F.axis ? F.y : F.x);
}
void touch_move(float x, float y) {
  if (player_open() && F.caught && !F.down) {
    player_touch(1, x, y);
    return;
  }
  if (!F.down)
    return;
  F.x = x;
  F.y = y;
  if (!F.moved) {
    float dx = x - F.x0, dy = y - F.y0;
    if (dx * dx + dy * dy < SLOP * SLOP)
      return;
    F.moved = 1;
    if (!F.held)
      begin_drag();
    app_touch();
  }
  if (!F.drag)
    return;
  float p = F.axis ? y : x, p0 = F.axis ? F.y0 : F.x0;
  sample(p);
  Scroller *s = F.drag;
  float raw = F.dragOff0 - (p - p0);
  if (raw < s->min)
    raw = s->min - rubber(s->min - raw, F.dragExtent);
  else if (raw > s->max)
    raw = s->max + rubber(raw - s->max, F.dragExtent);
  s->off = raw;
}
static void tap(float x, float y) {
  if (getenv("SB_TOUCHLOG"))
    fprintf(stderr, "[touch] tap %.0f,%.0f%s\n", x, y, F.hasHit ? "" : " (nothing there)");
  app_pointer_button(1, x, y);
  app_pointer_button(0, x, y); /* its act, or focus + OK as a click does (TV pages) */
}
void touch_up(float x, float y) {
  if (player_open() && F.caught && !F.down) {
    F.caught = 0;
    player_touch(2, x, y);
    return;
  }
  if (!F.down)
    return;
  F.x = x;
  F.y = y;
  if (F.drag) {
    Scroller *s = F.drag;
    s->drag = 0;
    float v = velocity();
    s->vel = -v;
    if (getenv("SB_TOUCHLOG"))
      fprintf(stderr, "[touch] fling %.0f dp/s\n", -v);
    F.drag = NULL;
  } else if (!F.moved && !F.held && !F.caught)
    tap(F.x0, F.y0);
  F.down = 0;
}
void touch_cancel(void) {
  if (F.drag) {
    F.drag->drag = 0;
    F.drag->vel = 0;
  }
  memset(&F, 0, sizeof F);
}
static float holdR[4];
void touch_hold_rect(float *x, float *y, float *w, float *h) {
  *x = holdR[0];
  *y = holdR[1];
  *w = holdR[2];
  *h = holdR[3];
}
void touch_tick(void) {
  if (!F.down || F.moved || F.held || F.caught)
    return;
  if (now_s() - F.t0 < HOLD_S)
    return;
  F.held = 1;
  const Hit *h = hit_at(F.x0, F.y0);
  if (getenv("SB_TOUCHLOG"))
    fprintf(stderr, "[touch] long press %.0f,%.0f%s\n", F.x0, F.y0, h && h->hold ? "" : " (as a held OK)");
  if (h && h->hold) {
    holdR[0] = h->x;
    holdR[1] = h->y;
    holdR[2] = h->w;
    holdR[3] = h->h;
    plat_haptic();
    h->hold(h->a, h->b);
    return;
  }
  if (app_pointer_menu(F.x0, F.y0))
    plat_haptic(); /* a TV page: the card's menu */
}
int touch_busy(void) {
  int m = movingNow;
  movingNow = 0;
  return F.down || m;
}
int touch_pressed(HitFn focus, HitFn act, int a, int b) {
  return F.down && !F.moved && !F.caught && F.hasHit && F.hit.focus == focus && F.hit.act == act && F.hit.a == a &&
         F.hit.b == b;
}
