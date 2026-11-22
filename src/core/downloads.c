#include "core/downloads.h"

#ifdef __ANDROID__
#include "core/app.h"
#include "core/best.h"
#include "core/form.h"
#include "core/json.h"
#include "core/net.h"
#include "core/store.h"
#include "platform/plat.h"
#include "sha256.h"
#include <SDL2/SDL.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

/* Only the app thread edits the queue and asks addons for sources. The transfer thread owns its files. */
typedef struct {
  Download item;
  char owner[256], hash[24], filename[256], name[256], description[512];
  unsigned long long addon;
  int exact, maxRes, refreshed;
  Stream *source; /* URLs and request headers stay in memory */
} Entry;

typedef struct {
  Stream source;
  char part[512], validator[512], final[512];
  SDL_atomic_t stop, done;
  SDL_SpinLock progressLock;
  long long bytes, total;
  DownloadState result;
  int expired;
} Transfer;

static Entry *entries;
static int count, nextKey, loaded, active, allowed, background, connected;
static int dirty;
static Uint32 netChecked;
static unsigned lookup;
static Stream candidate;
static int candidateFound;
static SDL_Thread *worker;
static Transfer transfer;
static char directory[448];

int downloads_enabled(void) {
  return form_touch() && !form_tablet();
}

int downloads_eligible(const Stream *s) {
  if (!s || s->p2p || s->cache == STREAM_CACHE_PENDING || sb_stream_is_limit(s) ||
      (strncasecmp(s->url, "http://", 7) && strncasecmp(s->url, "https://", 8)))
    return 0;
  const char *end = s->url + strcspn(s->url, "?#");
  return (end - s->url < 5 || strncasecmp(end - 5, ".m3u8", 5)) &&
         (end - s->url < 4 || strncasecmp(end - 4, ".mpd", 4));
}

static int owner_key(char *out, size_t cap) {
  if (!sb_signed_in())
    return 0;
  const char *id = sb_user_id();
  if (!*id)
    id = sb_user_email();
  if (!*id)
    return 0;
  int n = snprintf(out, cap, "%s:%s", acct_is_nuvio() ? "nuvio" : "stremio", id);
  return n > 0 && (size_t)n < cap;
}

static int visible(const Entry *e) {
  char owner[256];
  return owner_key(owner, sizeof owner) && !strcmp(owner, e->owner);
}

static Entry *find(int key) {
  for (int i = 0; i < count; i++)
    if (entries[i].item.key == key)
      return &entries[i];
  return NULL;
}

static void path(int key, const char *suffix, char *out, size_t cap) {
  snprintf(out, cap, "%s/%d.%s", directory, key, suffix);
}

static long long file_size(const char *file) {
  struct stat st;
  return !stat(file, &st) && S_ISREG(st.st_mode) ? st.st_size : 0;
}
static void delete_files(int key) {
  const char *suffixes[] = { "video", "part", "etag" };
  char file[512];
  for (size_t i = 0; i < sizeof suffixes / sizeof *suffixes; i++) {
    path(key, suffixes[i], file, sizeof file);
    unlink(file);
  }
}

static void save(void) {
  cJSON *root = cJSON_CreateObject();
  cJSON *list = cJSON_AddArrayToObject(root, "items");
  cJSON_AddNumberToObject(root, "next", nextKey);
  for (int i = 0; i < count; i++) {
    const Entry *e = &entries[i];
    const Download *d = &e->item;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToArray(list, o);
    cJSON_AddNumberToObject(o, "key", d->key);
    cJSON_AddStringToObject(o, "owner", e->owner);
    cJSON_AddStringToObject(o, "id", d->id);
    cJSON_AddStringToObject(o, "vid", d->vid);
    cJSON_AddStringToObject(o, "type", d->type);
    cJSON_AddStringToObject(o, "title", d->title);
    cJSON_AddStringToObject(o, "label", d->label);
    cJSON_AddNumberToObject(o, "season", d->season);
    cJSON_AddNumberToObject(o, "episode", d->episode);
    cJSON_AddNumberToObject(o, "state", d->state);
    cJSON_AddNumberToObject(o, "position", d->position);
    cJSON_AddNumberToObject(o, "maxRes", e->maxRes);
    if (e->exact) {
      char addon[24];
      snprintf(addon, sizeof addon, "%llu", e->addon);
      cJSON_AddStringToObject(o, "addon", addon);
      cJSON_AddStringToObject(o, "hash", e->hash);
      cJSON_AddStringToObject(o, "filename", e->filename);
      cJSON_AddStringToObject(o, "name", e->name);
      cJSON_AddStringToObject(o, "description", e->description);
    }
  }
  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!json)
    return;
  char file[512], temp[512];
  snprintf(file, sizeof file, "%s/index.json", directory);
  snprintf(temp, sizeof temp, "%s/index.tmp", directory);
  FILE *f = fopen(temp, "wb");
  if (f) {
    int ok = fputs(json, f) >= 0 && !fflush(f) && !fsync(fileno(f));
    if (fclose(f) == 0 && ok && !rename(temp, file))
      dirty = 0;
    else
      unlink(temp);
  }
  free(json);
}

