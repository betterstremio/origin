/* The collections this TV has imported: kept in their own file (a large export never slows the settings file's saves).
 */
#ifndef SB_COLSTORE_H
#define SB_COLSTORE_H
#include "core/collections.h"
const ColSet *colstore(void); /* loaded on first use; empty when nothing was imported */
int colstore_import(const char *json); /* the number of collections kept; -1 (and what was there stays) when it is not
                                          an export, or holds none */
void colstore_clear(void);
#endif
