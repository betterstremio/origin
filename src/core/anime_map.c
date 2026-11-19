#include "core/anime_map.h"
#include "core/json.h"
#include "core/net.h"
#include "core/store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ARM_URL "https://arm.haglund.dev/api/v2/ids"
#define MAX_ASK 100
#define MAX_FLIGHT 2
#define MISS_TTL (7 * 86400)

typedef struct {
  int alid;
  char imdb[16];
  int kitsu;
  int season;
  long long stamp;
  int imdbNext;
} MapEntry;

typedef struct {
  char key[16];
  int any;
  int best;
  int bestSeason;
  int head;
} ImdbSlot;

typedef struct {
  int id[MAX_ASK];
  int n;
  long long askedAt;
  char *body;
} Request;

typedef struct {
  char imdb[16];
  int kitsu;
  int season;
  long long stamp;
} Answer;

static MapEntry *entries;
static int nentries, capentries;
static int *alHash;
static unsigned alHashCap;
static ImdbSlot *imdbHash;
static unsigned imdbHashCap;
static int loadStarted;
static int loaded;
static unsigned version;

static int *pending;
static int npending, cappending;
static int pendingHead;
static int *pendingHash;
static unsigned pendingHashCap;
static int pendingDirty;
static Request *flights[MAX_FLIGHT];
static int inflight;
static long long backoffUntil;
static int failStreak;

static long long now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static unsigned hash_id(int id) {
  unsigned x = (unsigned)id;
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  return x ^ (x >> 16);
}

static unsigned hash_text(const char *s) {
  unsigned h = 2166136261U;
  for (; *s; s++)
    h = (h ^ (unsigned char)*s) * 16777619U;
  return h;
}

static unsigned hash_cap_for(unsigned n) {
  unsigned cap = 16;
  while (cap < n * 2)
    cap <<= 1;
  return cap;
}

static int valid_imdb(const char *s) {
  size_t n = s ? strlen(s) : 0;
  if (n < 7 || n > 12 || s[0] != 't' || s[1] != 't')
    return 0;
  for (size_t i = 2; i < n; i++)
    if (s[i] < '0' || s[i] > '9')
      return 0;
  return 1;
}

static int entry_find_linear(int alid) {
  for (int i = 0; i < nentries; i++)
    if (entries[i].alid == alid)
      return i;
  return -1;
}

static int entry_find(int alid) {
  if (!alHashCap)
    return entry_find_linear(alid);
  unsigned at = hash_id(alid) & (alHashCap - 1);
  for (;;) {
    int v = alHash[at];
    if (!v)
      return -1;
    if (entries[v - 1].alid == alid)
      return v - 1;
    at = (at + 1) & (alHashCap - 1);
  }
}

static int rebuild_al_hash(unsigned cap) {
  int *p = calloc(cap, sizeof *p);
  if (!p)
    return 0;
  for (int i = 0; i < nentries; i++) {
    unsigned at = hash_id(entries[i].alid) & (cap - 1);
    while (p[at])
      at = (at + 1) & (cap - 1);
    p[at] = i + 1;
  }
  free(alHash);
  alHash = p;
  alHashCap = cap;
  return 1;
}

static int ensure_al_hash(unsigned needed) {
  unsigned cap = alHashCap ? alHashCap : 16;
  while (cap <= needed * 2)
    cap <<= 1;
  return cap == alHashCap || rebuild_al_hash(cap);
}

static void al_hash_insert(int atEntry) {
  if (!alHashCap)
    return;
  unsigned at = hash_id(entries[atEntry].alid) & (alHashCap - 1);
  while (alHash[at])
    at = (at + 1) & (alHashCap - 1);
  alHash[at] = atEntry + 1;
}

