/* See nuvio_merge.h. Every lookup goes through a small hash map: a library of a few thousand titles against a few
   thousand rows must stay linear on a TV's CPU. */
#include "core/nuvio_merge.h"
#include "core/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EP_PREFIX "__nuvio_episode__:"

/* ---- times ----------------------------------------------------------------------------------- */
static double iso_ms(const char *s) { /* "2026-11-06T02:08:43.43+00:00" or "...Z": the UTC part, to the second */
  struct tm tm;
  memset(&tm, 0, sizeof tm);
  if (!s ||
      sscanf(s, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) < 3)
    return 0;
  tm.tm_year -= 1900;
  tm.tm_mon--;
  return (double)timegm(&tm) * 1000.0;
}
void nm_ms_iso(double ms, char *out, size_t cap) {
  time_t t = (time_t)(ms / 1000);
  struct tm tm;
  gmtime_r(&t, &tm);
  strftime(out, cap, "%Y-%m-%dT%H:%M:%S.000Z", &tm);
}
double nm_row_ms(const cJSON *v) {
  if (cJSON_IsNumber(v))
    return v->valuedouble > 1e12 ? v->valuedouble : v->valuedouble * 1000.0;
  if (cJSON_IsString(v) && v->valuestring) {
    char *e;
    double d = strtod(v->valuestring, &e);
    if (e != v->valuestring && !*e)
      return d > 1e12 ? d : d * 1000.0;
    return iso_ms(v->valuestring);
  }
  return 0;
}
static const cJSON *fld(const cJSON *o, const char *k) {
  return cJSON_GetObjectItemCaseSensitive(o, k);
}
static double t_of(const cJSON *o, const char *k) {
  return nm_row_ms(fld(o, k));
}

/* ---- a string -> pointer map (the keys belong to the trees they came from) ---------------------- */
typedef struct {
  const char **k;
  void **v;
  unsigned cap, n;
} Map;
static unsigned hs(const char *s) {
  unsigned h = 2166136261u;
  while (*s)
    h = (h ^ (unsigned char)*s++) * 16777619u;
  return h;
}
static void map_init(Map *m, unsigned want) {
  unsigned c = 64;
  while (c < want * 2)
    c <<= 1;
  m->cap = c;
  m->n = 0;
  m->k = calloc(c, sizeof *m->k);
  m->v = calloc(c, sizeof *m->v);
}
static void map_free(Map *m) {
  free(m->k);
  free(m->v);
  memset(m, 0, sizeof *m);
}
static void map_grow(Map *m);
static void *map_get(const Map *m, const char *k) {
  if (!m->cap || !k)
    return NULL;
  unsigned i = hs(k) & (m->cap - 1);
  while (m->k[i]) {
    if (!strcmp(m->k[i], k))
      return m->v[i];
    i = (i + 1) & (m->cap - 1);
  }
  return NULL;
}
static void map_put(Map *m, const char *k, void *v) {
  if (!m->cap || !k)
    return;
  if ((m->n + 1) * 2 > m->cap)
    map_grow(m);
  unsigned i = hs(k) & (m->cap - 1);
  while (m->k[i]) {
    if (!strcmp(m->k[i], k)) {
      m->k[i] = k;
      m->v[i] = v;
      return;
    }
    i = (i + 1) & (m->cap - 1);
  }
  m->k[i] = k;
  m->v[i] = v;
  m->n++;
}
static void map_grow(Map *m) {
  Map o = *m;
  m->cap = o.cap * 2;
  m->n = 0;
  m->k = calloc(m->cap, sizeof *m->k);
  m->v = calloc(m->cap, sizeof *m->v);
  for (unsigned i = 0; i < o.cap; i++)
    if (o.k[i]) {
      unsigned j = hs(o.k[i]) & (m->cap - 1);
      while (m->k[j])
        j = (j + 1) & (m->cap - 1);
      m->k[j] = o.k[i];
      m->v[j] = o.v[i];
      m->n++;
    }
  free(o.k);
  free(o.v);
}
static void set_of(Map *m, const cJSON *arr) {
  const cJSON *e;
  if (cJSON_IsArray(arr))
    cJSON_ArrayForEach(e, arr)
      if (cJSON_IsString(e) && e->valuestring)
        map_put(m, e->valuestring, (void *)e);
}

