/* The profile menu: a card out of the bar's picture with the profiles, Settings, Share logs, Exit (3.0.0 header). */
#ifndef SB_PROFPANEL_H
#define SB_PROFPANEL_H
int profpanel_open(void); /* 0 when it cannot open (signed out, another overlay up) */
int profpanel_is_open(void);
float profpanel_k(void); /* 0 closed .. 1 open */
#endif
