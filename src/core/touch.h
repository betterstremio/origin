/* Fingers on a phone or a tablet (core/form.h). One finger at a time, read as:
     a tap        it lifts within 8 dp of where it landed, before a long press: the target's act (core/hit.h), or on
                  pages still drawn for a TV, the pointer's click (focus + OK)
     a long press it stays put for 0.45 s: the target's hold, with a buzz (on TV pages: the card's menu)
     a drag       it moves 8 dp: along the axis it moved most, the innermost scroller under it that scrolls that way
                  follows the finger; lifted while moving, it flings on and slows down (a rubber band past the ends)
   Screens register where their scrollers are while they draw (touch_region), like their hit rectangles. A finger
   that lands on a moving scroller stops it, and that landing is no tap. */
#ifndef SB_TOUCH_H
#define SB_TOUCH_H
#include "core/hit.h"

/* a scrolled offset: 0 at the start, max at the end (content moved up/left by off) */
typedef struct {
  float off, vel, min, max, from, settleTo;
  int drag, settling;
  float settleT;
} Scroller;
void scroller_limits(Scroller *s, float min, float max); /* each frame, from the content's size (max < min: min) */
void scroller_tick(Scroller *s, float dt); /* the fling and the spring back: each frame it is drawn */
int scroller_moving(const Scroller *s);
void scroller_to(Scroller *s, float off); /* glides there (0.3 s) */
void scroller_set(Scroller *s, float off); /* there at once */

void touch_begin_frame(void); /* with hit_begin: this frame's regions replace the last */
void touch_region(float x, float y, float w, float h, Scroller *sx,
                  Scroller *sy); /* layout units, through the transform */

void touch_down(float x, float y); /* layout units */
void touch_move(float x, float y);
void touch_up(float x, float y);
void touch_cancel(void);
void touch_tick(void); /* the long press's clock (from app_tick) */
int touch_busy(void); /* a finger is down, or a scroller moves: frames must be drawn */
void touch_note_moving(void); /* a scroller drawn this frame moves */
/* the target a resting finger is on (for its pressed look), 0 when none or once the finger has moved off */
int touch_pressed(HitFn focus, HitFn act, int a, int b);
void touch_hold_rect(float *x, float *y, float *w,
                     float *h); /* where the target of the long press being handled sits */
#endif