/* ---- item helpers ---------------------------------------------------------------------------- */
typedef struct {
  const NmIn *in;
  NmOut *out;
  Map items, chg;
  int cap;
} Cx;
static void mark(Cx *c, cJSON *it) {
  const char *id = js_str(it, "_id", NULL);
  if (!id || map_get(&c->chg, id))
    return;
  map_put(&c->chg, id, it);
  if (c->out->nChanged == c->cap) {
    int nc = c->cap ? c->cap * 2 : 256;
    cJSON **n = realloc(c->out->changed, sizeof *n * (size_t)nc);
    if (!n)
      return;
    c->out->changed = n;
    c->cap = nc;
  }
  c->out->changed[c->out->nChanged++] = it;
}
static void set_s(cJSON *o, const char *k, const char *v) {
  cJSON_DeleteItemFromObjectCaseSensitive(o, k);
  cJSON_AddStringToObject(o, k, v ? v : "");
}
static void set_n(cJSON *o, const char *k, double v) {
  cJSON_DeleteItemFromObjectCaseSensitive(o, k);
  cJSON_AddNumberToObject(o, k, v);
}
static void set_b(cJSON *o, const char *k, int v) {
  cJSON_DeleteItemFromObjectCaseSensitive(o, k);
  cJSON_AddBoolToObject(o, k, v);
}
static cJSON *state_of(cJSON *it) {
  cJSON *s = cJSON_GetObjectItemCaseSensitive(it, "state");
  if (!s) {
    s = cJSON_CreateObject();
    cJSON_AddItemToObject(it, "state", s);
  }
  return s;
}
static double item_mtime(const cJSON *it) {
  return t_of(it, "_mtime");
}
/* a title this device did not have: kept out of the Library (removed + temp) until a row says it was saved */
static cJSON *ensure_item(Cx *c, const char *id, const char *type, const char *name) {
  cJSON *l = map_get(&c->items, id);
  if (l)
    return l;
  char iso[40];
  nm_ms_iso(c->in->now, iso, sizeof iso);
  l = cJSON_CreateObject();
  cJSON_AddStringToObject(l, "_id", id);
  cJSON_AddStringToObject(l, "name", name ? name : "");
  cJSON_AddStringToObject(l, "type", type && *type ? type : "movie");
  cJSON_AddStringToObject(l, "poster", "");
  cJSON_AddStringToObject(l, "posterShape", "poster");
  cJSON_AddStringToObject(l, "background", "");
  cJSON_AddStringToObject(l, "logo", "");
  cJSON_AddStringToObject(l, "year", "");
  cJSON_AddBoolToObject(l, "removed", 1);
  cJSON_AddBoolToObject(l, "temp", 1);
  cJSON_AddStringToObject(l, "_ctime", iso);
  cJSON_AddStringToObject(l, "_mtime", "1970-01-01T00:00:00.000Z");
  cJSON *s = cJSON_AddObjectToObject(l, "state");
  cJSON_AddStringToObject(s, "lastWatched", "1970-01-01T00:00:00.000Z");
  cJSON_AddNumberToObject(s, "timeOffset", 0);
  cJSON_AddNumberToObject(s, "duration", 0);
  cJSON_AddNumberToObject(s, "timesWatched", 0);
  cJSON_AddStringToObject(s, "video_id", "");
  cJSON_AddStringToObject(s, "watched", "");
  cJSON_AddItemToObject(c->in->lib, id, l);
  map_put(&c->items, js_str(l, "_id", ""), l);
  mark(c, l);
  return l;
}
/* "tt123:2:5" -> season 2, episode 5 */
static int vid_se(const char *vid, int *se, int *ep) {
  const char *b = strrchr(vid, ':');
  if (!b || b == vid)
    return 0;
  const char *a = b - 1;
  while (a > vid && *a != ':')
    a--;
  if (*a != ':')
    return 0;
  char *e1, *e2;
  long s = strtol(a + 1, &e1, 10), e = strtol(b + 1, &e2, 10);
  if (e1 != b || *e2)
    return 0;
  *se = (int)s;
  *ep = (int)e;
  return 1;
}
static int eps_has(const cJSON *s, int se, int ep) {
  char k[24];
  snprintf(k, sizeof k, "%d:%d", se, ep);
  const cJSON *a = js_arr(s, "nvEps"), *e;
  if (a)
    cJSON_ArrayForEach(e, a)
      if (cJSON_IsString(e) && !strcmp(e->valuestring, k))
        return 1;
  return 0;
}
static void eps_set(cJSON *s, int se, int ep, int on) {
  char k[24];
  snprintf(k, sizeof k, "%d:%d", se, ep);
  cJSON *a = cJSON_GetObjectItemCaseSensitive(s, "nvEps");
  if (!cJSON_IsArray(a)) {
    cJSON_DeleteItemFromObjectCaseSensitive(s, "nvEps");
    a = cJSON_AddArrayToObject(s, "nvEps");
  }
  int i = 0;
  cJSON *e;
  cJSON_ArrayForEach(e, a) {
    if (cJSON_IsString(e) && !strcmp(e->valuestring, k)) {
      if (!on)
        cJSON_DeleteItemFromArray(a, i);
      return;
    }
    i++;
  }
  if (on)
    cJSON_AddItemToArray(a, cJSON_CreateString(k));
}
static char *body_of(cJSON *p) {
  char *s = cJSON_PrintUnformatted(p);
  cJSON_Delete(p);
  return s;
}
static cJSON *params(const NmIn *in) {
  cJSON *p = cJSON_CreateObject();
  cJSON_AddNumberToObject(p, "p_profile_id", in->profile);
  return p;
}
static const char *row_str2(const cJSON *r, const char *a, const char *b) {
  return js_str(r, a, js_str(r, b, ""));
}