static void load(void) {
  if (loaded || !downloads_enabled())
    return;
  int len = snprintf(directory, sizeof directory, "%s/downloads", store_data_dir());
  if (len < 0 || (size_t)len >= sizeof directory || (mkdir(directory, 0700) && errno != EEXIST))
    return;
  loaded = 1;
  char file[512];
  snprintf(file, sizeof file, "%s/index.json", directory);
  cJSON *root = NULL;
  long long lenOnDisk = file_size(file);
  FILE *f = lenOnDisk > 0 && lenOnDisk < 16 * 1024 * 1024 ? fopen(file, "rb") : NULL;
  if (f) {
    size_t n = (size_t)lenOnDisk;
    char *json = malloc(n + 1);
    if (json && fread(json, 1, n, f) == n) {
      json[n] = 0;
      root = cJSON_Parse(json);
    }
    free(json);
    fclose(f);
  }
  nextKey = (int)js_num(root, "next", 1);
  if (nextKey < 1)
    nextKey = 1;
  const cJSON *list = js_arr(root, "items"), *o;
  entries = calloc((size_t)cJSON_GetArraySize(list), sizeof *entries);
  if (!entries) {
    cJSON_Delete(root);
    return;
  }
  cJSON_ArrayForEach(o, list) {
    Entry *e = &entries[count];
    Download *d = &e->item;
    d->key = (int)js_num(o, "key", 0);
    d->season = (int)js_num(o, "season", -1);
    d->episode = (int)js_num(o, "episode", 0);
    d->state = (int)js_num(o, "state", DL_FAILED);
    d->position = js_num(o, "position", 0);
    e->maxRes = (int)js_num(o, "maxRes", 0);
    snprintf(d->id, sizeof d->id, "%s", js_str(o, "id", ""));
    snprintf(d->vid, sizeof d->vid, "%s", js_str(o, "vid", ""));
    snprintf(d->type, sizeof d->type, "%s", js_str(o, "type", ""));
    snprintf(d->title, sizeof d->title, "%s", js_str(o, "title", ""));
    snprintf(d->label, sizeof d->label, "%s", js_str(o, "label", ""));
    snprintf(e->owner, sizeof e->owner, "%s", js_str(o, "owner", ""));
    const char *addon = js_str(o, "addon", NULL);
    e->exact = addon != NULL;
    e->addon = addon ? strtoull(addon, NULL, 10) : 0;
    snprintf(e->hash, sizeof e->hash, "%s", js_str(o, "hash", ""));
    snprintf(e->filename, sizeof e->filename, "%s", js_str(o, "filename", ""));
    snprintf(e->name, sizeof e->name, "%s", js_str(o, "name", ""));
    snprintf(e->description, sizeof e->description, "%s", js_str(o, "description", ""));
    if (d->key <= 0 || !d->id[0] || !d->vid[0] || !e->owner[0] || find(d->key))
      continue;
    if (nextKey <= d->key)
      nextKey = d->key + 1;
    if (d->state == DL_REMOVING) {
      delete_files(d->key);
      dirty = 1;
      continue;
    }
    path(d->key, "video", file, sizeof file);
    d->bytes = file_size(file);
    if (d->bytes) {
      d->total = d->bytes;
      d->state = DL_READY;
    } else {
      path(d->key, "part", file, sizeof file);
      d->bytes = file_size(file);
      if (d->state == DL_RUNNING || d->state == DL_FINDING)
        d->state = DL_QUEUED;
      if (d->state == DL_READY)
        d->state = DL_FAILED;
    }
    count++;
  }
  cJSON_Delete(root);
}

