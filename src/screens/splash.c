#include "screens/splash.h"
#include "screens/board.h"
#include "core/app.h"
#include "ui/gfx.h"
#include "ui/text.h"
#include "platform/plat.h"
#include "screens/login.h"
#include "screens/welcome.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* The start screen, on Home's own page colour: the O of the name draws itself once round, like a loading ring, then
   moves aside as "rigin" slides out from behind it. The word holds, still, until Home is complete; then the camera
   flies through the O's hole and Home opens up inside it. No glow, no picture, no colour: the white of the app's
   focus on the grey of its page. All of it is drawn: nothing to load. */
static int on, waitHome, gone, frames;
static double t0, cap, signedAt, firstDraw, lastDraw, worstGap, worstAt, animStart;
static Timer1 out;
static float frozen = -1, frozenX = -1;
#define MIN_UP 0.9 /* the entrance fits inside this, so the app starts no later than it did */
#define BG PAGE
#define EXIT 0.55f /* the flight through the O into Home; Home is live under it from its first frame */
#define ZOOM 36.0f /* how far: the hole ends wider than the screen's diagonal */
#define WORD_PX 168
/* the O of Onest Bold, measured (em): a circle 0.712 across, stroke 0.14, its centre 0.354 above the baseline, its
   left edge 0.036 past the pen, "rigin" starting at its advance 0.773 */
#define O_R 0.356f
#define O_IN 0.216f
#define O_MID 0.354f
#define O_LSB 0.036f
#define O_ADV 0.773f
void splash_start(int waitForHome, double signedInAt) {
  int justSignedIn = signedInAt > 0;
  signedAt = signedInAt;
  on = 1;
  gone = 0;
  frames = 0;
  animStart = 0;
  firstDraw = lastDraw = worstGap = worstAt = 0;
  waitHome = waitForHome;
  t0 = justSignedIn ? app_now() - 2 : app_now();
  cap = justSignedIn ? 9.0 : 2.5; /* the restart after signing in: already up, no second entrance */
  {
    const char *f = getenv("SB_SPLASH_T");
    frozen = f ? (float)atof(f) : -1;
    f = getenv("SB_SPLASH_X");
    frozenX = f ? (float)atof(f) : -1;
  } /* development: held at this moment, for pictures of it */
}
static void splash_end(void) {
  if (gone)
    return;
  gone = 1;
  timer_start(&out, signedAt > 0 ? 0.36f : EXIT);
  app_splash_done = 1;
  fprintf(
      stderr,
      "[start] ready in %.0f ms, first frame at %.0f ms, %d frames, worst gap %.0f ms at %.0f ms of the animation\n",
      (app_now() - t0) * 1000, (animStart - t0) * 1000, frames, worstGap * 1000, worstAt * 1000);
  if (waitHome)
    app_reveal_start();
  if (signedAt > 0)
    welcome_open_now(); /* right after signing in: the guide comes straight from the loading (Home revealed under it) */
}
int splash_up(void) {
  return on;
}
int splash_signed_in(void) {
  return on && signedAt > 0;
}
void splash_tick(float dt) {
  if (!on || frozen >= 0)
    return;
  if (gone) {
    if (timer_tick(&out, dt) >= 1 && !out.on)
      on = 0;
    return;
  }
  double el = app_now() - t0;
  if (signedAt <= 0 && el < cap && (!animStart || app_now() - animStart < MIN_UP))
    return; /* the animation plays out, counted from the frame it first appeared on */
  if (el < MIN_UP || (waitHome && el < cap && (!board_home_ready() || (signedAt > 0 && !welcome_ready()))))
    return; /* (signed in: the guide's pictures in too) */
  splash_end();
}


/* part of a ring, a0 to a1 (radians, 0 = right, clockwise on screen), r0 to r1, with a soft edge of one surface
   pixel inside and out (no multisampling on these GPUs) */
static void arc_draw(float cx, float cy, float r0, float r1, float a0, float a1, uint32_t c) {
  if (a1 <= a0 || r1 <= r0)
    return;
  float f = 1.2f / gfx_scale(), span = a1 - a0;
  int n = (int)(span / (2 * 3.14159265f) * 128) + 2;
  float rr[4] = { r0 - f > 0 ? r0 - f : 0, r0, r1, r1 + f };
  uint32_t cc[4] = { col_alpha(c, 0), c, c, col_alpha(c, 0) };
  float pc = cosf(a0), ps = sinf(a0);
  for (int i = 1; i <= n; i++) {
    float a = a0 + span * i / n, nc = cosf(a), ns = sinf(a);
    for (int j = 0; j < 3; j++) {
      float px[4] = { cx + rr[j] * pc, cx + rr[j] * nc, cx + rr[j + 1] * nc, cx + rr[j + 1] * pc },
            py[4] = { cy + rr[j] * ps, cy + rr[j] * ns, cy + rr[j + 1] * ns, cy + rr[j + 1] * ps };
      uint32_t q[4] = { cc[j], cc[j], cc[j + 1], cc[j + 1] };
      gfx_quad_pts(px, py, q);
    }
    pc = nc;
    ps = ns;
  }
}

/* the word: its O (centre ox, oy) drawn `draw` of the way round from the top, "rigin" shown `rev` of the way out from
   behind it. Returns the word's width */