/* ---- the Library: saved titles --------------------------------------------------------------- */
static void merge_lib(Cx *c, Map *remote, Map *prevIds) {
  const NmIn *in = c->in;
  NmOut *o = c->out;
  const cJSON *row;
  cJSON *it;
  double last = in->last;
  int first = last <= 0;
  cJSON_ArrayForEach(row, in->rLib) {
    const char *id = js_str(row, "content_id", NULL);
    if (!id || !*id)
      continue;
    cJSON *l = map_get(&c->items, id);
    if (l && js_bool(l, "removed", 0) && !js_bool(l, "temp", 0) && map_get(prevIds, id) && item_mtime(l) > last)
      continue; /* removed here since: stays removed (the push drops it) */
    if (!l)
      l = ensure_item(c, id, js_str(row, "content_type", "movie"), js_str(row, "name", ""));
    if (js_bool(l, "removed", 1) || js_bool(l, "temp", 1)) {
      set_b(l, "removed", 0);
      set_b(l, "temp", 0);
      mark(c, l);
    }
    const char *k[3] = { "name", "poster", "background" };
    for (int i = 0; i < 3; i++) {
      const char *v = js_str(row, k[i], "");
      if (*v && !js_str(l, k[i], "")[0]) {
        set_s(l, k[i], v);
        mark(c, l);
      }
    }
    if (item_mtime(l) <= 0) {
      char iso[40];
      nm_ms_iso(t_of(row, "added_at"), iso, sizeof iso);
      set_s(l, "_mtime", iso);
      mark(c, l);
    }
  }
  if (!in->libCapped)
    cJSON_ArrayForEach(it, in->lib) {
      const char *id = js_str(it, "_id", "");
      if (js_bool(it, "removed", 1) || js_bool(it, "temp", 0))
        continue;
      if (!map_get(remote, id) && map_get(prevIds, id) && item_mtime(it) <= last) {
        set_b(it, "removed", 1);
        mark(c, it);
      }
    } /* removed on the phone */
  cJSON *rows = cJSON_CreateArray();
  int diff = 0, nr = in->rLib ? cJSON_GetArraySize(in->rLib) : 0;
  cJSON_ArrayForEach(it, in->lib) {
    if (js_bool(it, "removed", 1) || js_bool(it, "temp", 0) || !strcmp(js_str(it, "type", ""), "other"))
      continue;
    const char *id = js_str(it, "_id", "");
    cJSON *r = cJSON_CreateObject();
    const cJSON *src = map_get(remote, id);
    if (!src)
      diff = 1;
    double added = src ? t_of(src, "added_at") : 0;
    if (!added)
      added = t_of(it, "_ctime");
    if (!added)
      added = in->now;
    cJSON_AddStringToObject(r, "content_id", id);
    cJSON_AddStringToObject(r, "content_type", js_str(it, "type", "movie"));
    cJSON_AddStringToObject(r, "name", js_str(it, "name", ""));
    const char *pv = js_str(it, "poster", "");
    if (*pv)
      cJSON_AddStringToObject(r, "poster", pv);
    else
      cJSON_AddNullToObject(r, "poster");
    cJSON_AddStringToObject(r, "poster_shape", src ? js_str(src, "poster_shape", "POSTER") : "POSTER");
    const char *bg = js_str(it, "background", "");
    if (*bg)
      cJSON_AddStringToObject(r, "background", bg);
    else
      cJSON_AddNullToObject(r, "background");
    cJSON_AddStringToObject(r, "description", src ? js_str(src, "description", "") : "");
    cJSON_AddStringToObject(r, "release_info", src ? js_str(src, "release_info", "") : js_str(it, "year", ""));
    const cJSON *ir = src ? fld(src, "imdb_rating") : NULL;
    if (cJSON_IsNumber(ir))
      cJSON_AddNumberToObject(r, "imdb_rating", ir->valuedouble);
    else
      cJSON_AddNullToObject(r, "imdb_rating");
    const cJSON *gn = src ? fld(src, "genres") : NULL;
    cJSON_AddItemToObject(r, "genres", cJSON_IsArray(gn) ? cJSON_Duplicate(gn, 1) : cJSON_CreateArray());
    const char *ab = src ? js_str(src, "addon_base_url", NULL) : NULL;
    if (ab)
      cJSON_AddStringToObject(r, "addon_base_url", ab);
    else
      cJSON_AddNullToObject(r, "addon_base_url");
    cJSON_AddNumberToObject(r, "added_at", added);
    cJSON_AddItemToArray(rows, r);
    cJSON_AddItemToArray(o->libIds, cJSON_CreateString(id));
  }
  int n = cJSON_GetArraySize(rows);
  if (n != nr)
    diff = 1;
  /* whole-list push: a list much shorter than what came down is a fault on this side, never a user's few removals */
  if (nr > 10 && n < nr / 2) {
    o->refusedShrink = 1;
    diff = 0;
  }
  if (diff && !in->libCapped && !(first && !n)) {
    cJSON *p = params(in);
    cJSON_AddItemToObject(p, "p_items", rows);
    o->pushLib = body_of(p);
    o->nLib = n;
  } else
    cJSON_Delete(rows);
}