static int select_source(Entry *e, const Stream *s) {
  Stream *copy = malloc(sizeof *copy);
  if (!copy)
    return 0;
  *copy = *s;
  free(e->source);
  e->source = copy;
  e->exact = 1;
  e->addon = s->addonKey;
  snprintf(e->name, sizeof e->name, "%s", s->name);
  snprintf(e->filename, sizeof e->filename, "%s", s->filename);
  snprintf(e->hash, sizeof e->hash, "%s", s->hash);
  snprintf(e->description, sizeof e->description, "%s", s->title);
  return 1;
}

int downloads_add(const Download *item, const Stream *source, int maxResolution) {
  load();
  char owner[256];
  if (!loaded || !item->id[0] || !item->vid[0] || !owner_key(owner, sizeof owner) ||
      (source && !downloads_eligible(source)))
    return DL_ADD_FAILED;
  for (int i = 0; i < count; i++) {
    Entry *e = &entries[i];
    if (strcmp(e->owner, owner) || strcmp(e->item.id, item->id) || strcmp(e->item.vid, item->vid))
      continue;
    if (item->season >= 0 && (e->item.season != item->season || e->item.episode != item->episode)) {
      e->item.season = item->season;
      e->item.episode = item->episode;
      dirty = 1;
    }
    if (!source || e->item.key == active ||
        (e->item.state != DL_FAILED && e->item.state != DL_UNAVAILABLE && e->item.state != DL_NO_SPACE))
      return DL_ADD_EXISTS;
    if (!select_source(e, source))
      return DL_ADD_FAILED;
    downloads_retry(e->item.key);
    return DL_ADD_OK;
  }
  Entry *next = realloc(entries, (size_t)(count + 1) * sizeof *entries);
  if (!next)
    return DL_ADD_FAILED;
  entries = next;
  Entry *e = &entries[count];
  *e = (Entry){ .item = *item, .maxRes = maxResolution };
  if (source && !select_source(e, source))
    return DL_ADD_FAILED;
  snprintf(e->owner, sizeof e->owner, "%s", owner);
  e->item.key = nextKey++;
  e->item.state = DL_QUEUED;
  count++;
  dirty = 1;
  return DL_ADD_OK;
}

int downloads_count(void) {
  load();
  int n = 0;
  for (int i = 0; i < count; i++)
    n += entries[i].item.state != DL_REMOVING && visible(&entries[i]);
  return n;
}

const Download *downloads_at(int i) {
  for (int j = 0; j < count; j++)
    if (entries[j].item.state != DL_REMOVING && visible(&entries[j]) && i-- == 0)
      return &entries[j].item;
  return NULL;
}

const Download *downloads_get(int key) {
  Entry *e = find(key);
  return e && e->item.state != DL_REMOVING && visible(e) ? &e->item : NULL;
}

int downloads_path(int key, char *out, size_t cap) {
  const Download *d = downloads_get(key);
  if (!d || d->state != DL_READY)
    return 0;
  path(key, "video", out, cap);
  return file_size(out) > 0;
}

void downloads_save_position(int key, double position) {
  Entry *e = find(key);
  if (e && visible(e) && e->item.state == DL_READY) {
    e->item.position = position;
    dirty = 1;
  }
}

int downloads_wifi_only(void) {
  return strcmp(store_get("downloadsWifi", "true"), "false") != 0;
}
void downloads_set_wifi_only(int on) {
  store_set("downloadsWifi", on ? "true" : "false");
  netChecked = 0;
}
int downloads_waiting_wifi(void) {
  return downloads_wifi_only() && !allowed;
}

/* Keep a partial file only with a strong validator for the exact request. Signed URLs can be issued again, but if
   they change the transfer starts from zero. The sidecar contains a digest, never the URL or credentials. */
typedef struct {
  Transfer *t;
  FILE *file;
  long long offset, bytes, total, length, from, to, rangeTotal;
  char digest[SHA256_BLOCK_SIZE * 2 + 1], etag[256], previous[256];
  unsigned char probe[12];
  size_t probed;
  int status, checked, rejected, encoded, failed, space, badRange;
} Response;

