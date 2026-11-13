/* Collections: a user's imported Nuvio-style collections (a row of tiles on Home; a tile opens a page of catalogs).
   The format and its rules follow Nuvio's collectionsStore.js. No SDL, GL or network here: tests build this alone. */
#ifndef SB_COLLECTIONS_H
#define SB_COLLECTIONS_H
#include "cJSON.h"
enum { COL_SQUARE = 0, COL_LANDSCAPE, COL_POSTER };
enum { COL_ROWS = 0, COL_FOLLOW, COL_TABBED };
typedef struct {
  char addonId[96], base[256], type[16], catalog[160], genre[64];
} ColSource;
typedef struct {
  char id[64], title[96], cover[512], hero[512], logo[512];
  int shape, hideTitle;
  ColSource *src;
  int nsrc;
} ColFolder;
typedef struct {
  char id[64], title[96];
  int pinTop, view, showAll;
  ColFolder *folders;
  int nfolders;
} Collection;
typedef struct {
  Collection *c;
  int n;
} ColSet;

int col_parse(const cJSON *root, ColSet *out); /* an array, or an object holding "collections"; the number kept, -1 when
                                                  it is neither (out stays empty) */
int col_parse_text(const char *json, ColSet *out); /* the same from text: -1 when it is not JSON */
void col_free(ColSet *s);
void col_counts(const ColSet *s, int *folders, int *sources);
#endif