/* ---- watch progress: the newest row of each title decides where it stands here ------------------ */
static double prog_t(const cJSON *r) {
  double t = t_of(r, "last_watched");
  return t ? t : t_of(r, "updated_at");
}
static void prog_key(const char *id, int isEp, int se, int ep, char *out, size_t cap) {
  if (isEp)
    snprintf(out, cap, "%s_s%de%d", id, se, ep);
  else
    snprintf(out, cap, "%s", id);
}
static void merge_prog(Cx *c) {
  const NmIn *in = c->in;
  NmOut *o = c->out;
  const cJSON *row;
  cJSON *it;
  double last = in->last;
  Map newest;
  map_init(&newest, in->rProg ? (unsigned)cJSON_GetArraySize(in->rProg) : 8);
  cJSON_ArrayForEach(row, in->rProg) {
    const char *id = js_str(row, "content_id", NULL);
    if (!id || !*id)
      continue;
    const cJSON *b = map_get(&newest, id);
    if (!b || prog_t(row) > prog_t(b))
      map_put(&newest, id, (void *)row);
  }
  cJSON_ArrayForEach(row, in->rProg) {
    const char *id = js_str(row, "content_id", NULL);
    if (!id || !*id || map_get(&newest, id) != row)
      continue;
    double t = prog_t(row);
    cJSON *l = ensure_item(c, id, js_str(row, "content_type", "movie"), NULL);
    cJSON *s = state_of(l);
    if (t <= t_of(s, "lastWatched") + 1000)
      continue; /* this device is as new or newer */
    const char *vid = js_str(row, "video_id", "");
    char v[200];
    if (!strncmp(vid, EP_PREFIX, strlen(EP_PREFIX)))
      snprintf(v, sizeof v, "%s:%s", id, vid + strlen(EP_PREFIX));
    else if (*vid)
      snprintf(v, sizeof v, "%s", vid);
    else if (cJSON_IsNumber(fld(row, "season")))
      snprintf(v, sizeof v, "%s:%d:%d", id, (int)js_num(row, "season", 0), (int)js_num(row, "episode", 0));
    else
      snprintf(v, sizeof v, "%s", id);
    double pos = js_num(row, "position_ms", js_num(row, "position", 0)),
           dur = js_num(row, "duration_ms", js_num(row, "duration", 0));
    char iso[40];
    nm_ms_iso(t, iso, sizeof iso);
    set_s(s, "video_id", v);
    set_n(s, "timeOffset", pos);
    set_n(s, "duration", dur);
    set_s(s, "lastWatched", iso);
    if (item_mtime(l) < t)
      set_s(l, "_mtime", iso);
    mark(c, l);
  }
  map_free(&newest);
  /* what this device watched since: its row replaced (or a finished one deleted), the rest sent as they came */
  cJSON *entries = cJSON_CreateArray(), *del = cJSON_CreateArray(), *grave = cJSON_CreateArray();
  int diff = 0;
  Map byKey;
  map_init(&byKey, in->rProg ? (unsigned)cJSON_GetArraySize(in->rProg) + 16 : 16);
  cJSON_ArrayForEach(row, in->rProg) {
    cJSON *e = cJSON_Duplicate(row, 1);
    cJSON_AddItemToArray(entries, e);
    const char *pk = js_str(e, "progress_key", NULL);
    if (!pk) {
      char key[260];
      const char *id = js_str(e, "content_id", "");
      const cJSON *se = fld(e, "season"), *ep = fld(e, "episode");
      prog_key(id, cJSON_IsNumber(se) && cJSON_IsNumber(ep), (int)js_num(e, "season", 0), (int)js_num(e, "episode", 0),
               key, sizeof key);
      cJSON_AddStringToObject(e, "progress_key", key);
      pk = js_str(e, "progress_key", "");
    }
    map_put(&byKey, pk, e);
  }
  cJSON_ArrayForEach(it, in->lib) {
    cJSON *s = cJSON_GetObjectItemCaseSensitive(it, "state");
    if (!s)
      continue;
    const char *id = js_str(it, "_id", ""), *vid = js_str(s, "video_id", "");
    double lt = t_of(s, "lastWatched");
    if (lt <= last || !*vid)
      continue;
    int se = 0, ep = 0, isEp = strcmp(vid, id) && vid_se(vid, &se, &ep);
    char key[260];
    prog_key(id, isEp, se, ep, key, sizeof key);
    double off = js_num(s, "timeOffset", 0), dur = js_num(s, "duration", 0);
    cJSON *ex = map_get(&byKey, key);
    if (ex) {
      double et = t_of(ex, "last_watched"), eu = t_of(ex, "updated_at");
      if ((et > eu ? et : eu) + 1000 >= lt)
        continue;
    } /* Nuvio has it as new already (either of its times) */
    if (off <= 0) {
      if (ex) {
        map_put(&byKey, js_str(ex, "progress_key", ""), NULL);
        cJSON_AddItemToArray(del, cJSON_CreateString(key));
        cJSON_AddItemToArray(grave, cJSON_DetachItemViaPointer(entries, ex));
      }
      continue;
    } /* (kept until the map goes: it holds the key's text) */
    if (dur > 0 && dur < 60000)
      continue; /* (clips and trailers stay here) */
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "content_id", id);
    cJSON_AddStringToObject(r, "content_type", js_str(it, "type", "movie"));
    cJSON_AddStringToObject(r, "video_id", vid);
    if (isEp) {
      cJSON_AddNumberToObject(r, "season", se);
      cJSON_AddNumberToObject(r, "episode", ep);
    } else {
      cJSON_AddNullToObject(r, "season");
      cJSON_AddNullToObject(r, "episode");
    }
    cJSON_AddNumberToObject(r, "position", (double)(long long)off);
    cJSON_AddNumberToObject(r, "duration", (double)(long long)dur);
    cJSON_AddNumberToObject(r, "last_watched", lt);
    cJSON_AddStringToObject(r, "progress_key", key);
    if (ex) {
      map_put(&byKey, js_str(r, "progress_key", ""), r);
      cJSON_ReplaceItemViaPointer(entries, ex, r);
    } else {
      cJSON_AddItemToArray(entries, r);
      map_put(&byKey, js_str(r, "progress_key", ""), r);
    }
    diff = 1;
  }
  map_free(&byKey);
  cJSON_Delete(grave);
  if (cJSON_GetArraySize(del)) {
    cJSON *p = params(in);
    cJSON_AddItemToObject(p, "p_keys", del);
    cJSON_AddStringToObject(p, "p_origin_client_id", in->client ? in->client : "");
    o->delProg = body_of(p);
  } else
    cJSON_Delete(del);
  int n = cJSON_GetArraySize(entries);
  if (diff && n) { /* the rows as Nuvio takes them: only its own fields */
    static const char *const f[] = { "content_id", "content_type", "video_id",     "season",      "episode",
                                     "position",   "duration",     "last_watched", "progress_key" };
    cJSON *clean = cJSON_CreateArray();
    cJSON *e;
    cJSON_ArrayForEach(e, entries) {
      cJSON *r = cJSON_CreateObject();
      for (unsigned i = 0; i < sizeof f / sizeof *f; i++) {
        const cJSON *v = fld(e, f[i]);
        if (!v && !strcmp(f[i], "position"))
          v = fld(e, "position_ms");
        if (!v && !strcmp(f[i], "duration"))
          v = fld(e, "duration_ms");
        cJSON_AddItemToObject(r, f[i], v ? cJSON_Duplicate(v, 1) : cJSON_CreateNull());
      }
      cJSON_AddItemToArray(clean, r);
    }
    cJSON *p = params(in);
    cJSON_AddItemToObject(p, "p_entries", clean);
    cJSON_AddStringToObject(p, "p_origin_client_id", in->client ? in->client : "");
    o->pushProg = body_of(p);
    o->nProg = n;
  }
  cJSON_Delete(entries);
}

