#ifndef SB_LAYOUT_H
#define SB_LAYOUT_H

typedef struct {
  float w, h, scale;
} SbLayout;

/* Preserve the 16:9 design's minimum space, adding room on the longer axis. */
static inline SbLayout layout_for_surface(int w, int h) {
  SbLayout l = { 1920.0f, 1080.0f, 1.0f };
  if (w <= 0 || h <= 0)
    return l;
  float sx = w / 1920.0f, sy = h / 1080.0f;
  l.scale = sx < sy ? sx : sy;
  l.w = w / l.scale;
  l.h = h / l.scale;
  /* a 16:9 surface gives the design's own size exactly (1280 / (1280 / 1920) is not always 1920 in floats) */
  if (l.w > 1919.5f && l.w < 1920.5f)
    l.w = 1920.0f;
  if (l.h > 1079.5f && l.h < 1080.5f)
    l.h = 1080.0f;
  return l;
}

#endif
