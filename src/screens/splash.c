#include "screens/splash.h"
#include "screens/board.h"
#include "screens/splash_mark.h"
#include "core/app.h"
#include "ui/gfx.h"
#include "ui/img.h"
#include "ui/icon.h"
#include "ui/text.h"
#include "ui/sound.h"
#include "platform/plat.h"
#include "screens/login.h"
#include "screens/welcome.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* The start screen: on near-black, two thin arcs of light, cyan and magenta, swell into crescents and close into the ring of
   the logo; the play fades in; the ring lifts and the name sharpens under it, still until Home is complete; then the camera
   flies through the hole in the ring and Home opens up inside it. All of it is drawn (see splash_mark.h): no picture or video to load. */
static int on, waitHome, gone, frames; static double t0, cap, signedAt, firstDraw, lastDraw, worstGap, worstAt, animStart; static Timer1 out; static float frozen = -1, frozenX = -1;
#define MIN_UP 0.9   /* the entrance fits inside this, so the app starts no later than it did */
#define PACE 1.96f   /* the animation is written on a 1.76 s timeline (g below); this plays it in MIN_UP */
#define BG RGB(4, 3, 15)
#define EXIT 0.5f    /* the flight through the ring into Home; Home is live under it from its first frame */
#define ZOOM 12.0f   /* how far: the hole ends wider than the screen */
/* ready = the first two rows' visible posters are in, the hero text is set and the backdrop is up:
   Home is revealed complete, never with empty cards; 2.5 s at most (a slow network), 9 s right after signing in (nothing cached yet) */
void splash_start(int waitForHome, double signedInAt) {
  int justSignedIn = signedInAt > 0; signedAt = signedInAt;
  on = 1; gone = 0; frames = 0; animStart = 0; firstDraw = lastDraw = worstGap = worstAt = 0; waitHome = waitForHome; t0 = justSignedIn ? app_now() - 2 : app_now(); cap = justSignedIn ? 9.0 : 2.5; /* the restart after signing in: already up, no second entrance */
  { const char *f = getenv("SB_SPLASH_T"); frozen = f ? (float)atof(f) : -1; f = getenv("SB_SPLASH_X"); frozenX = f ? (float)atof(f) : -1; } /* development: held at this moment, for pictures of it */
  if (!justSignedIn && waitForHome) sound_intro();
}
static void splash_end(void) {
  if (gone) return; gone = 1; timer_start(&out, signedAt > 0 ? 0.36f : EXIT); app_splash_done = 1;
  fprintf(stderr, "[start] ready in %.0f ms, first frame at %.0f ms, %d frames, worst gap %.0f ms at %.0f ms of the animation\n", (app_now() - t0) * 1000, (animStart - t0) * 1000, frames, worstGap * 1000, worstAt * 1000);
  if (waitHome) app_reveal_start();
  if (signedAt > 0) welcome_open_now(); /* right after signing in: the guide comes straight from the loading (Home revealed under it) */
}
int splash_up(void) { return on; }
int splash_signed_in(void) { return on && signedAt > 0; }
void splash_tick(float dt) {
  if (!on || frozen >= 0) return;
  if (gone) { if (timer_tick(&out, dt) >= 1 && !out.on) on = 0; return; }
  double el = app_now() - t0;
  if (signedAt <= 0 && el < cap && (!animStart || app_now() - animStart < MIN_UP)) return; /* the animation plays out, counted from the frame it first appeared on */
  if (el < MIN_UP || (waitHome && el < cap && (!board_home_ready() || (signedAt > 0 && !welcome_ready())))) return; /* (signed in: the guide's pictures in too) */
  splash_end();
}

/* The ring at moment g of the timeline: strips round the circle, each with the colours of the logo at that angle. Early on every
   radius is pressed towards the rim, so a thin arc shows; it opens first round the arcs' middles (240 and 60 degrees) and last
   between them, which makes crescents of it. */
