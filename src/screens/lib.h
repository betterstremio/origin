/* The Library: the focused title on top, the categories and the sort, a grid of posters. */
#ifndef SB_LIB_H
#define SB_LIB_H
#include "core/app.h"
extern Screen libScreen;
void lib_from_bar(void);
int lib_warm(void); /* off screen, after start: the first screen made ready (1 when it is) */ /* the bar hands the focus
                                                                                                 down: the categories */
/* phones (screens/phone_lib.c) */
void lib_ph_refresh(void);
int lib_ph_count(void);
const Item *lib_ph_item(int i);
int lib_ph_filter(void);
void lib_ph_set_filter(int f);
int lib_ph_filters(const char **labels, int *counts, int max);
int lib_ph_sort(void);
void lib_ph_next_sort(void);
const char *lib_ph_sort_label(void);
#endif
