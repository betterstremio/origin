#include "platform/plat.h"
#include "core/store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <SDL2/SDL.h>

static char dir[1024], path[1100];
static cJSON *root;
static int dirty;
static Uint32 dirtyAt;

static char *read_all(const char *p, size_t *len) {
  FILE *f = fopen(p, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *b = malloc(n + 1);
  size_t got = fread(b, 1, n, f);
  fclose(f);
  b[got] = 0;
  if (len)
    *len = got;
  return b;
}
static int write_all(const char *p, const void *data, size_t len) {
  static unsigned seq;
  char tmp[1240];
  snprintf(tmp, sizeof tmp, "%s.%u.tmp", p,
           __sync_add_and_fetch(&seq, 1)); /* the writer thread and the exit flush never share one */
  FILE *f = fopen(tmp, "wb");
  if (!f)
    return 0;
  size_t w = fwrite(data, 1, len, f);
  fclose(f);
  if (w != len) {
    remove(tmp);
    return 0;
  }
  return rename(tmp, p) == 0;
}
void store_init(const char *dataDir) {
  snprintf(dir, sizeof dir, "%s", dataDir);
  snprintf(path, sizeof path, "%s/state.json", dir);
  char *t = read_all(path, NULL);
  root = t ? cJSON_Parse(t) : NULL;
  free(t);
  if (!root)
    root = cJSON_CreateObject();
  char c[1100];
  snprintf(c, sizeof c, "%s/cache", dir);
  mkdir(c, 0755);
}
const char *store_data_dir(void) {
  return dir;
}
/* settings are read while drawing (every frame, in places): a hash index over the root, rebuilt after any change of
   its keys (the root is a list walked with strcmp, and the library and progress entries make it long) */
static cJSON **ix;
static unsigned ixCap;
static int ixStale = 1;
static unsigned hstr(const char *s) {
  unsigned h = 2166136261u;
  while (*s)
    h = (h ^ (unsigned char)*s++) * 16777619u;
  return h;
}
static cJSON *find(const char *k) {
  if (!root || !k)
    return NULL;
  if (ixStale) {
    ixStale = 0;
    unsigned n = (unsigned)cJSON_GetArraySize(root), cap = 64;
    while (cap < n * 2)
      cap <<= 1;
    if (cap != ixCap) {
      free(ix);
      ix = malloc(cap * sizeof *ix);
      ixCap = cap;
    }
    memset(ix, 0, ixCap * sizeof *ix);
    cJSON *it;
    cJSON_ArrayForEach(it, root)
      if (it->string) {
        unsigned i = hstr(it->string) & (ixCap - 1);
        while (ix[i] && strcmp(ix[i]->string, it->string))
          i = (i + 1) & (ixCap - 1);
        if (!ix[i])
          ix[i] = it;
      }
  }
  unsigned i = hstr(k) & (ixCap - 1);
  cJSON *v;
  while ((v = ix[i]) && strcmp(v->string, k))
    i = (i + 1) & (ixCap - 1);
  return v;
}
static unsigned serial = 1;
unsigned store_serial(void) {
  return serial;
}
static void touch(void) {
  dirty = 1;
  dirtyAt = SDL_GetTicks();
  ixStale = 1;
  serial++;
}
const char *store_get(const char *k, const char *def) {
  cJSON *v = find(k);
  return cJSON_IsString(v) && v->valuestring ? v->valuestring : def;
}
void store_set(const char *k, const char *val) {
  const char *cur = store_get(k, NULL);
  if (cur && val && !strcmp(cur, val))
    return;
  cJSON_DeleteItemFromObjectCaseSensitive(root, k);
  if (val)
    cJSON_AddStringToObject(root, k, val);
  touch();
}
cJSON *store_node(const char *k) {
  return find(k);
}
void store_set_node(const char *k, cJSON *n) {
  if (n && find(k) == n) {
    touch();
    return;
  } /* the store's own node, changed in place */
  cJSON_DeleteItemFromObjectCaseSensitive(root, k);
  if (n)
    cJSON_AddItemToObject(root, k, n);
  touch();
}
void store_remove(const char *k) {
  if (find(k)) {
    cJSON_DeleteItemFromObjectCaseSensitive(root, k);
    touch();
  }
}
/* cache files are written on a thread of their own: a disk write in the middle of a frame was a long frame on the TV
   (its flash can take tens of ms) */
#include <pthread.h>
typedef struct CW {
  char path[1200];
  unsigned char *data;
  size_t len;
  struct CW *next;
} CW;
static CW *cwHead, *cwTail;
static pthread_mutex_t cwMu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cwCv = PTHREAD_COND_INITIALIZER;
static int cwThread;
static void *cw_run(void *a) {
  plat_thread_background(15);
  (void)a;
  for (;;) {
    pthread_mutex_lock(&cwMu);
    while (!cwHead)
      pthread_cond_wait(&cwCv, &cwMu);
    CW *w = cwHead;
    cwHead = w->next;
    if (!cwHead)
      cwTail = NULL;
    pthread_mutex_unlock(&cwMu);
    write_all(w->path, w->data, w->len);
    free(w->data);
    free(w);
  }
  return NULL;
}
static void write_later(const char *path, unsigned char *own, size_t len) { /* own: malloc'd, taken */
  CW *w = malloc(sizeof *w);
  snprintf(w->path, sizeof w->path, "%s", path);
  w->data = own;
  w->len = len;
  w->next = NULL;
  pthread_mutex_lock(&cwMu);
  if (!cwThread) {
    pthread_t t;
    if (pthread_create(&t, NULL, cw_run, NULL) == 0) {
      pthread_detach(t);
      cwThread = 1;
    }
  }
  if (!cwThread) {
    pthread_mutex_unlock(&cwMu);
    write_all(w->path, w->data, w->len);
    free(w->data);
    free(w);
    return;
  }
  if (cwTail)
    cwTail->next = w;
  else
    cwHead = w;
  cwTail = w;
  pthread_cond_signal(&cwCv);
  pthread_mutex_unlock(&cwMu);
}
static void cache_path(const char *key, char *out, size_t cap);
void cache_write(const char *key, const unsigned char *data, size_t len) {
  char p[1200];
  cache_path(key, p, sizeof p);
  unsigned char *c = malloc(len ? len : 1);
  memcpy(c, data, len);
  write_later(p, c, len);
}
void store_flush(void) {
  if (!dirty)
    return;
  dirty = 0;
  char *t = cJSON_PrintUnformatted(root);
  if (!t)
    return;
  if (!write_all(path, t, strlen(t)))
    fprintf(stderr, "[store] could not write %s\n", path);
  free(t);
}
/* while running, the state is printed here and written by the writer thread (the file is a few hundred KB with a
   library); store_flush() at exit writes it at once */
void store_pump(void) {
  if (!dirty || SDL_GetTicks() - dirtyAt <= 1500)
    return;
  dirty = 0;
  char *t = cJSON_PrintUnformatted(root);
  if (t)
    write_later(path, (unsigned char *)t, strlen(t));
}

/* ---- an account's own data, set aside (profiles) ------------------------------------------- */
typedef struct {
  cJSON *o;
  char path[1300];
} ScopeJob;
static int scopeBusy;
static pthread_mutex_t scopeMu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t scopeCv = PTHREAD_COND_INITIALIZER;
static void scope_write(ScopeJob *j) {
  char *t = cJSON_PrintUnformatted(j->o);
  if (t) {
    if (!write_all(j->path, t, strlen(t)))
      fprintf(stderr, "[store] could not write %s\n", j->path);
    free(t);
  }
  cJSON_Delete(j->o);
  free(j);
}
static void *scope_run(void *a) {
  plat_thread_background(15);
  scope_write(a);
  pthread_mutex_lock(&scopeMu);
  scopeBusy--;
  pthread_cond_broadcast(&scopeCv);
  pthread_mutex_unlock(&scopeMu);
  return NULL;
}
static void scope_wait(void) {
  pthread_mutex_lock(&scopeMu);
  while (scopeBusy)
    pthread_cond_wait(&scopeCv, &scopeMu);
  pthread_mutex_unlock(&scopeMu);
} /* (A, B and straight back to A: A's file is complete before it is read) */
void store_scope_swap(const char *from, const char *to, const char *const *keys, int n) {
  char d[1100];
  snprintf(d, sizeof d, "%s/acct", dir);
  mkdir(d, 0755);
  cJSON *o = cJSON_CreateObject();
  for (int i = 0; i < n; i++) {
    cJSON *v = cJSON_DetachItemFromObjectCaseSensitive(root, keys[i]);
    if (v)
      cJSON_AddItemToObject(o, keys[i], v);
  }
  touch();
  if (!from)
    cJSON_Delete(o);
  else {
    ScopeJob *j = malloc(sizeof *j);
    j->o = o;
    snprintf(j->path, sizeof j->path, "%s/%s.json", d, from);
    pthread_t t;
    pthread_mutex_lock(&scopeMu);
    scopeBusy++;
    pthread_mutex_unlock(&scopeMu);
    if (pthread_create(&t, NULL, scope_run, j) == 0)
      pthread_detach(t);
    else {
      pthread_mutex_lock(&scopeMu);
      scopeBusy--;
      pthread_mutex_unlock(&scopeMu);
      scope_write(j);
    }
  }
  scope_wait();
  char p[1300];
  snprintf(p, sizeof p, "%s/%s.json", d, to);
  char *b = read_all(p, NULL);
  cJSON *in = b ? cJSON_Parse(b) : NULL;
  free(b);
  if (in)
    for (int i = 0; i < n; i++) {
      cJSON *v = cJSON_DetachItemFromObjectCaseSensitive(in, keys[i]);
      if (v)
        cJSON_AddItemToObject(root, keys[i], v);
    }
  cJSON_Delete(in);
  touch();
}
void store_scope_forget(const char *prefix) {
  scope_wait();
  char d[1100];
  snprintf(d, sizeof d, "%s/acct", dir);
  DIR *h = opendir(d);
  if (!h)
    return;
  struct dirent *e;
  size_t pl = strlen(prefix);
  while ((e = readdir(h)))
    if (!strncmp(e->d_name, prefix, pl)) {
      char p[1400];
      snprintf(p, sizeof p, "%s/%s", d, e->d_name);
      remove(p);
    }
  closedir(h);
}

/* ---- file cache ---------------------------------------------------------------------------- */
static void cache_path(const char *key, char *out, size_t cap) {
  unsigned long long h1 = 1469598103934665603ULL;
  unsigned h2 = 2166136261u;
  for (const unsigned char *p = (const unsigned char *)key; *p; p++) {
    h1 = (h1 ^ *p) * 1099511628211ULL;
    h2 = (h2 ^ *p) * 16777619u;
  }
  snprintf(out, cap, "%s/cache/%016llx%08x", dir, h1, h2);
}
unsigned char *cache_read(const char *key, long maxAge, size_t *len) {
  char p[1200];
  cache_path(key, p, sizeof p);
  struct stat st;
  if (stat(p, &st))
    return NULL;
  if (maxAge > 0 && time(NULL) - st.st_mtime > maxAge)
    return NULL;
  unsigned char *b = (unsigned char *)read_all(p, len);
  if (b) {
    struct timespec ts[2] = { { 0, UTIME_NOW }, { 0, UTIME_OMIT } };
    utimensat(AT_FDCWD, p, ts, 0);
  } /* last used: the access time (the change time says how fresh it is) */
  return b;
}
/* the cache folder is kept under a cap, the least recently used files going first (the TV's app partition has
   little room: a full one breaks installs). Run once at start, on a thread of its own */
typedef struct {
  char name[40];
  time_t at;
  long size;
} CEnt;
static int cent_cmp(const void *a, const void *b) {
  time_t x = ((const CEnt *)a)->at, y = ((const CEnt *)b)->at;
  return x < y ? -1 : x > y;
}
static long capBytes, keptBytes = -1;
long cache_cap_bytes(void) {
  return capBytes;
}
long cache_kept_bytes(void) {
  return __atomic_load_n(&keptBytes, __ATOMIC_RELAXED);
}
static void *prune_run(void *a) {
  (void)a;
  plat_thread_background(15);
  char d[1100];
  snprintf(d, sizeof d, "%s/cache", dir);
  DIR *dd = opendir(d);
  if (!dd)
    return NULL;
  int n = 0, cap = 4096;
  CEnt *e = malloc(cap * sizeof *e);
  long total = 0;
  struct dirent *de;
  while ((de = readdir(dd))) {
    if (de->d_name[0] == '.' || strlen(de->d_name) >= sizeof e[0].name)
      continue;
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", d, de->d_name);
    struct stat st;
    if (stat(p, &st) || !S_ISREG(st.st_mode))
      continue;
    if (n == cap) {
      cap *= 2;
      e = realloc(e, cap * sizeof *e);
    }
    snprintf(e[n].name, sizeof e[n].name, "%s", de->d_name);
    e[n].at = st.st_atime > st.st_mtime ? st.st_atime : st.st_mtime;
    e[n].size = st.st_size;
    total += st.st_size;
    n++;
  }
  closedir(dd);
  if (total > capBytes) {
    qsort(e, n, sizeof *e, cent_cmp);
    long goal = capBytes * 3 / 4;
    int del = 0;
    for (int i = 0; i < n && total > goal; i++) {
      char p[1200];
      snprintf(p, sizeof p, "%s/%s", d, e[i].name);
      if (!remove(p)) {
        total -= e[i].size;
        del++;
      }
    }
    fprintf(stderr, "[cache] %d files removed, %ld KB kept\n", del, total / 1024);
  }
  __atomic_store_n(&keptBytes, total, __ATOMIC_RELAXED);
  free(e);
  return NULL;
}
void cache_prune_start(int capMb) {
  capBytes = (long)capMb * 1024 * 1024;
  pthread_t t;
  if (!pthread_create(&t, NULL, prune_run, NULL))
    pthread_detach(t);
}
int cache_file(const char *key, long maxAge, char *p, size_t cap) {
  cache_path(key, p, cap);
  struct stat st;
  if (stat(p, &st))
    return 0;
  return !(maxAge > 0 && time(NULL) - st.st_mtime > maxAge);
}