static void response_header(const char *line, void *ud) {
  Response *r = ud;
  if (!strncasecmp(line, "HTTP/", 5)) {
    const char *p = strchr(line, ' ');
    r->status = p ? atoi(p + 1) : 0;
    r->length = r->from = r->to = r->rangeTotal = -1;
    r->etag[0] = 0;
    r->rejected = r->encoded = 0;
  } else if (!strncasecmp(line, "Content-Range:", 14)) {
    sscanf(line + 14, " bytes %lld-%lld/%lld", &r->from, &r->to, &r->rangeTotal);
  } else if (!strncasecmp(line, "Content-Length:", 15)) {
    sscanf(line + 15, "%lld", &r->length);
  } else if (!strncasecmp(line, "ETag:", 5)) {
    const char *p = line + 5 + strspn(line + 5, " \t");
    size_t n = strcspn(p, "\r\n");
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t'))
      n--;
    if (n > 1 && n < sizeof r->etag && p[0] == '"' && p[n - 1] == '"')
      snprintf(r->etag, sizeof r->etag, "%.*s", (int)n, p);
  } else if (!strncasecmp(line, "Content-Type:", 13)) {
    const char *p = line + 13 + strspn(line + 13, " \t");
    r->rejected = !strncasecmp(p, "text/", 5) || strcasestr(p, "mpegurl") || strcasestr(p, "dash+xml") ||
                  strcasestr(p, "application/json");
  } else if (!strncasecmp(line, "Content-Encoding:", 17)) {
    const char *p = line + 17 + strspn(line + 17, " \t");
    r->encoded = strncasecmp(p, "identity", 8) != 0;
  }
}

static int check_space(long long remaining) {
  struct statvfs fs;
  if (!statvfs(directory, &fs)) {
    unsigned long long freeBytes = (unsigned long long)fs.f_bavail * fs.f_frsize;
    if (freeBytes < 32 * 1024 * 1024ULL ||
        (remaining > 0 && freeBytes - 32 * 1024 * 1024ULL < (unsigned long long)remaining))
      return 0;
  }
  return 1;
}

static int fail_io(Response *r) {
  r->space = errno == ENOSPC || errno == EDQUOT;
  r->failed = 1;
  return 0;
}

static int start_response(Response *r) {
  if (r->encoded || (r->status != 200 && r->status != 206))
    return 0;
  if (r->status == 206) {
    if (!r->offset || r->from != r->offset || r->rangeTotal <= r->from || r->to != r->rangeTotal - 1 ||
        strcmp(r->etag, r->previous) || (r->length >= 0 && r->length != r->rangeTotal - r->from)) {
      r->badRange = 1;
      return 0;
    }
    r->total = r->rangeTotal;
  } else {
    r->offset = 0; /* If-Range failed or the server ignored Range: write a fresh file */
    r->total = r->length;
  }
  if (!check_space(r->total - r->offset)) {
    r->space = 1;
    return 0;
  }
  r->checked = 1;
  r->bytes = r->offset;
  if (r->offset) {
    r->file = fopen(r->t->part, "ab");
    return r->file ? 1 : fail_io(r);
  }
  /* Invalidate the old sidecar before truncating, then save the new validator before writing to the file. */
  unlink(r->t->validator);
  r->file = fopen(r->t->part, "wb");
  if (!r->file)
    return fail_io(r);
  if (r->etag[0]) {
    FILE *v = fopen(r->t->validator, "w");
    if (v) {
      int ok = fprintf(v, "%s\n%s\n", r->digest, r->etag) > 0 && !fflush(v) && !fsync(fileno(v));
      if (fclose(v) || !ok)
        unlink(r->t->validator);
    }
  }
  return 1;
}

static int write_data(Response *r, const unsigned char *p, size_t n) {
  if (r->total >= 0 && (long long)n > r->total - r->bytes) {
    r->rejected = 1;
    return 0;
  }
  if (r->total < 0 && (r->bytes / (8 * 1024 * 1024) != (r->bytes + (long long)n) / (8 * 1024 * 1024)) &&
      !check_space((long long)n)) {
    r->space = 1;
    return 0;
  }
  if (fwrite(p, 1, n, r->file) != n)
    return fail_io(r);
  r->bytes += (long long)n;
  SDL_AtomicLock(&r->t->progressLock);
  r->t->bytes = r->bytes;
  r->t->total = r->total;
  SDL_AtomicUnlock(&r->t->progressLock);
  return 1;
}

