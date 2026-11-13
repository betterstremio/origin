#include "core/form.h"
#include "platform/plat.h"
#include "ui/gfx.h"
#include <stdlib.h>
#include <string.h>

static int formRead, touch, tablet;
static float density;
static void form_read(void) {
  if (formRead)
    return;
  formRead = 1;
  const char *f = getenv("SB_FORM");
  touch = f && (!strcmp(f, "phone") || !strcmp(f, "tablet"));
  tablet = f && !strcmp(f, "tablet");
  const char *d = getenv("SB_DENSITY");
  density = d ? (float)atof(d) : 0;
}
int form_touch(void) {
  form_read();
  return touch;
}
int form_tablet(void) {
  form_read();
  return tablet;
}
float form_density(void) {
  form_read();
  return density > 0.3f ? density : 1.0f;
}
void form_set_density(float d) {
  form_read();
  if (d > 0.3f && !getenv("SB_DENSITY"))
    density = d;
}

static int safePx[4], safeAsked;
static unsigned safeFrame;
void form_safe_refresh(void) {
  safeAsked = 0;
}
void form_safe(float *top, float *bottom, float *left, float *right) {
  unsigned fr = gfx_frame();
  if (!safeAsked || fr - safeFrame > 30) {
    safeAsked = 1;
    safeFrame = fr;
    plat_safe_area(&safePx[0], &safePx[1], &safePx[2], &safePx[3]);
  }
  float s = gfx_scale();
  if (s <= 0)
    s = 1;
  *top = safePx[0] / s;
  *bottom = safePx[1] / s;
  *left = safePx[2] / s;
  *right = safePx[3] / s;
}
