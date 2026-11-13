/* What the app runs on, as the screens need to know it: a TV (a remote, 10 feet away), or a phone or tablet (fingers,
   held in the hand). Read once, from SB_FORM (Android's activity sets it; SB_FORM=phone on the desktop preview).
   Every TV path stays as it was: form_touch() is 0 there and costs a load. */
#ifndef SB_FORM_H
#define SB_FORM_H
int form_touch(void); /* a phone or a tablet: touch screens, portrait, dp layout */
int form_tablet(void); /* a tablet (smallest side 600 dp or more) */
float form_density(void); /* surface pixels per dp (Android's DisplayMetrics.density; SB_DENSITY on the desktop) */
void form_set_density(float d);
/* the parts of the screen the system draws over (status bar, navigation bar, a camera cutout), in layout units of the
   current canvas; asked of the platform at most every few frames */
void form_safe(float *top, float *bottom, float *left, float *right);
void form_safe_refresh(void); /* ask again now (after a resize) */
#endif
