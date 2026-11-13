#ifndef SB_CONFIRMCARD_H
#define SB_CONFIRMCARD_H
#include "core/app.h"
extern Screen confirmCard;
typedef void (*ConfirmCb)(int yes, void *ud);
/* a question before something with a cost: OK says yes, Back says no (a click on either words, or outside the card for
 * no). The strings are copied */
void confirm_show(const char *title, const char *body, const char *yes, const char *no, ConfirmCb cb, void *ud);
#endif
