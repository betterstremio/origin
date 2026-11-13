/* tests/collections/run.sh [export.json c f s] : the collections parser alone; a hand-written fixture, and optionally a
 * real export's counts */
#include "core/collections.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int bad;
#define CK(c)                                       \
  do {                                              \
    if (!(c)) {                                     \
      printf("line %d failed: %s\n", __LINE__, #c); \
      bad++;                                        \
    }                                               \
  } while (0)
static const char *FX =
    "[{\"id\":\"c1\",\"title\":\"Shelf\",\"pinToTop\":true,\"viewMode\":\"ROWS\",\"showAllTab\":true,\"folders\":[{\"id\":\"f1\",\"title\":\"Paper\",\"tileShape\":\"WIDE\",\"hideTitle\":true,"
    "\"coverImageUrl\":\"http://x/c.webp\",\"heroBackdropUrl\":\"http://x/h.webp\",\"titleLogoUrl\":\"http://x/l.webp\",\"sources\":[{\"provider\":\"addon\",\"addonId\":\"a\",\"type\":\"Movies\",\"catalogId\":\"k.1\",\"genre\":\"None\"},{\"provider\":\"tmdb\",\"tmdbId\":5}]}]}]";
static int count(const char *t, int *f, int *s) {
  ColSet c;
  int n = col_parse_text(t, &c);
  col_counts(&c, f, s);
  col_free(&c);
  return n;
}

int main(int argc, char **argv) {
  ColSet s;
  int f, n;
  CK(col_parse_text(FX, &s) == 1);
  {
    Collection *c = &s.c[0];
    ColFolder *d = &c->folders[0];
    CK(c->pinTop && c->showAll && c->view == COL_ROWS && c->nfolders == 1);
    CK(d->shape == COL_LANDSCAPE && d->hideTitle && !strcmp(d->cover, "http://x/c.webp") &&
       !strcmp(d->hero, "http://x/h.webp") && !strcmp(d->logo, "http://x/l.webp"));
    CK(d->nsrc == 1 && !strcmp(d->src[0].type, "movies") && !strcmp(d->src[0].genre, "None") &&
       !strcmp(d->src[0].catalog, "k.1") && !strcmp(d->src[0].addonId, "a"));
  }
  col_free(&s);
  col_free(&s);
  CK(s.n == 0 && s.c == NULL);
  CK(count("{\"collections\":[{\"id\":\"a\",\"title\":\"b\"}]}", &f, &n) == 1 && f == 0);
  CK(count(
         "[{\"id\":\"a\",\"title\":\"b\",\"folders\":[{\"id\":\"f\",\"title\":\"t\",\"sources\":[],\"catalogSources\":[{\"addonId\":\"x\",\"type\":\"movie\",\"catalogId\":\"y\"}]}]}]",
         &f, &n) == 1 &&
     f == 1 && n == 1);
  CK(col_parse_text(
         "[{\"id\":\"a\",\"title\":\"b\",\"folders\":[{\"id\":\"f\",\"title\":\"t\",\"sources\":[{\"addon_id\":\"x\",\"api_type\":\"SERIES\",\"catalog_id\":\"y\"},{\"url\":\"http://u\",\"type\":\"movie\",\"catalogId\":\"z\"},{\"addonId\":\"x\",\"type\":\"movie\"}]}]}]",
         &s) == 1);
  {
    ColFolder *d = &s.c[0].folders[0];
    CK(d->nsrc == 2 && !strcmp(d->src[0].addonId, "x") && !strcmp(d->src[0].type, "series") &&
       !strcmp(d->src[1].addonId, "http://u") && !strcmp(d->src[1].base, "http://u"));
  }
  col_free(&s);
  CK(count(
         "[{\"id\":\"a\",\"title\":\"b\",\"folders\":[{\"title\":\"no id\"},{\"id\":\"f\",\"title\":\"t\"}]},{\"id\":\"only\"}]",
         &f, &n) == 1 &&
     f == 1 && n == 0);
  CK(col_parse_text(
         "[{\"id\":\"a\",\"title\":\"b\",\"viewMode\":\"ODD\",\"folders\":[{\"id\":\"f\",\"title\":\"t\",\"tileShape\":\"ODD\"},{\"id\":\"g\",\"title\":\"u\",\"tileShape\":\"poster\"}]}]",
         &s) == 1);
  CK(s.c[0].view == COL_TABBED && s.c[0].folders[0].shape == COL_SQUARE && s.c[0].folders[1].shape == COL_POSTER);
  col_free(&s);
  CK(count("[]", &f, &n) == 0);
  CK(col_parse_text("{}", &s) == -1);
  CK(col_parse_text("5", &s) == -1);
  CK(col_parse_text("[{", &s) == -1);
  CK(col_parse_text(NULL, &s) == -1);
  CK(col_parse(NULL, &s) == -1);
  CK(count(
         "[{\"id\":5,\"title\":\"b\"},7,\"x\",null,{\"id\":\"a\",\"title\":\"b\",\"folders\":\"no\"},{\"id\":\"c\",\"title\":\"d\",\"folders\":[3,{\"id\":\"f\",\"title\":\"t\",\"sources\":9,\"tileShape\":4}]}]",
         &f, &n) == 2 &&
     f == 1 && n == 0);
  {
    size_t L = 5000;
    char *lng = malloc(L + 1);
    memset(lng, 'x', L);
    lng[L] = 0;
    char *t = malloc(3 * L + 300);
    snprintf(
        t, 3 * L + 300,
        "[{\"id\":\"a\",\"title\":\"%s\",\"folders\":[{\"id\":\"%s\",\"title\":\"t\",\"sources\":[{\"addonId\":\"x\",\"type\":\"m\",\"catalogId\":\"%s\"}]}]}]",
        lng, lng, lng);
    CK(col_parse_text(t, &s) == 1);
    CK(strlen(s.c[0].title) == sizeof s.c[0].title - 1 &&
       strlen(s.c[0].folders[0].id) == sizeof s.c[0].folders[0].id - 1 &&
       strlen(s.c[0].folders[0].src[0].catalog) == sizeof s.c[0].folders[0].src[0].catalog - 1);
    col_free(&s);
    free(lng);
    free(t);
  }
  if (argc > 1) {
    FILE *h = fopen(argv[1], "rb");
    CK(h != NULL);
    if (h) {
      fseek(h, 0, SEEK_END);
      long z = ftell(h);
      rewind(h);
      char *t = malloc((size_t)z + 1);
      size_t g = fread(t, 1, (size_t)z, h);
      t[g] = 0;
      fclose(h);
      int c = count(t, &f, &n);
      printf("%d %d %d\n", c, f, n);
      if (argc > 4)
        CK(c == atoi(argv[2]) && f == atoi(argv[3]) && n == atoi(argv[4]));
      free(t);
    }
  }
  if (!bad)
    printf("ok\n");
  return bad != 0;
}
