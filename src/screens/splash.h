/* The start screen: the name, its O drawn as a ring, on Home's page colour, until Home is complete. */
#ifndef SB_SPLASH_H
#define SB_SPLASH_H
#include <stdint.h>
void splash_start(int waitForHome,
                  double signedInAt); /* signedInAt: right after signing in (when its loading began), 0 at a start */
void splash_tick(float dt);
void splash_draw(void);
int splash_up(void); /* the start screen is showing (or leaving) */
int splash_signed_in(void); /* ... as the loading right after signing in */
/* the name as the start screen draws it (its O a drawn ring), its left edge at x and the O's top at y, px the font
   size; returns its width. For the sign-in and other first screens */
float splash_wordmark_draw(float x, float y, int px, uint32_t color);
float splash_wordmark_width(int px);
#endif
