/* The profile panel: profiles, Add profile, Settings, Share logs, Exit, out of the left edge (3.0.0 header). */
#ifndef SB_PROFPANEL_H
#define SB_PROFPANEL_H
int profpanel_open(void); /* 0 when it cannot open (signed out, another overlay up) */
int profpanel_is_open(void);
#endif