/* ---- watched: movies by timesWatched, episodes by state.nvEps ("S:E") ---------------------------- */
static void w_row(cJSON *rows, const char *id, const char *type, const char *title, int isEp, int se, int ep,
                  double at) {
  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "content_id", id);
  cJSON_AddStringToObject(r, "content_type", type);
  cJSON_AddStringToObject(r, "title", title);
  if (isEp) {
    cJSON_AddNumberToObject(r, "season", se);
    cJSON_AddNumberToObject(r, "episode", ep);
  } else {
    cJSON_AddNullToObject(r, "season");
    cJSON_AddNullToObject(r, "episode");
  }
  cJSON_AddNumberToObject(r, "watched_at", at);
  cJSON_AddItemToArray(rows, r);
}
static void w_key(char *out, size_t cap, const char *id, int isEp, int se, int ep) {
  if (isEp)
    snprintf(out, cap, "%s|%d|%d", id, se, ep);
  else
    snprintf(out, cap, "%s", id);
}
static void w_keep(cJSON *keys, Map *seen, const char *key) {
  cJSON *e = cJSON_CreateString(key);
  cJSON_AddItemToArray(keys, e);
  map_put(seen, e->valuestring, e);
}
static void merge_watched(Cx *c, Map *prevW) {
  const NmIn *in = c->in;
  NmOut *o = c->out;
  const cJSON *row;
  cJSON *it;
  double last = in->last;
  char key[260];
  Map seen, dels;
  map_init(&seen, in->rWat ? (unsigned)cJSON_GetArraySize(in->rWat) + 64 : 64);
  map_init(&dels, 16);
  cJSON *rows = cJSON_CreateArray(), *del = cJSON_CreateArray(), *delNames = cJSON_CreateArray();
  int diff = 0;
  cJSON_ArrayForEach(row, in->rWat) {
    const char *id = js_str(row, "content_id", NULL);
    if (!id || !*id)
      continue;
    const cJSON *se = fld(row, "season"), *ep = fld(row, "episode");
    int isEp = cJSON_IsNumber(se) && cJSON_IsNumber(ep), sn = isEp ? (int)se->valuedouble : 0,
        en = isEp ? (int)ep->valuedouble : 0;
    w_key(key, sizeof key, id, isEp, sn, en);
    cJSON *l = map_get(&c->items, id);
    cJSON *s = l ? state_of(l) : NULL;
    int here = l && (isEp ? eps_has(s, sn, en) : js_num(s, "timesWatched", 0) > 0);
    if (!here && l && map_get(prevW, key) && item_mtime(l) > last) { /* unwatched here since */
      cJSON *k = cJSON_CreateObject();
      cJSON_AddStringToObject(k, "content_id", id);
      if (isEp) {
        cJSON_AddNumberToObject(k, "season", sn);
        cJSON_AddNumberToObject(k, "episode", en);
      }
      cJSON_AddItemToArray(del, k);
      cJSON *nm = cJSON_CreateString(key);
      cJSON_AddItemToArray(delNames, nm);
      map_put(&dels, nm->valuestring, nm);
      diff = 1;
      continue;
    }
    if (!here) {
      l = ensure_item(c, id, js_str(row, "content_type", "movie"), row_str2(row, "title", "name"));
      s = state_of(l);
      if (isEp)
        eps_set(s, sn, en, 1);
      else
        set_n(s, "timesWatched", 1);
      mark(c, l);
    }
    w_keep(o->wKeys, &seen, key);
    w_row(rows, id, js_str(row, "content_type", "movie"), row_str2(row, "title", "name"), isEp, sn, en,
          t_of(row, "watched_at"));
  }
  /* removed on the phone since the last sync: unwatched here too */
  if (!in->watCapped && cJSON_IsArray(in->prevW)) {
    const cJSON *k;
    cJSON_ArrayForEach(k, in->prevW) {
      if (!cJSON_IsString(k) || map_get(&seen, k->valuestring) || map_get(&dels, k->valuestring))
        continue;
      char id[200];
      int se = 0, ep = 0;
      snprintf(id, sizeof id, "%s", k->valuestring);
      char *bar = strchr(id, '|');
      int isEp = bar && sscanf(bar, "|%d|%d", &se, &ep) == 2;
      if (bar)
        *bar = 0;
      cJSON *l = map_get(&c->items, id);
      if (!l || item_mtime(l) > last)
        continue;
      cJSON *s = state_of(l);
      if (isEp)
        eps_set(s, se, ep, 0);
      else
        set_n(s, "timesWatched", 0);
      mark(c, l);
    }
  }
  /* watched here and not on the phone */
  cJSON_ArrayForEach(it, in->lib) {
    cJSON *s = cJSON_GetObjectItemCaseSensitive(it, "state");
    if (!s)
      continue;
    const char *id = js_str(it, "_id", ""), *ty = js_str(it, "type", "movie");
    if (strcmp(ty, "series") && js_num(s, "timesWatched", 0) > 0 && !map_get(&seen, id)) {
      double lw = t_of(s, "lastWatched");
      w_row(rows, id, ty, js_str(it, "name", ""), 0, 0, 0, lw ? lw : in->now);
      w_keep(o->wKeys, &seen, id);
      diff = 1;
    }
    const cJSON *eps = js_arr(s, "nvEps"), *e;
    if (eps)
      cJSON_ArrayForEach(e, eps) {
        int se, ep;
        if (!cJSON_IsString(e) || sscanf(e->valuestring, "%d:%d", &se, &ep) != 2)
          continue;
        w_key(key, sizeof key, id, 1, se, ep);
        if (map_get(&seen, key))
          continue;
        w_row(rows, id, "series", js_str(it, "name", ""), 1, se, ep, in->now);
        w_keep(o->wKeys, &seen, key);
        diff = 1;
      }
  }
  if (cJSON_GetArraySize(del) && !in->watCapped) {
    cJSON *p = params(in);
    cJSON_AddItemToObject(p, "p_keys", del);
    o->delW = body_of(p);
  } else
    cJSON_Delete(del);
  cJSON_Delete(delNames);
  map_free(&seen);
  map_free(&dels);
  int n = cJSON_GetArraySize(rows);
  if (diff && n && !in->watCapped) {
    cJSON *p = params(in);
    cJSON_AddItemToObject(p, "p_items", rows);
    o->pushW = body_of(p);
    o->nW = n;
  } else
    cJSON_Delete(rows);
}

