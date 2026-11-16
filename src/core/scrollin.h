/* SB_SCROLL=<profile> / SB_INPUT="ms:key:down|up|rep,...": keys on a wall-clock timeline, sent through app_key as real
   key events are (held keys repeat with repeat=1). Profiles: H-V H-R T150 T250 B5 RT CTRL. SB_SCROLL_N = rows (default 8).
   On webOS pass it through the launch params (scrollin_launch_params); Android: TODO. */
#ifndef SB_SCROLLIN_H
#define SB_SCROLLIN_H
void scrollin_launch_params(int argc, char **argv); /* webOS: argv[1] JSON "sbenv", before anything reads getenv */
void scrollin_init(void);
void scrollin_tick(void); /* once per frame, after fill_tick */
int scrollin_on(void);
#endif
