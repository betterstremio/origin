#include "core/anime_taste.h"
#include "core/anilist.h"
#include "core/json.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_SEEDS 12
#define MAX_KEYS 48
enum { S_NEW, S_RESOLVING, S_RESOLVED, S_INFO, S_FAIL };
typedef struct {
  char lib[64], name[96];
  double at; /* when it was last touched in the library, ms */
  float w; /* how much it counts: recency x how far it was watched */
  int alid, st;
  AlInfo info;
} Seed;
typedef struct {
  char k[32];
  float w;
  int tag; /* a tag, not a genre */
} Key;
static Seed seeds[MAX_SEEDS];
static int nseeds, pending;
static unsigned libSeen, ver, gen = 1;
static Key gk[MAX_KEYS];
static int ngk;
static char topStudio[48];
static int hasAlid[64], nhas; /* every anime of the library that was resolved (left out of the picks) */
static int begun;

static double mtime_ms(const cJSON *v) {
  if (cJSON_IsNumber(v))
    return v->valuedouble;
  if (cJSON_IsString(v) && v->valuestring) {
    struct tm tm;
    int ms = 0;
    memset(&tm, 0, sizeof tm);
    if (sscanf(v->valuestring, "%d-%d-%dT%d:%d:%d.%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min,
               &tm.tm_sec, &ms) >= 6) {
      tm.tm_year -= 1900;
      tm.tm_mon -= 1;
      return (double)timegm(&tm) * 1000 + ms;
    }
  }
  return 0;
}
static void add_key(const char *name, float w, int tag) {
  for (int i = 0; i < ngk; i++)
    if (!strcmp(gk[i].k, name)) {
      gk[i].w += w;
      return;
    }
  if (ngk < MAX_KEYS) {
    snprintf(gk[ngk].k, sizeof gk[ngk].k, "%s", name);
    gk[ngk].w = w;
    gk[ngk++].tag = tag;
  }
}
static int key_cmp(const void *a, const void *b) {
  float x = ((const Key *)a)->w, y = ((const Key *)b)->w;
  return x < y ? 1 : x > y ? -1 : 0;
}
static void rebuild_weights(void) {
  ngk = 0;
  topStudio[0] = 0;
  Key st[16];
  int nst = 0;
  for (int i = 0; i < nseeds; i++) {
    const Seed *s = &seeds[i];
    if (s->st != S_INFO)
      continue;
    for (int g = 0; g < s->info.ngenres; g++)
      add_key(s->info.genres[g], s->w, 0);
    for (int t = 0; t < s->info.ntags; t++)
      add_key(s->info.tags[t], s->w * 0.4f, 1); /* (tags count with the genres, lighter) */
    if (s->info.studio[0]) {
      int k = 0;
      while (k < nst && strcmp(st[k].k, s->info.studio))
        k++;
      if (k == nst && nst < 16) {
        snprintf(st[nst].k, sizeof st[0].k, "%s", s->info.studio);
        st[nst++].w = 0;
      }
      if (k < nst)
        st[k].w += s->w;
    }
  }
  qsort(gk, (size_t)ngk, sizeof gk[0], key_cmp);
  qsort(st, (size_t)nst, sizeof st[0], key_cmp);
  if (nst)
    snprintf(topStudio, sizeof topStudio, "%s", st[0].k);
}
static void settle(void) {
  if (pending > 0)
    return;
  rebuild_weights();
  ver++;
  char gs[160] = "";
  int shown = 0, withInfo = 0;
  for (int i = 0; i < ngk && shown < 3; i++)
    if (!gk[i].tag) {
      snprintf(gs + strlen(gs), sizeof gs - strlen(gs), "%s%s", shown ? ", " : "", gk[i].k);
      shown++;
    }
  for (int i = 0; i < nseeds; i++)
    withInfo += seeds[i].st == S_INFO;
  fprintf(stderr, "[anime] taste: %d seeds, top genres %s\n", withInfo, gs[0] ? gs : "(none)");
}
static void on_info(const AlInfo *in, int cached, void *ud) {
  (void)cached;
  unsigned g = (unsigned)((uintptr_t)ud >> 8);
  int i = (int)((uintptr_t)ud & 255);
  if (g != gen || i >= nseeds)
    return;
  Seed *s = &seeds[i];
  if (in) {
    s->info = *in;
    s->st = S_INFO;
  } else
    s->st = S_FAIL;
  pending--;
  settle();
}
static void on_resolved(int alid, void *ud) {
  unsigned g = (unsigned)((uintptr_t)ud >> 8);
  int i = (int)((uintptr_t)ud & 255);
  if (g != gen || i >= nseeds)
    return;
  Seed *s = &seeds[i];
  if (alid <= 0) {
    s->st = S_FAIL;
    pending--;
    settle();
    return;
  }
  s->alid = alid;
  s->st = S_RESOLVED;
  if (nhas < 64)
    hasAlid[nhas++] = alid;
  anilist_media_info(alid, on_info, ud);
}
static int seed_cmp(const void *a, const void *b) {
  double x = ((const Seed *)a)->at, y = ((const Seed *)b)->at;
  return x < y ? 1 : x > y ? -1 : 0;
}
void anime_taste_begin(void) {
  if (begun && libSeen == lib_version())
    return;
  begun = 1;
  libSeen = lib_version();
  static Seed old[MAX_SEEDS], fresh[MAX_SEEDS * 4]; /* (main thread only; too big for a stack on a stick) */
  int nold = nseeds, nf = 0;
  memcpy(old, seeds, sizeof old);
  double now = (double)time(NULL) * 1000;
  const cJSON *it;
  cJSON_ArrayForEach(it, lib_all()) {
    const char *id = js_str(it, "_id", ""), *ty = js_str(it, "type", "");
    if (!lib_id_is_anime(ty, id) || (js_bool(it, "removed", 0) && !js_bool(it, "temp", 0)) || nf >= MAX_SEEDS * 4)
      continue;
    const cJSON *st = js_obj(it, "state");
    double to = js_num(st, "timeOffset", 0), d = js_num(st, "duration", 0);
    int done = js_num(st, "timesWatched", 0) > 0 || js_bool(st, "flaggedWatched", 0);
    double at = mtime_ms(cJSON_GetObjectItemCaseSensitive(it, "_mtime"));
    double days = at > 0 ? (now - at) / 86400000.0 : 90;
    float rec = (float)exp(-days / 60.0);
    if (rec < 0.12f)
      rec = 0.12f;
    float how = done ? 1.0f : to > 0 ? (d > 0 ? 0.5f + 0.5f * (float)(to / d) : 0.7f) : 0.25f;
    Seed *s = &fresh[nf++];
    memset(s, 0, sizeof *s);
    snprintf(s->lib, sizeof s->lib, "%s", id);
    snprintf(s->name, sizeof s->name, "%s", js_str(it, "name", ""));
    s->at = at;
    s->w = rec * how;
  }
  qsort(fresh, (size_t)nf, sizeof fresh[0], seed_cmp);
  if (nf > MAX_SEEDS)
    nf = MAX_SEEDS;
  gen++;
  pending = 0;
  nseeds = nf;
  nhas = 0;
  for (int i = 0; i < nf; i++) {
    seeds[i] = fresh[i];
    for (int j = 0; j < nold; j++) /* a seed already answered keeps its answers */
      if (!strcmp(old[j].lib, seeds[i].lib) && old[j].st == S_INFO) {
        seeds[i].alid = old[j].alid;
        seeds[i].info = old[j].info;
        seeds[i].st = S_INFO;
        if (nhas < 64)
          hasAlid[nhas++] = seeds[i].alid;
        break;
      }
    if (seeds[i].st != S_INFO) {
      seeds[i].st = S_RESOLVING;
      pending++;
    }
  }
  for (int i = 0; i < nseeds; i++)
    if (seeds[i].st == S_RESOLVING)
      anilist_resolve(seeds[i].lib, on_resolved, (void *)(uintptr_t)(((uintptr_t)gen << 8) | (uintptr_t)i));
  if (!pending)
    settle();
}
unsigned anime_taste_version(void) {
  return ver;
}
int anime_taste_seeds(void) {
  int n = 0;
  for (int i = 0; i < nseeds; i++)
    n += seeds[i].st == S_INFO;
  return n;
}
int anime_taste_pending(void) {
  return pending;
}
int anime_taste_genres(char g[][24], int max) {
  int n = 0;
  for (int i = 0; i < ngk && n < max; i++)
    if (!gk[i].tag)
      snprintf(g[n++], 24, "%s", gk[i].k);
  return n;
}
int anime_taste_recent(int *ids, char names[][96], int max) {
  int n = 0;
  for (int i = 0; i < nseeds && n < max; i++)
    if (seeds[i].st == S_INFO && seeds[i].alid > 0) {
      ids[n] = seeds[i].alid;
      snprintf(names[n++], 96, "%s", seeds[i].name);
    }
  return n;
}
const char *anime_taste_studio(void) {
  return topStudio;
}
int anime_taste_has(int alid) {
  for (int i = 0; i < nhas; i++)
    if (hasAlid[i] == alid)
      return 1;
  return 0;
}
int anime_taste_score(int alid) {
  AlFacts f;
  if (!ngk || !anilist_facts(alid, &f))
    return 0;
  float top = 0, fit = 0;
  for (int k = 0; k < ngk && top <= 0; k++)
    if (!gk[k].tag)
      top = gk[k].w; /* (sorted: the first genre is the heaviest) */
  if (top <= 0)
    return 0;
  for (int g = 0; g < f.ng; g++)
    for (int k = 0; k < ngk; k++)
      if (!gk[k].tag && !strcmp(gk[k].k, f.genres[g])) {
        fit += gk[k].w / top;
        break;
      }
  float pop = f.popularity > 200000 ? 200000 : (float)f.popularity;
  return (int)(fit * 1000 + pop / 400.0f + (float)f.score * 2);
}