static int response_data(const unsigned char *p, size_t n, void *ud) {
  Response *r = ud;
  if (SDL_AtomicGet(&r->t->stop))
    return 0;
  if (!p || (r->status >= 300 && r->status < 400))
    return 1;
  if (r->status != 200 && r->status != 206)
    return 0;
  if (r->rejected || (!r->checked && !start_response(r)))
    return 0;
  if (!r->offset && r->probed < sizeof r->probe) {
    size_t k = sizeof r->probe - r->probed;
    if (k > n)
      k = n;
    memcpy(r->probe + r->probed, p, k);
    r->probed += k;
    if (r->probed < sizeof r->probe)
      return 1;
    /* MP4/MOV or Matroska/WebM. Also catches manifests and add-on error pages served as octet-stream. */
    if (memcmp(r->probe + 4, "ftyp", 4) && memcmp(r->probe + 4, "moov", 4) && memcmp(r->probe + 4, "mdat", 4) &&
        memcmp(r->probe, "\x1a\x45\xdf\xa3", 4)) {
      r->rejected = 1;
      return 0;
    }
    if (!write_data(r, r->probe, sizeof r->probe))
      return 0;
    p += k;
    n -= k;
  }
  return !n || write_data(r, p, n);
}

static void resume_info(Response *r) {
  SHA256_CTX hash;
  unsigned char sum[SHA256_BLOCK_SIZE];
  sha256_init(&hash);
  sha256_update(&hash, (const unsigned char *)r->t->source.url, strlen(r->t->source.url) + 1);
  sha256_update(&hash, (const unsigned char *)r->t->source.hdrs, strlen(r->t->source.hdrs));
  sha256_final(&hash, sum);
  for (size_t i = 0; i < sizeof sum; i++)
    snprintf(r->digest + 2 * i, 3, "%02x", sum[i]);
  FILE *v = fopen(r->t->validator, "r");
  if (!v)
    return;
  char digest[sizeof r->digest + 1];
  if (fgets(digest, sizeof digest, v) && !strncmp(digest, r->digest, sizeof r->digest - 1) &&
      digest[sizeof r->digest - 1] == '\n' && fgets(r->previous, sizeof r->previous, v)) {
    r->previous[strcspn(r->previous, "\r\n")] = 0;
    size_t n = strlen(r->previous);
    if (n > 1 && r->previous[0] == '"' && r->previous[n - 1] == '"')
      r->offset = file_size(r->t->part);
  }
  fclose(v);
}

static void request_headers(const Response *r, char *out, size_t cap) {
  /* The transfer owns the byte range and encoding; other headers are supplied by the selected addon. */
  out[0] = 0;
  const char *p = r->t->source.hdrs;
  size_t used = 0;
  while (*p) {
    size_t n = strcspn(p, "\r\n");
    if (n && strncasecmp(p, "Range:", 6) && strncasecmp(p, "If-Range:", 9) && strncasecmp(p, "Accept-Encoding:", 16) &&
        used + n + 2 < cap)
      used += (size_t)snprintf(out + used, cap - used, "%.*s\n", (int)n, p);
    p += n + strspn(p + n, "\r\n");
  }
  used += (size_t)snprintf(out + used, cap - used, "Accept-Encoding: identity");
  if (r->offset)
    snprintf(out + used, cap - used, "\nRange: bytes=%lld-\nIf-Range: %s", r->offset, r->previous);
}

static int run_transfer(void *ud) {
  Transfer *t = ud;
  Response r = { .t = t, .length = -1, .total = -1 };
  resume_info(&r);
  char headers[1024];
  request_headers(&r, headers, sizeof headers);
  int st = net_media_sync(t->source.url, headers, response_header, response_data, &r);
  if (r.file) {
    if (fflush(r.file) || fsync(fileno(r.file)))
      fail_io(&r);
    if (fclose(r.file))
      fail_io(&r);
  }
  if (r.rejected) {
    unlink(t->part);
    unlink(t->validator);
  }
  t->result = r.space ? DL_NO_SPACE : r.rejected ? DL_UNAVAILABLE : DL_FAILED;
  if ((st == 200 || st == 206) && r.checked && !r.rejected && !r.failed && r.bytes >= (long long)sizeof r.probe &&
      (r.total < 0 || r.bytes == r.total) && !rename(t->part, t->final)) {
    unlink(t->validator);
    t->result = DL_READY;
  } else if (r.status == 416 || r.badRange) {
    unlink(t->validator); /* the next attempt starts over */
  }
  t->expired = r.status == 401 || r.status == 403 || r.status == 404 || r.status == 410;
  SDL_AtomicSet(&t->done, 1);
  return 0;
}

