/* "Who's watching?": a Nuvio account's profiles and a locked profile's PIN, over whatever is on screen. */
#ifndef SB_PROFILES_H
#define SB_PROFILES_H
enum {
  PF_START,
  PF_SIGNIN,
  PF_SWITCH
}; /* at launch (Home ready behind it), right after signing in, from the top bar or Settings */
void profiles_open(int mode);
int profiles_is_open(void);
void profiles_at_start(void); /* at launch: asks if the setting says so; a locked last profile always asks */
int profile_avatar_draw(const char *av, float x, float y,
                        float D); /* a profile's picture in a circle of D; 0 when not (yet) there */
int profile_avatar_draw_r(const char *av, float x, float y, float D, float r); /* the same, corners of radius r */
/* the profile menu's way in: switch to the profile at list position i (no PIN), or its PIN pad; back() runs when Back
   leaves the pad (the menu takes the keys again) */
int profiles_can_add(void);
void profiles_switch(int i);
void profiles_open_pin(int i, void (*back)(void));
#endif