void nm_merge(const NmIn *in, NmOut *o) {
  memset(o, 0, sizeof *o);
  o->libIds = cJSON_CreateArray();
  o->wKeys = cJSON_CreateArray();
  Cx c;
  memset(&c, 0, sizeof c);
  c.in = in;
  c.out = o;
  unsigned nl = (unsigned)cJSON_GetArraySize(in->lib);
  map_init(&c.items, nl + 256);
  map_init(&c.chg, 256);
  cJSON *it;
  cJSON_ArrayForEach(it, in->lib) {
    const char *id = js_str(it, "_id", NULL);
    if (id)
      map_put(&c.items, id, it);
  }
  Map remote, prevIds, prevW;
  const cJSON *row;
  map_init(&remote, in->rLib ? (unsigned)cJSON_GetArraySize(in->rLib) : 8);
  cJSON_ArrayForEach(row, in->rLib) {
    const char *id = js_str(row, "content_id", NULL);
    if (id && *id)
      map_put(&remote, id, (void *)row);
  }
  map_init(&prevIds, in->prevIds ? (unsigned)cJSON_GetArraySize(in->prevIds) : 8);
  set_of(&prevIds, in->prevIds);
  map_init(&prevW, in->prevW ? (unsigned)cJSON_GetArraySize(in->prevW) : 8);
  set_of(&prevW, in->prevW);
  merge_lib(&c, &remote, &prevIds);
  merge_prog(&c);
  merge_watched(&c, &prevW);
  map_free(&remote);
  map_free(&prevIds);
  map_free(&prevW);
  map_free(&c.items);
  map_free(&c.chg);
}
void nm_out_free(NmOut *o) {
  free(o->changed);
  free(o->pushLib);
  free(o->pushProg);
  free(o->delProg);
  free(o->pushW);
  free(o->delW);
  cJSON_Delete(o->libIds);
  cJSON_Delete(o->wKeys);
  memset(o, 0, sizeof *o);
}