#define COLS 11 /* soft edge, hole, then each side of two fold lines with the bands between them, rim, soft edge */
static void ring_draw(float cx, float cy, float R, float g, float glow) {
  static float cs[MARK_N + 1], sn[MARK_N + 1]; static int init;
  if (!init) { init = 1; for (int i = 0; i <= MARK_N; i++) { float a = i * 2 * 3.14159265f / MARK_N; cs[i] = cosf(a); sn[i] = sinf(a); } }
  float soft = 1.2f / (gfx_scale() * R), grow = 0.04f + 0.96f * ease_apply(EASE_IN_OUT, g / 0.5f), span = 46 + 150 * ease_apply(EASE_IN_OUT, g / 0.62f), in = g / 0.08f; if (in > 1) in = 1;
  float x0[COLS], y0[COLS], hx0[3], hy0[3]; uint32_t c0[COLS], hc0[3];
  for (int i = 0; i <= MARK_N; i++) {
    int m = i % MARK_N; float th = m * 360.0f / MARK_N;
    float d1 = fabsf(fmodf(th - 240 + 540, 360) - 180), d2 = fabsf(fmodf(th - 60 + 540, 360) - 180), d = d1 < d2 ? d1 : d2;
    float p = (span - d) / 85; if (p < 0) p = 0; if (p > 1) p = 1; p = p * p * (3 - 2 * p);
    float T = (0.03f + 0.97f * p) * grow, a = p * 2.2f; if (a > 1) a = 1; a *= in; if (T > 1) T = 1;
    float hot = (1 - T) * 0.55f, h = MARK_HOLE[m] / 65535.0f, l = MARK_LOW[m] / 65535.0f, u = MARK_HIGH[m] / 65535.0f, F = MARK_FOLD_SOFT;
    float rr[COLS] = { h - soft, h, (h + l) / 2, l - F, l + F, (l + u) / 2, u - F, u + F, (u + 1) / 2, 1, 1 + soft };
    for (int j = 2; j < COLS - 1; j++) { if (rr[j] < rr[j - 1]) rr[j] = rr[j - 1]; if (rr[j] > 1) rr[j] = 1; } /* (a fold resting against the hole or the rim) */
    static const signed char src[COLS] = { 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 8 };
    float x1[COLS], y1[COLS], hx1[3], hy1[3]; uint32_t c1[COLS], hc1[3];
    for (int j = 0; j < COLS; j++) {
      const unsigned char *k = MARK_COL[m][src[j]]; float r = 1 - (1 - rr[j]) * T, al = (j == 0 || j == COLS - 1) ? 0 : a;
      x1[j] = cx + R * r * cs[i]; y1[j] = cy + R * r * sn[i];
      c1[j] = RGBA((int)(k[0] + (255 - k[0]) * hot), (int)(k[1] + (255 - k[1]) * hot), (int)(k[2] + (255 - k[2]) * hot), (int)(al * 255));
    }
    { static const float hr[3] = { 1, 1.11f, 1.36f }, ha[3] = { 0.42f, 0.13f, 0 }; const unsigned char *k = MARK_COL[m][8];
      for (int j = 0; j < 3; j++) { float q = ha[j] * glow * a * (0.4f + 0.6f * T); if (q > 1) q = 1; hx1[j] = cx + R * hr[j] * cs[i]; hy1[j] = cy + R * hr[j] * sn[i]; hc1[j] = RGBA(k[0], k[1], k[2], (int)(q * 255)); } }
    if (i) {
      for (int j = 0; j < 2; j++) { float px[4] = { hx0[j], hx1[j], hx1[j + 1], hx0[j + 1] }, py[4] = { hy0[j], hy1[j], hy1[j + 1], hy0[j + 1] }; uint32_t pc[4] = { hc0[j], hc1[j], hc1[j + 1], hc0[j + 1] }; gfx_quad_pts(px, py, pc); }
      for (int j = 0; j < COLS - 1; j++) { float px[4] = { x0[j], x1[j], x1[j + 1], x0[j + 1] }, py[4] = { y0[j], y1[j], y1[j + 1], y0[j + 1] }; uint32_t pc[4] = { c0[j], c1[j], c1[j + 1], c0[j + 1] }; gfx_quad_pts(px, py, pc); }
    }
    memcpy(x0, x1, sizeof x0); memcpy(y0, y1, sizeof y0); memcpy(c0, c1, sizeof c0); memcpy(hx0, hx1, sizeof hx0); memcpy(hy0, hy1, sizeof hy0); memcpy(hc0, hc1, sizeof hc0);
  }
}

/* a flat band round the centre, r0 to r1 */
static void band_draw(float cx, float cy, float r0, float r1, uint32_t c) {
  enum { SEG = 72 }; uint32_t pc[4] = { c, c, c, c };
  for (int i = 0; i < SEG; i++) { float a0 = i * 2 * 3.14159265f / SEG, a1 = (i + 1) * 2 * 3.14159265f / SEG, c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
    float px[4] = { cx + r0 * c0, cx + r0 * c1, cx + r1 * c1, cx + r1 * c0 }, py[4] = { cy + r0 * s0, cy + r0 * s1, cy + r1 * s1, cy + r1 * s0 }; gfx_quad_pts(px, py, pc); }
}