static void begin_transfer(Entry *e, const Stream *source) {
  memset(&transfer, 0, sizeof transfer);
  transfer.source = *source;
  path(active, "part", transfer.part, sizeof transfer.part);
  path(active, "etag", transfer.validator, sizeof transfer.validator);
  path(active, "video", transfer.final, sizeof transfer.final);
  transfer.bytes = e->item.bytes;
  transfer.total = source->size > 0 ? (long long)source->size : -1;
  e->item.total = transfer.total;
  e->item.state = DL_RUNNING;
  worker = SDL_CreateThread(run_transfer, "video-download", &transfer);
  if (!worker) {
    e->item.state = DL_FAILED;
    active = 0;
  }
  dirty = 1;
}

static int match(const Entry *e, const Stream *s) {
  if (s->addonKey != e->addon)
    return 0;
  if (e->hash[0] && s->hash[0])
    return !strcmp(e->hash, s->hash);
  if (e->filename[0] && s->filename[0])
    return !strcmp(e->filename, s->filename);
  return (e->name[0] || e->description[0]) && !strcmp(e->name, s->name) && !strcmp(e->description, s->title);
}

static int resolution(const Stream *s) {
  char label[850];
  best_stream_info(s, label, sizeof label);
  const char *res = best_res_of(label);
  if (!res || !*res)
    return -1;
  if (!strcmp(res, "SD"))
    return 480;
  return !strcmp(res, "4K") ? 2160 : atoi(res);
}

static void on_sources(StreamGroup *group, int index, void *ud) {
  (void)index;
  Entry *e = find(active);
  if ((unsigned)(uintptr_t)ud == lookup && e && visible(e) && e->item.state == DL_FINDING) {
    for (int i = 0; i < group->n; i++) {
      const Stream *s = &group->items[i];
      if (!downloads_eligible(s) || (e->exact && !match(e, s)))
        continue;
      if (!e->exact && e->maxRes) {
        int height = resolution(s);
        if (height < 0 || height > e->maxRes)
          continue;
      }
      const Stream *options[] = { &candidate, s };
      if (!candidateFound ||
          (!e->exact && best_pick(options, 2, best_dur(NULL, e->item.type), NULL, 10000, NULL) == s)) {
        candidate = *s;
        candidateFound = 1;
      }
    }
  }
  free(group->items);
}

static void sources_done(int total, void *ud) {
  (void)total;
  Entry *e = find(active);
  if ((unsigned)(uintptr_t)ud != lookup || !e || e->item.state != DL_FINDING)
    return;
  if (!visible(e) || !allowed) {
    e->item.state = DL_QUEUED;
  } else if (candidateFound) {
    begin_transfer(e, &candidate);
    return;
  } else {
    e->item.state = DL_UNAVAILABLE;
  }
  active = 0;
  dirty = 1;
}

static void stop_active(Entry *e) {
  if (e->item.key != active)
    return;
  if (worker) {
    SDL_AtomicSet(&transfer.stop, 1);
  } else {
    lookup++;
    active = 0;
  }
}

void downloads_pause(int key) {
  Entry *e = find(key);
  if (e && visible(e) && (e->item.state == DL_QUEUED || e->item.state == DL_FINDING || e->item.state == DL_RUNNING)) {
    stop_active(e);
    e->item.state = DL_PAUSED;
    dirty = 1;
  }
}

void downloads_retry(int key) {
  Entry *e = find(key);
  if (e && visible(e) &&
      (e->item.state == DL_PAUSED || e->item.state == DL_FAILED || e->item.state == DL_UNAVAILABLE ||
       e->item.state == DL_NO_SPACE)) {
    e->refreshed = 0;
    e->item.state = DL_QUEUED;
    dirty = 1;
  }
}

static void remove_entry(Entry *e) {
  delete_files(e->item.key);
  free(e->source);
  count--;
  memmove(e, e + 1, (entries + count - e) * sizeof *e);
  dirty = 1;
}

void downloads_remove(int key) {
  Entry *e = find(key);
  if (!e || !visible(e))
    return;
  stop_active(e);
  if (key == active && worker) {
    e->item.state = DL_REMOVING;
    dirty = 1;
  } else {
    remove_entry(e);
  }
}