static void rebuild_indexes(void) {
  if (!ensure_al_hash((unsigned)nentries)) {
    free(alHash);
    alHash = NULL;
    alHashCap = 0;
  }

  free(imdbHash);
  imdbHash = NULL;
  imdbHashCap = hash_cap_for((unsigned)nentries + 1);
  imdbHash = calloc(imdbHashCap, sizeof *imdbHash);
  if (!imdbHash) {
    imdbHashCap = 0;
    return;
  }
  for (int i = 0; i < nentries; i++) {
    MapEntry *e = &entries[i];
    e->imdbNext = -1;
    if (!e->imdb[0])
      continue;
    unsigned at = hash_text(e->imdb) & (imdbHashCap - 1);
    while (imdbHash[at].key[0] && strcmp(imdbHash[at].key, e->imdb))
      at = (at + 1) & (imdbHashCap - 1);
    ImdbSlot *s = &imdbHash[at];
    if (!s->key[0]) {
      snprintf(s->key, sizeof s->key, "%s", e->imdb);
      s->any = e->alid;
      s->best = e->season > 0 ? e->alid : 0;
      s->bestSeason = e->season;
      s->head = -1;
    } else if (e->season > 0 && (!s->best || e->season < s->bestSeason)) {
      s->best = e->alid;
      s->bestSeason = e->season;
    }
    int *link = &s->head;
    while (*link >= 0 &&
           (entries[*link].season > 0 ? entries[*link].season : 0x7fffffff) <=
               (e->season > 0 ? e->season : 0x7fffffff))
      link = &entries[*link].imdbNext;
    e->imdbNext = *link;
    *link = i;
  }
}

static void entry_set(int alid, const char *imdb, int kitsu, int season, long long stamp) {
  int at = entry_find(alid);
  if (at < 0) {
    int hashed = ensure_al_hash((unsigned)nentries + 1);
    if (nentries == capentries) {
      int next = capentries ? capentries * 2 : 256;
      MapEntry *p = realloc(entries, sizeof *entries * (size_t)next);
      if (!p)
        return;
      entries = p;
      capentries = next;
    }
    at = nentries++;
    entries[at].alid = alid; /* (before it is hashed: the hash reads it) */
    if (hashed)
      al_hash_insert(at);
    else {
      free(alHash);
      alHash = NULL;
      alHashCap = 0;
    }
  }
  entries[at].alid = alid;
  snprintf(entries[at].imdb, sizeof entries[at].imdb, "%s", imdb ? imdb : "");
  entries[at].kitsu = kitsu > 0 ? kitsu : 0;
  entries[at].season = season > 0 ? season : 0;
  entries[at].stamp = stamp;
  entries[at].imdbNext = -1;
}

static void write_header(void) {
  char p[1100], t[1110];
  snprintf(p, sizeof p, "%s/almap.txt", store_data_dir());
  snprintf(t, sizeof t, "%s.tmp", p);
  FILE *f = fopen(t, "wb");
  if (!f)
    return;
  int ok = fprintf(f, "almap 1\n") > 0;
  ok = fclose(f) == 0 && ok;
  if (ok) {
    remove(p);
    rename(t, p);
  } else
    remove(t);
}

static void on_map_file(int status, const unsigned char *body, size_t len, void *ud);

static void start_load(void) {
  if (loadStarted)
    return;
  loadStarted = 1;
  char p[1100];
  snprintf(p, sizeof p, "%s/almap.txt", store_data_dir());
  net_file(p, on_map_file, NULL);
}

static void clear_entries(void) {
  free(entries);
  entries = NULL;
  nentries = 0;
  capentries = 0;
  free(alHash);
  alHash = NULL;
  alHashCap = 0;
  free(imdbHash);
  imdbHash = NULL;
  imdbHashCap = 0;
}

static void parse_map(int status, const unsigned char *body, size_t len) {
  clear_entries();
  int valid = 0;
  size_t pos = 0;
  int lineNo = 0;
  while (status == 200 && body && pos < len) {
    size_t end = pos;
    while (end < len && body[end] != '\n')
      end++;
    size_t n = end - pos;
    char line[128];
    if (n >= sizeof line)
      n = sizeof line - 1;
    memcpy(line, body + pos, n);
    line[n] = 0;
    line[strcspn(line, "\r")] = 0;
    if (!lineNo++)
      valid = !strcmp(line, "almap 1");
    else if (valid) {
      int alid, kitsu, season;
      long long stamp;
      char imdb[32];
      if (sscanf(line, "%d %31s %d %d %lld", &alid, imdb, &kitsu, &season, &stamp) == 5 && alid > 0) {
        if (strcmp(imdb, "-") && !valid_imdb(imdb))
          imdb[0] = 0;
        else if (!strcmp(imdb, "-"))
          imdb[0] = 0;
        entry_set(alid, imdb, kitsu, season, stamp);
      }
    }
    if (end == len)
      break;
    pos = end + 1;
  }
  if (!valid)
    write_header();
  rebuild_indexes();
}