static float word_draw(float ox, float oy, int px, float draw, float rev, uint32_t c) {
  float R = O_R * px, Ri = O_IN * px, top = -1.5707963f;
  float wr = text_width("rigin", px, 700, 0), x = ox - R + (O_ADV - O_LSB) * px;
  if (rev > 0) {
    const SbLine *l = text_line("rigin", px, 700, 0, 0);
    if (l) {
      float y = oy + O_MID * px - l->baseline;
      gfx_clip_push(ox, y - px, wr + 3 * px, 3 * px);
      gfx_opacity_push(rev);
      text_draw("rigin", x - (1 - rev) * 0.55f * wr, y, px, 700, c, 0, 0, ALIGN_LEFT);
      gfx_opacity_pop();
      gfx_clip_pop();
    }
    if (rev < 1)
      gfx_circle(ox, oy, R - 0.5f, BG | 0xff000000u);
  } /* the letters come out from behind the O: its disc hides them until they pass its rim */
  if (draw >= 1)
    arc_draw(ox, oy, Ri, R, 0, 2 * 3.14159265f, c);
  else if (draw > 0) {
    float e = top + draw * 2 * 3.14159265f, m = (R + Ri) / 2, h = (R - Ri) / 2;
    arc_draw(ox, oy, Ri, R, top, e, c);
    gfx_circle(ox + m * cosf(top), oy + m * sinf(top), h, c);
    gfx_circle(ox + m * cosf(e), oy + m * sinf(e), h, c);
  } /* (round ends while it draws; they meet when it closes) */
  return x + wr - (ox - R);
}
float splash_wordmark_draw(float x, float y, int px, uint32_t c) {
  float R = O_R * px;
  return word_draw(x + R, y + R, px, 1, 1, c);
}
float splash_wordmark_width(int px) {
  return (O_ADV - O_LSB) * px + text_width("rigin", px, 700, 0);
}

/* a flat band round a centre, r0 to r1 */
static void band_draw(float cx, float cy, float r0, float r1, uint32_t c) {
  arc_draw(cx, cy, r0, r1, 0, 2 * 3.14159265f, c);
}

void splash_draw(void) {
  if (!on)
    return;
  {
    double n = app_now();
    if (!gone)
      frames++;
    if (!firstDraw)
      firstDraw = n;
    if (!animStart && frames >= 2 && (n - lastDraw < 0.04 || n - firstDraw > 0.45)) {
      animStart = n;
    } /* the clock waits for smooth frames (Home is built on this thread over the first few, 150-400 ms on
         the TV): otherwise the opening would be lost in that */
    if (lastDraw > 0 && !gone && animStart && animStart != n && n - lastDraw > worstGap) {
      worstGap = n - lastDraw;
      worstAt = lastDraw - animStart;
    }
    lastDraw = n;
  } /* (for the [start] line: the animation's own frames) */
  float x = frozenX >= 0 ? frozenX : gone ? timer_x(&out) : 0; /* 0..1 of the way out */
  if (signedAt > 0) {
    gfx_opacity_push(1 - ease_apply(EASE_CSS_EASE, x));
    gfx_rect(0, 0, SB_W, SB_H, PAGE);
    login_backdrop_draw();
    login_loading_draw(signedAt, 1);
    gfx_opacity_pop();
    return;
  } /* right after signing in: the same loading carries on, no mark */
  float t = frozen >= 0 ? frozen : animStart > 0 ? (float)(app_now() - animStart) : 0;
  int px = WORD_PX;
  float R = O_R * px, Ri = O_IN * px, W = splash_wordmark_width(px), cy = SB_H / 2;
  /* 1. the O draws itself round, at the centre of the screen (0 to 0.42 s) */
  float draw = ease_apply(EASE_OUT, t / 0.42f);
  /* 2. it moves to its place in the word and "rigin" slides out from behind it (0.3 to 0.78 s) */
  float k = ease_apply(EASE_SNAP, (t - 0.30f) / 0.48f);
  float ox = SB_W / 2 + k * (SB_W / 2 - W / 2 + R - SB_W / 2), oy = cy;
  if (x <= 0) {
    gfx_rect(0, 0, SB_W, SB_H, BG);
    word_draw(ox, oy, px, draw, k, WHITE_A(255));
    return;
  }
  /* 3. the way out: the scale grows by the same factor each moment, which reads as flying, not as a picture being
     enlarged; the O comes to the middle of the screen as we go. Around the ring stays dark, the ring itself darkens
     into it, and the dark in the hole clears first, so Home is what we fly into */
  float Z = powf(ZOOM, powf(x, 1.5f)), m = ease_apply(EASE_IN_OUT, x), fx = ox + (SB_W / 2 - ox) * m,
        fy = oy + (SB_H / 2 - oy) * m, hole = 1 - ease_apply(EASE_OUT, x / 0.22f),
        dim = ease_apply(EASE_IN_OUT, (x - 0.1f) / 0.45f);
  if (hole > 0)
    band_draw(fx, fy, 0, Ri * Z, col_alpha(BG | 0xff000000u, hole));
  band_draw(fx, fy, R * Z - 0.5f, 3000, BG | 0xff000000u);
  band_draw(fx, fy, Ri * Z, R * Z, col_mix(WHITE_A(255), BG | 0xff000000u, dim));
  {
    float a = 1 - x / 0.16f;
    if (a > 0) {
      gfx_opacity_push(a);
      gfx_scale_push(Z, Z, fx, fy);
      gfx_offset_push(fx - ox, fy - oy);
      const SbLine *l = text_line("rigin", px, 700, 0, 0);
      if (l)
        text_draw("rigin", ox - R + (O_ADV - O_LSB) * px, oy + O_MID * px - l->baseline, px, 700, WHITE_A(255), 0, 0,
                  ALIGN_LEFT);
      gfx_offset_pop();
      gfx_scale_pop();
      gfx_opacity_pop();
    }
  } /* (the rest of the word goes first, with the camera) */
}
