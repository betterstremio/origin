#include "../../src/ui/layout.h"
#include <assert.h>
#include <math.h>

static void check(int w, int h, float expectedW, float expectedH) {
  SbLayout l = layout_for_surface(w, h);
  assert(fabsf(l.w - expectedW) < 0.01f);
  assert(fabsf(l.h - expectedH) < 0.01f);
  assert(fabsf(l.w * l.scale - w) < 0.01f);
  assert(fabsf(l.h * l.scale - h) < 0.01f);
}

int main(void) {
  check(1920, 1080, 1920, 1080);
  check(1920, 1200, 1920, 1200);
  check(2000, 1333, 1920, 1279.68f);
  check(3440, 1440, 2580, 1080);
  check(1080, 1920, 1920, 3413.333f);
  check(960, 600, 1920, 1200);
  SbLayout zero = layout_for_surface(0, 0);
  assert(zero.w == 1920 && zero.h == 1080 && zero.scale == 1);
  return 0;
}
