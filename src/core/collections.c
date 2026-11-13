/* Collections: the parsing. Tolerant as Nuvio's collectionsStore.js is (GPL-3.0): what lacks an id or a title goes, and
   so does a source lacking an addon, a type or a catalog; strings too long are cut, never overrun. */
#include "core/collections.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *str(const cJSON *o, const char *k) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
  return cJSON_IsString(v) && v->valuestring[0] ? v->valuestring : NULL;
}
static const char *first(const cJSON *o, const char *const *ks) {
  for (; *ks; ks++) {
    const char *s = str(o, *ks);
    if (s)
      return s;
  }
  return NULL;
}
static int flag(const cJSON *o, const char *k) {
  return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, k));
}
static const cJSON *arr(const cJSON *o, const char *k) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
  return cJSON_IsArray(v) ? v : NULL;
}
#define CP(d, s) snprintf((d), sizeof(d), "%s", (s) ? (s) : "")

static int parse_source(const cJSON *j, ColSource *s) {
  const char *p = str(j, "provider");
  if (p && (!strcmp(p, "tmdb") || !strcmp(p, "trakt")))
    return 0;
  static const char
      *const K_BASE[] = { "addonBaseUrl", "addon_base_url", "baseUrl", "base_url", "addonUrl", "url", NULL },
             *const K_ID[] = { "addonId", "addon_id", NULL }, *const K_TY[] = { "type", "apiType", "api_type", NULL },
             *const K_CAT[] = { "catalogId", "catalog_id", NULL };
  const char *base = first(j, K_BASE), *id = first(j, K_ID), *ty = first(j, K_TY), *cat = first(j, K_CAT);
  if (!id)
    id = base;
  if (!id || !ty || !cat)
    return 0;
  CP(s->addonId, id);
  CP(s->base, base);
  CP(s->type, ty);
  CP(s->catalog, cat);
  CP(s->genre, str(j, "genre"));
  for (char *c = s->type; *c; c++)
    *c = (char)tolower((unsigned char)*c);
  return 1;
}
static int parse_folder(const cJSON *j, ColFolder *f) { /* 1 kept, 0 dropped, -1 out of memory */
  const char *id = str(j, "id"), *ti = str(j, "title"), *sh = str(j, "tileShape");
  if (!id || !ti)
    return 0;
  CP(f->id, id);
  CP(f->title, ti);
  CP(f->cover, str(j, "coverImageUrl"));
  CP(f->hero, str(j, "heroBackdropUrl"));
  CP(f->logo, str(j, "titleLogoUrl"));
  f->shape = !sh                                                         ? COL_SQUARE
             : (!strcasecmp(sh, "LANDSCAPE") || !strcasecmp(sh, "WIDE")) ? COL_LANDSCAPE
             : !strcasecmp(sh, "POSTER")                                 ? COL_POSTER
                                                                         : COL_SQUARE;
  f->hideTitle = flag(j, "hideTitle");
  const cJSON *ss = arr(j, "sources");
  if (!cJSON_GetArraySize(ss))
    ss = arr(j, "catalogSources");
  int n = cJSON_GetArraySize(ss);
  f->src = calloc(n ? (size_t)n : 1, sizeof *f->src);
  if (!f->src)
    return -1;
  const cJSON *x;
  cJSON_ArrayForEach(x, ss)
    if (cJSON_IsObject(x) && parse_source(x, &f->src[f->nsrc]))
      f->nsrc++;
  return 1;
}
int col_parse(const cJSON *root, ColSet *out) {
  memset(out, 0, sizeof *out);
  const cJSON *cs = cJSON_IsArray(root) ? root : cJSON_IsObject(root) ? arr(root, "collections") : NULL;
  if (!cs)
    return -1;
  int n = cJSON_GetArraySize(cs);
  out->c = calloc(n ? (size_t)n : 1, sizeof *out->c);
  if (!out->c)
    return -1;
  const cJSON *j;
  cJSON_ArrayForEach(j, cs) {
    if (!cJSON_IsObject(j))
      continue;
    const char *id = str(j, "id"), *ti = str(j, "title"), *vm = str(j, "viewMode");
    if (!id || !ti)
      continue;
    Collection *c = &out->c[out->n++];
    CP(c->id, id);
    CP(c->title, ti);
    c->pinTop = flag(j, "pinToTop");
    c->showAll = flag(j, "showAllTab");
    c->view = vm && !strcmp(vm, "ROWS") ? COL_ROWS : vm && !strcmp(vm, "FOLLOW_LAYOUT") ? COL_FOLLOW : COL_TABBED;
    const cJSON *fs = arr(j, "folders");
    int nf = cJSON_GetArraySize(fs);
    c->folders = calloc(nf ? (size_t)nf : 1, sizeof *c->folders);
    if (!c->folders) {
      col_free(out);
      return -1;
    }
    const cJSON *x;
    cJSON_ArrayForEach(x, fs) {
      if (!cJSON_IsObject(x))
        continue;
      int r = parse_folder(x, &c->folders[c->nfolders]);
      if (r < 0) {
        col_free(out);
        return -1;
      }
      if (r)
        c->nfolders++;
      else
        free(c->folders[c->nfolders].src), memset(&c->folders[c->nfolders], 0, sizeof *c->folders);
    }
  }
  return out->n;
}
int col_parse_text(const char *json, ColSet *out) {
  memset(out, 0, sizeof *out);
  if (!json)
    return -1;
  cJSON *r = cJSON_Parse(json);
  if (!r)
    return -1;
  int n = col_parse(r, out);
  cJSON_Delete(r);
  return n;
}
void col_free(ColSet *s) {
  if (!s)
    return;
  for (int i = 0; i < s->n; i++) {
    Collection *c = &s->c[i];
    for (int k = 0; k < c->nfolders; k++)
      free(c->folders[k].src);
    free(c->folders);
  }
  free(s->c);
  memset(s, 0, sizeof *s);
}
void col_counts(const ColSet *s, int *folders, int *sources) {
  int f = 0, n = 0;
  for (int i = 0; i < s->n; i++) {
    f += s->c[i].nfolders;
    for (int k = 0; k < s->c[i].nfolders; k++)
      n += s->c[i].folders[k].nsrc;
  }
  if (folders)
    *folders = f;
  if (sources)
    *sources = n;
}
