/* The focused card's trailer, played: bgtrailer.c decides when, this plays it through the video backend behind the
   expanded card, whose rectangle the board reports and makes see-through. */
#ifndef SB_BTRAILER_H
#define SB_BTRAILER_H
void btr_init(void);
void btr_tick(float dt); /* once a frame, from the app */
void btr_key(int arrow); /* any key or pointer action (arrow: a move to another title) */
void btr_release(void); /* the player is about to load: the decoder is let go now */
void btr_card_rect(float x, float y, float w,
                   float h); /* the expanded card, in screen units, once a frame while it is fully open */
float btr_card_alpha(void); /* how far the card has opened onto the video, 0..1 */
#endif