static int known_entry(const MapEntry *e) {
  if (!e)
    return 0;
  return e->imdb[0] || time(NULL) - (time_t)e->stamp <= MISS_TTL;
}

static int pending_find(int alid) {
  if (!pendingHashCap)
    return 0;
  unsigned at = hash_id(alid) & (pendingHashCap - 1);
  for (unsigned n = 0; n < pendingHashCap; n++) {
    int v = pendingHash[at];
    if (!v)
      return 0;
    if (v == alid)
      return 1;
    at = (at + 1) & (pendingHashCap - 1);
  }
  return 0;
}

static void pending_rebuild(unsigned cap) {
  if (cap != pendingHashCap) {
    int *p = calloc(cap, sizeof *p);
    if (!p)
      return;
    free(pendingHash);
    pendingHash = p;
    pendingHashCap = cap;
  } else
    memset(pendingHash, 0, sizeof *pendingHash * pendingHashCap);
  for (int i = 0; i < npending; i++) {
    unsigned at = hash_id(pending[pendingHead + i]) & (pendingHashCap - 1);
    while (pendingHash[at])
      at = (at + 1) & (pendingHashCap - 1);
    pendingHash[at] = pending[pendingHead + i];
  }
}

static void pending_insert(int alid) {
  if (!pendingHashCap || (unsigned)(npending + 1) * 2 >= pendingHashCap)
    pending_rebuild(hash_cap_for((unsigned)npending + 1));
  if (!pendingHashCap)
    return;
  unsigned at = hash_id(alid) & (pendingHashCap - 1);
  while (pendingHash[at] > 0)
    at = (at + 1) & (pendingHashCap - 1);
  pendingHash[at] = alid;
}

static void pending_remove(int alid) {
  if (!pendingHashCap)
    return;
  unsigned at = hash_id(alid) & (pendingHashCap - 1);
  for (unsigned n = 0; n < pendingHashCap; n++) {
    if (!pendingHash[at])
      return;
    if (pendingHash[at] == alid) {
      pendingHash[at] = -1;
      return;
    }
    at = (at + 1) & (pendingHashCap - 1);
  }
}

static int flight_has(int alid) {
  for (int i = 0; i < MAX_FLIGHT; i++)
    if (flights[i])
      for (int j = 0; j < flights[i]->n; j++)
        if (flights[i]->id[j] == alid)
          return 1;
  return 0;
}

static void pending_add(int alid) {
  if (pending_find(alid) || flight_has(alid))
    return;
  if (pendingHead + npending == cappending) {
    if (pendingHead) {
      memmove(pending, pending + pendingHead, sizeof *pending * (size_t)npending);
      pendingHead = 0;
    } else {
      int next = cappending ? cappending * 2 : 256;
      int *p = realloc(pending, sizeof *pending * (size_t)next);
      if (!p)
        return;
      pending = p;
      cappending = next;
    }
  }
  pending[pendingHead + npending++] = alid;
  pending_insert(alid);
}

static void pending_take(int n) {
  for (int i = 0; i < n; i++)
    pending_remove(pending[pendingHead + i]);
  pendingHead += n;
  npending -= n;
  if (!npending) {
    pendingHead = 0;
    if (pendingHash)
      memset(pendingHash, 0, sizeof *pendingHash * pendingHashCap);
  }
}

static void pending_drop_known(void) {
  int keep = 0;
  for (int i = 0; i < npending; i++) {
    int alid = pending[pendingHead + i];
    int at = entry_find(alid);
    if (!known_entry(at >= 0 ? &entries[at] : NULL))
      pending[pendingHead + keep++] = alid;
  }
  pendingHead = 0;
  npending = keep;
  pending_rebuild(hash_cap_for((unsigned)npending + 1));
}

