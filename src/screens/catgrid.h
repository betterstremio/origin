#ifndef SB_CATGRID_H
#define SB_CATGRID_H
#include "core/app.h"
typedef struct {
  CatalogDef def;
  char title[96];
} CatParams;
extern Screen catScreen;
#endif