void splash_draw(void) {
  if (!on) return;
  { double n = app_now(); if (!gone) frames++; if (!firstDraw) firstDraw = n;
    if (!animStart && frames >= 2 && (n - lastDraw < 0.04 || n - firstDraw > 0.45)) { animStart = n; if (signedAt <= 0) sound_intro_go(); } /* the clock and the music wait for smooth frames (Home is built on this thread over the first few, 150-400 ms on the TV): otherwise the opening would be lost in that */
    if (lastDraw > 0 && !gone && animStart && animStart != n && n - lastDraw > worstGap) { worstGap = n - lastDraw; worstAt = lastDraw - animStart; } lastDraw = n; } /* (for the [start] line: the animation's own frames) */
  float x = frozenX >= 0 ? frozenX : gone ? timer_x(&out) : 0; /* 0..1 of the way out */
  if (signedAt > 0) { gfx_opacity_push(1 - ease_apply(EASE_CSS_EASE, x)); gfx_rect(0, 0, SB_W, SB_H, PAGE); login_backdrop_draw(); login_loading_draw(signedAt, 1); gfx_opacity_pop(); return; } /* right after signing in: the same loading carries on, no mark */
  float t = frozen >= 0 ? frozen : animStart > 0 ? (float)(app_now() - animStart) : 0, g = t * PACE, cx = SB_W / 2;
  float settle = ease_apply(EASE_OUT, (g - 0.62f) / 0.24f), lift = ease_apply(EASE_IN_OUT, (g - 1.08f) / 0.32f);
  float R = 250 - 10 * settle - 34 * lift, cy = SB_H / 2 - 8 - 112 * lift;
  float glow = g > 1.8f ? 1 + 0.14f * sinf((g - 1.8f) * 1.3f) : 1; /* still waiting for Home: the light breathes */
  /* the way out: the scale grows by the same factor each moment, which reads as flying at speed, not as a picture being enlarged.
     The screen around the ring stays dark, the dark in the hole clears first, so Home is what we fly into. */
  float Z = x > 0 ? powf(ZOOM, powf(x, 1.5f)) : 1, away = 1 - x / 0.2f; if (away < 0) away = 0;
  if (x <= 0) gfx_rect(0, 0, SB_W, SB_H, BG);
  else { float r = 0.86f * R * Z, h = 1 - ease_apply(EASE_OUT, x / 0.22f); if (h > 0) band_draw(cx, cy, 0, r, col_alpha(BG, h)); band_draw(cx, cy, r, 2800, BG); glow *= 1 - (x < 0.35f ? x / 0.35f : 1); } /* (the ring's own band covers the seam) */
  ring_draw(cx, cy, R * Z, g, glow);
  if (away > 0) { gfx_opacity_push(away); gfx_scale_push(Z, Z, cx, cy);
    { float k = ease_apply(EASE_OUT, (g - 0.8f) / 0.3f), S = 1.9f * R, z = 0.86f + 0.14f * k; /* the play, in the hole */
      if (k > 0) { gfx_opacity_push(k); gfx_scale_push(z, z, cx, cy); icon_draw("o_play", cx + 0.05f * R - 0.549f * S, cy + 0.027f * R - 0.461f * S, S, RGB(239, 234, 255)); gfx_scale_pop(); gfx_opacity_pop(); } }
    { float k = ease_apply(EASE_OUT, (g - 1.22f) / 0.36f), y = cy + R + 44, sp = 9 * (1 - k); /* the name comes into focus: a few copies around it, closing up */
      if (k > 0) { uint32_t c = col_mix(RGB(184, 166, 240), RGB(255, 255, 255), k);
        if (sp > 0.4f) { static const signed char DX[8] = { 10, 7, 0, -7, -10, -7, 0, 7 }, DY[8] = { 0, 7, 10, 7, 0, -7, -10, -7 }; gfx_opacity_push(k * 0.2f); for (int i = 0; i < 8; i++) text_draw("Origin", cx + DX[i] * sp / 10, y + DY[i] * sp / 10, 100, 700, c, 0, 0, ALIGN_CENTER); gfx_opacity_pop(); }
        gfx_opacity_push(k * k); text_draw("Origin", cx, y, 100, 700, c, 0, 0, ALIGN_CENTER); gfx_opacity_pop(); } }
    gfx_scale_pop(); gfx_opacity_pop(); }
}