static int imdb_lookup(const char *imdb) {
  if (imdbHashCap) {
    unsigned at = hash_text(imdb) & (imdbHashCap - 1);
    for (;;) {
      ImdbSlot *s = &imdbHash[at];
      if (!s->key[0])
        return 0;
      if (!strcmp(s->key, imdb))
        return s->best ? s->best : s->any;
      at = (at + 1) & (imdbHashCap - 1);
    }
  }
  int any = 0, best = 0, season = 0;
  for (int i = 0; i < nentries; i++)
    if (!strcmp(entries[i].imdb, imdb)) {
      any = entries[i].alid;
      if (entries[i].season > 0 && (!best || entries[i].season < season)) {
        best = entries[i].alid;
        season = entries[i].season;
      }
    }
  return best ? best : any;
}

static void append_answers(Request *r, const Answer *answers) {
  char p[1100];
  snprintf(p, sizeof p, "%s/almap.txt", store_data_dir());
  FILE *f = fopen(p, "ab");
  int wrote = 0;
  if (f) {
    for (int i = 0; i < r->n; i++) {
      if (fprintf(f, "%d %s %d %d %lld\n", r->id[i], answers[i].imdb[0] ? answers[i].imdb : "-", answers[i].kitsu,
                  answers[i].season, answers[i].stamp) < 0)
        break;
      wrote++;
    }
    if (fclose(f) != 0)
      wrote = 0;
  }
  for (int i = 0; i < r->n; i++)
    entry_set(r->id[i], answers[i].imdb, answers[i].kitsu, answers[i].season, answers[i].stamp);
  rebuild_indexes();
  if (!wrote)
    fprintf(stderr, "[animemap] could not write almap.txt (kept in memory)\n");
  version++;
}

static void retry_request(Request *r) {
  for (int i = 0; i < r->n; i++)
    pending_add(r->id[i]);
}

static void remove_flight(Request *r) {
  for (int i = 0; i < MAX_FLIGHT; i++)
    if (flights[i] == r) {
      flights[i] = NULL;
      inflight--;
      return;
    }
}

static void on_answer(int status, const unsigned char *body, size_t len, void *ud) {
  Request *r = ud;
  long long elapsed = now_ms() - r->askedAt;
  remove_flight(r);
  int found = 0;
  Answer answers[MAX_ASK];
  memset(answers, 0, sizeof answers);
  cJSON *root = status == 200 && body ? net_json(body) : NULL;
  int good = root && cJSON_IsArray(root) && cJSON_GetArraySize(root) == r->n;
  if (good)
    for (int i = 0; i < r->n; i++) {
      const cJSON *item = cJSON_GetArrayItem(root, i);
      const char *imdb = js_str(item, "imdb", "");
      if (valid_imdb(imdb)) {
        snprintf(answers[i].imdb, sizeof answers[i].imdb, "%s", imdb);
        found++;
      }
      const cJSON *k = item ? cJSON_GetObjectItemCaseSensitive(item, "kitsu") : NULL;
      answers[i].kitsu = cJSON_IsNumber(k) && k->valueint > 0 ? k->valueint : 0;
      const cJSON *tv = item ? cJSON_GetObjectItemCaseSensitive(item, "thetvdb-season") : NULL;
      const cJSON *tm = item ? cJSON_GetObjectItemCaseSensitive(item, "themoviedb-season") : NULL;
      if (cJSON_IsNumber(tv) && tv->valueint > 0)
        answers[i].season = tv->valueint;
      else if (cJSON_IsNumber(tm) && tm->valueint > 0)
        answers[i].season = tm->valueint;
      answers[i].stamp = (long long)time(NULL);
    }
  if (root)
    net_json_free(root);
  if (good) {
    append_answers(r, answers);
    failStreak = 0;
    backoffUntil = 0;
    fprintf(stderr, "[animemap] asked %d, found %d, missed %d, %lld ms\n", r->n, found, r->n - found, elapsed);
  } else {
    retry_request(r);
    int delay = ++failStreak >= 2 ? 600 : 60;
    backoffUntil = now_ms() + (long long)delay * 1000;
    fprintf(stderr, "[animemap] failed (status %d), backing off %d s\n", status, delay);
  }
  free(r->body);
  free(r);
}

