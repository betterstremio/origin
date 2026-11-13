/* Interface sounds: short soft taps as you move around (Settings > Audio > Interface sounds). */
#ifndef SB_SOUND_H
#define SB_SOUND_H
enum { SND_FOCUS, SND_TAB, SND_FILTER, SND_TOGGLE, SND_HOLD, SND_PLAY, SND_START, SND_N };
void sound_play(int which); /* cheap: the work is on the sound thread; a sound asked twice within 60 ms plays once */
void sound_focus(void); /* sound_play(SND_FOCUS) */
double sound_last(void); /* when a sound was last asked for (app_now's clock), so a general one can step aside */
int sound_has_intro(void); /* this package has the start-up music (2.1.3 does not) */
void sound_intro(void); /* the start-up music, once (Settings > Audio > Startup sound): gets it ready */
void sound_intro_go(void); /* ... and starts it, on the animation's first frame */
void sound_quiet(int on); /* 1 while the player is up: what is playing finishes, then the device is let go */
#endif
