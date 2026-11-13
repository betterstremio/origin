#include "core/colstore.h"
#include "platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* a set replaced stays allocated until the next one: rows on screen may still point into it until they rebuild */
static ColSet cur, old;
static int loaded;
static void file_path(char *out, size_t cap) {
  snprintf(out, cap, "%s/collections.json", plat_data_dir());
}
const ColSet *colstore(void) {
  if (!loaded) {
    loaded = 1;
    char p[1200];
    file_path(p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (f) {
      fseek(f, 0, SEEK_END);
      long n = ftell(f);
      fseek(f, 0, SEEK_SET);
      char *t = n > 0 ? malloc((size_t)n + 1) : NULL;
      if (t) {
        size_t g = fread(t, 1, (size_t)n, f);
        t[g] = 0;
        if (col_parse_text(t, &cur) < 0)
          memset(&cur, 0, sizeof cur);
        free(t);
      }
      fclose(f);
    }
  }
  return &cur;
}
int colstore_import(const char *json) {
  ColSet s;
  int n = col_parse_text(json, &s);
  if (n <= 0) {
    col_free(&s);
    return -1;
  }
  char p[1200], tmp[1240];
  file_path(p, sizeof p);
  snprintf(tmp, sizeof tmp, "%s.tmp", p);
  FILE *f = fopen(tmp, "wb");
  size_t len = strlen(json);
  if (!f || fwrite(json, 1, len, f) != len) {
    if (f)
      fclose(f);
    remove(tmp);
    col_free(&s);
    return -1;
  }
  fclose(f);
  if (rename(tmp, p)) {
    remove(tmp);
    col_free(&s);
    return -1;
  }
  colstore();
  col_free(&old);
  old = cur;
  cur = s;
  return n;
}
void colstore_clear(void) {
  colstore();
  char p[1200];
  file_path(p, sizeof p);
  remove(p);
  col_free(&old);
  old = cur;
  memset(&cur, 0, sizeof cur);
}