static int send_one(int n) {
  Request *r = calloc(1, sizeof *r);
  if (!r)
    return 0;
  r->n = n;
  size_t cap = 4 + (size_t)n * 28;
  r->body = malloc(cap);
  if (!r->body) {
    free(r);
    return 0;
  }
  size_t o = 0;
  r->body[o++] = '[';
  for (int i = 0; i < n; i++) {
    r->id[i] = pending[pendingHead + i];
    o += (size_t)snprintf(r->body + o, cap - o, "%s{\"anilist\":%d}", i ? "," : "", r->id[i]);
  }
  snprintf(r->body + o, cap - o, "]");
  r->askedAt = now_ms();
  int slot = -1;
  for (int i = 0; i < MAX_FLIGHT; i++)
    if (!flights[i]) {
      slot = i;
      break;
    }
  if (slot < 0) {
    free(r->body);
    free(r);
    return 0;
  }
  pending_take(n);
  flights[slot] = r;
  inflight++;
  if (getenv("SB_ANIME_MAP_OFF")) {
    on_answer(0, NULL, 0, r);
    return 1;
  }
  net_post_hdr_t(ARM_URL, r->body, "application/json", NULL, 10000, on_answer, r);
  return 1;
}

const char *anime_map_imdb(int alid) {
  start_load();
  if (!loaded)
    return NULL;
  int at = entry_find(alid);
  return at >= 0 && known_entry(&entries[at]) && entries[at].imdb[0] ? entries[at].imdb : NULL;
}

int anime_map_known(int alid) {
  start_load();
  if (!loaded)
    return 0;
  int at = entry_find(alid);
  return at >= 0 && known_entry(&entries[at]);
}

int anime_map_kitsu(int alid) {
  start_load();
  if (!loaded)
    return 0;
  int at = entry_find(alid);
  return at >= 0 && known_entry(&entries[at]) ? entries[at].kitsu : 0;
}

int anime_map_season(int alid) {
  start_load();
  if (!loaded)
    return 0;
  int at = entry_find(alid);
  return at >= 0 && known_entry(&entries[at]) ? entries[at].season : 0;
}

int anime_map_alid(const char *imdb) {
  start_load();
  if (!loaded)
    return 0;
  return valid_imdb(imdb) ? imdb_lookup(imdb) : 0;
}

int anime_map_aliases(const char *imdb, int *alids, int *kitsus, int max) {
  start_load();
  if (!loaded || !valid_imdb(imdb) || max <= 0 || !imdbHashCap)
    return 0;
  unsigned at = hash_text(imdb) & (imdbHashCap - 1);
  for (;;) {
    ImdbSlot *s = &imdbHash[at];
    if (!s->key[0])
      return 0;
    if (!strcmp(s->key, imdb)) {
      int n = 0;
      for (int i = s->head; i >= 0 && n < max; i = entries[i].imdbNext) {
        if (alids)
          alids[n] = entries[i].alid;
        if (kitsus)
          kitsus[n] = entries[i].kitsu;
        n++;
      }
      return n;
    }
    at = (at + 1) & (imdbHashCap - 1);
  }
}

void anime_map_want(const int *alids, int n) {
  start_load();
  for (int i = 0; alids && i < n; i++)
    if (alids[i] > 0 && !anime_map_known(alids[i]))
      pending_add(alids[i]);
  pendingDirty = 1;
}

unsigned anime_map_version(void) {
  start_load();
  return version;
}

void anime_map_tick(void) {
  if (!loaded || !npending)
    return;
  if (pendingDirty) {
    pendingDirty = 0;
    return;
  }
  if (now_ms() < backoffUntil)
    return;
  while (npending && inflight < MAX_FLIGHT && now_ms() >= backoffUntil) /* (a failure answered at once sets it) */
    if (!send_one(npending < MAX_ASK ? npending : MAX_ASK)) {
      backoffUntil = now_ms() + 5000;
      break;
    }
}

static void on_map_file(int status, const unsigned char *body, size_t len, void *ud) {
  (void)ud;
  parse_map(status, body, len);
  loaded = 1;
  pending_drop_known();
  version++;
  fprintf(stderr, "[animemap] %d pairs from the disk\n", nentries);
}