static void worker_update(Entry *e) {
  SDL_AtomicLock(&transfer.progressLock);
  e->item.bytes = transfer.bytes;
  e->item.total = transfer.total;
  SDL_AtomicUnlock(&transfer.progressLock);
  if (!SDL_AtomicGet(&transfer.done))
    return;
  SDL_WaitThread(worker, NULL);
  worker = NULL;
  active = 0;
  if (e->item.state == DL_REMOVING) {
    remove_entry(e);
    return;
  }
  if (transfer.result == DL_READY) {
    free(e->source);
    e->source = NULL;
    e->item.state = DL_READY;
    e->item.bytes = e->item.total = file_size(transfer.final);
  } else {
    e->item.bytes = file_size(transfer.part);
    if (e->item.state == DL_RUNNING) {
      if (transfer.expired && !e->refreshed) {
        e->refreshed = 1;
        free(e->source);
        e->source = NULL;
        e->item.state = DL_QUEUED;
      } else {
        e->item.state = transfer.result;
      }
    }
  }
  dirty = 1;
}

void downloads_pump(void) {
  if (!downloads_enabled())
    return;
  load();
  if (!loaded)
    return;
  static Uint32 lastSaveAttempt;
  Uint32 now = SDL_GetTicks();
  if (dirty && (!lastSaveAttempt || now - lastSaveAttempt >= 100)) {
    lastSaveAttempt = now;
    save();
  }
  Entry *e = find(active);
  if (!netChecked || now - netChecked >= 1000) {
    netChecked = now ? now : 1;
    connected = plat_download_network(downloads_wifi_only());
    int progress = -2; /* idle */
    if (connected) {
      for (int i = 0; i < count; i++) {
        Entry *pending = &entries[i];
        if (pending->item.state <= DL_RUNNING && visible(pending)) {
          progress = -1;
          break;
        }
      }
      if (progress == -1 && e && e->item.state == DL_RUNNING && e->item.total > 0) {
        long long pct = e->item.bytes * 100 / e->item.total;
        progress = pct > 100 ? 100 : (int)pct;
      }
    }
    background = plat_download_status(progress, !app_away());
  }
  allowed = connected && (!app_away() || background);
  if (e && (!allowed || !visible(e)) && (e->item.state == DL_FINDING || e->item.state == DL_RUNNING)) {
    stop_active(e);
    e->item.state = DL_QUEUED;
    dirty = 1;
  }
  if (worker)
    worker_update(e);
  if (active || !allowed || dirty)
    return;
  for (int i = 0; i < count; i++) {
    e = &entries[i];
    if (e->item.state != DL_QUEUED || !visible(e))
      continue;
    active = e->item.key;
    if (e->source) {
      begin_transfer(e, e->source);
    } else {
      e->item.state = DL_FINDING;
      candidateFound = 0;
      sb_streams(e->item.type, e->item.vid, on_sources, sources_done, (void *)(uintptr_t)++lookup);
    }
    break;
  }
}

void downloads_shutdown(void) {
  Entry *e = find(active);
  if (e) {
    stop_active(e);
    if (e->item.state == DL_RUNNING || e->item.state == DL_FINDING)
      e->item.state = DL_QUEUED;
  }
  if (worker)
    SDL_WaitThread(worker, NULL);
  worker = NULL;
  if (e && e->item.state == DL_REMOVING)
    remove_entry(e);
  if (loaded) {
    save();
    plat_download_status(-2, 0);
  }
  for (int i = 0; i < count; i++)
    free(entries[i].source);
  free(entries);
  entries = NULL;
  count = loaded = active = 0;
}
#else
int downloads_enabled(void) {
  return 0;
}
void downloads_pump(void) {}
void downloads_shutdown(void) {}
int downloads_eligible(const Stream *s) {
  return 0;
}
int downloads_add(const Download *item, const Stream *source, int maxResolution) {
  return DL_ADD_FAILED;
}
int downloads_count(void) {
  return 0;
}
const Download *downloads_at(int i) {
  return NULL;
}
const Download *downloads_get(int key) {
  return NULL;
}
int downloads_waiting_wifi(void) {
  return 0;
}
int downloads_wifi_only(void) {
  return 0;
}
void downloads_set_wifi_only(int on) {}
int downloads_path(int key, char *out, size_t cap) {
  return 0;
}
void downloads_pause(int key) {}
void downloads_retry(int key) {}
void downloads_remove(int key) {}
void downloads_save_position(int key, double position) {}
#endif
