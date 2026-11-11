#include "platform/plat.h"
#include "core/net.h"
#include "core/update.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <curl/curl.h>
#include <SDL2/SDL.h>
#if !SB_STATIC_CURL
#include <dlfcn.h>
#endif

/* ---- the curl entry points, resolved at run time on the TV -------------------------------- */
static CURL *(*f_easy_init)(void);
static CURLcode (*f_easy_setopt)(CURL *, CURLoption, ...);
static CURLcode (*f_easy_perform)(CURL *);
static CURLcode (*f_easy_getinfo)(CURL *, CURLINFO, ...);
static void (*f_easy_cleanup)(CURL *);
static struct curl_slist *(*f_slist_append)(struct curl_slist *, const char *);
static void (*f_slist_free_all)(struct curl_slist *);
static CURLcode (*f_global_init)(long);
static void (*f_easy_reset)(CURL *); /* (optional: without it every request opens its own connection, as before) */
static CURLSH *(*f_share_init)(void); static CURLSHcode (*f_share_setopt)(CURLSH *, CURLSHoption, ...); /* (optional too) */
static char caPath[1024];
/* "StremioOrigin/1.0.0 (webOS; native)": the running version, the platform it runs on */
static const char *ua(void) { static char u[96]; if (!u[0]) { const char *d = plat_device_name(); snprintf(u, sizeof u, "StremioOrigin/%s (%s; native)", update_current(), strstr(d, "webOS") ? "webOS" : strstr(d, "Android") ? "Android TV" : "Mac"); } return u; }

static int resolve(void) {
#if SB_STATIC_CURL
  f_easy_init = curl_easy_init; f_easy_setopt = curl_easy_setopt; f_easy_perform = curl_easy_perform; f_easy_getinfo = curl_easy_getinfo;
  f_easy_cleanup = curl_easy_cleanup; f_slist_append = curl_slist_append; f_slist_free_all = curl_slist_free_all; f_global_init = curl_global_init; f_easy_reset = curl_easy_reset;
  f_share_init = curl_share_init; f_share_setopt = curl_share_setopt;
  return 1;
#else
  static const char *names[] = { "libcurl.so.5", "libcurl.so.4", "libcurl.so", NULL };
  void *h = NULL; for (int i = 0; names[i] && !h; i++) h = dlopen(names[i], RTLD_NOW);
  if (!h) { fprintf(stderr, "[net] no libcurl: %s\n", dlerror()); return 0; }
#define L(v, n) do { *(void **)(&v) = dlsym(h, n); if (!v) { fprintf(stderr, "[net] missing %s\n", n); return 0; } } while (0)
  L(f_easy_init, "curl_easy_init"); L(f_easy_setopt, "curl_easy_setopt"); L(f_easy_perform, "curl_easy_perform"); L(f_easy_getinfo, "curl_easy_getinfo");
  L(f_easy_cleanup, "curl_easy_cleanup"); L(f_slist_append, "curl_slist_append"); L(f_slist_free_all, "curl_slist_free_all"); L(f_global_init, "curl_global_init");
#undef L
  *(void **)(&f_easy_reset) = dlsym(h, "curl_easy_reset");
  *(void **)(&f_share_init) = dlsym(h, "curl_share_init"); *(void **)(&f_share_setopt) = dlsym(h, "curl_share_setopt");
  return 1;
#endif
}

/* ---- requests ------------------------------------------------------------------------------- */
typedef struct Req {
  unsigned id; char *url, *hdr, *post, *ctype, *method; size_t postLen; NetCb cb; void *ud; long timeoutMs; cJSON *json; /* postLen: a raw body (net_send), else post is text; json: the body already parsed on the worker */
  int status; unsigned char *body; size_t len, cap; int cancelled, done;
  int size; double total;  /* net_size: only the headers are read (2: net_resolve, the final address kept) */
  int bg;                  /* the previews' lane */
  char *eff;
  int file;                /* net_file: url is a local path */
  struct Req *next;
} Req;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static Req *queue, *queueTail, *slowQ, *slowTail, *bgQ, *bgTail, *finished, *inflight[16], *fqueue, *fqueueTail;
static pthread_cond_t slowCv = PTHREAD_COND_INITIALIZER, bgCv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t fcv = PTHREAD_COND_INITIALIZER;
static unsigned nextId = 1; static int npending;
#define WORKERS 16   /* 0-4 take the quick requests (catalogs, details, the account); 5-7 the slow ones (stream addons, up to
                       45 s each): details never wait behind a debrid addon thinking */
#define SLOW0 5
#define BG0 8   /* 8-15: the previews' lane (stream lookups and link resolving for a card at rest): never the slots a title page waits on. Eight, because every addon is asked at once: with two, an addon that never answers held a slot for its whole timeout and the fast ones waited behind it */

static size_t on_data(char *p, size_t s, size_t n, void *ud) {
  Req *r = ud; size_t add = s * n;
  if (r->cancelled) return 0;
  if (r->len + add + 1 > r->cap) { r->cap = (r->len + add + 1) * 2; r->body = realloc(r->body, r->cap); }
  memcpy(r->body + r->len, p, add); r->len += add; r->body[r->len] = 0; return add;
}
/* net_size: "Content-Range: bytes 0-0/<total>" (a 200 instead: its Content-Length); each redirect starts over */
static size_t on_header(char *p, size_t s, size_t n, void *ud) {
  Req *r = ud; size_t len = s * n; char h[256]; size_t k = len < sizeof h - 1 ? len : sizeof h - 1; memcpy(h, p, k); h[k] = 0;
  if (!strncmp(h, "HTTP/", 5)) { r->total = 0; r->status = atoi(strchr(h, ' ') ? strchr(h, ' ') + 1 : "0"); }
  else if (!strncasecmp(h, "content-range:", 14)) { char *sl = strrchr(h, '/'); if (sl) r->total = atof(sl + 1); }
  else if (!strncasecmp(h, "content-length:", 15) && r->status == 200) r->total = atof(h + 15);
  return len;
}
static size_t on_size_data(char *p, size_t s, size_t n, void *ud) { (void)p; (void)s; (void)n; (void)ud; return 0; } /* never the body: a 200 would be the whole film */
/* A worker keeps its curl handle: the connections it opened (with their TLS sessions and DNS answers) stay, and the next
   request to the same addon or API skips the handshake, the slowest part of a request on a TV. Every kept connection holds
   memory, so it is bounded: a device under 1200 MB (a Firestick) keeps them on the quick lane only, one per worker. */
static CURL *handles[WORKERS]; static int keepSlots, keepConns;
static void keep_limits(void) { int mb = plat_mem_total_mb(), small = mb > 0 && mb < 1200; keepSlots = !f_easy_reset ? 0 : small ? SLOW0 : WORKERS; keepConns = small ? 1 : 2; fprintf(stderr, "[net] kept connections: %d workers, up to %d each\n", keepSlots, keepConns); }
/* DNS answers and TLS sessions shared by every worker: a host one worker has met costs another no lookup, and its TLS
   resumes (a full handshake is the slow part on a TV's CPU). Small and fixed: curl keeps a few dozen of each */
static CURLSH *share; static pthread_mutex_t shareMu[CURL_LOCK_DATA_LAST];
static void share_lock(CURL *c, curl_lock_data d, curl_lock_access a, void *u) { (void)c; (void)a; (void)u; if ((int)d >= 0 && d < CURL_LOCK_DATA_LAST) pthread_mutex_lock(&shareMu[d]); }
static void share_unlock(CURL *c, curl_lock_data d, void *u) { (void)c; (void)u; if ((int)d >= 0 && d < CURL_LOCK_DATA_LAST) pthread_mutex_unlock(&shareMu[d]); }
static void share_make(void) {
  if (!f_share_init || !f_share_setopt || !(share = f_share_init())) { fprintf(stderr, "[net] no shared DNS and TLS sessions\n"); return; }
  for (int i = 0; i < CURL_LOCK_DATA_LAST; i++) pthread_mutex_init(&shareMu[i], NULL);
  f_share_setopt(share, CURLSHOPT_LOCKFUNC, share_lock); f_share_setopt(share, CURLSHOPT_UNLOCKFUNC, share_unlock);
  f_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS); f_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION); }
static int nReq, nConn, nShake; static double shakeSum;
static void net_stats(CURL *c) { long nc = 0; double t = 0; f_easy_getinfo(c, CURLINFO_NUM_CONNECTS, &nc); f_easy_getinfo(c, CURLINFO_APPCONNECT_TIME, &t);
  pthread_mutex_lock(&mu); nReq++; if (nc > 0) nConn++; if (nc > 0 && t > 0) { nShake++; shakeSum += t; }
  if (nReq % 40 == 0) fprintf(stderr, "[net] %d requests, %d opened a connection, handshake average %.0f ms\n", nReq, nConn, nShake ? shakeSum * 1000 / nShake : 0.0);
  pthread_mutex_unlock(&mu); }
static void perform(Req *r, int slot) {
  CURL *c; int kept = slot >= 0 && slot < keepSlots;
  if (kept) { c = handles[slot]; if (c) f_easy_reset(c); else c = handles[slot] = f_easy_init(); } else c = f_easy_init();
  if (!c) { r->status = 0; return; }
  if (kept) { f_easy_setopt(c, CURLOPT_MAXCONNECTS, (long)keepConns); f_easy_setopt(c, CURLOPT_DNS_CACHE_TIMEOUT, 300L); }
  if (share) f_easy_setopt(c, CURLOPT_SHARE, share);
  struct curl_slist *hs = NULL;
  f_easy_setopt(c, CURLOPT_URL, r->url); f_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data); f_easy_setopt(c, CURLOPT_WRITEDATA, r);
  f_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L); f_easy_setopt(c, CURLOPT_MAXREDIRS, 8L); f_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  f_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 8000L); f_easy_setopt(c, CURLOPT_TIMEOUT_MS, r->timeoutMs > 0 ? r->timeoutMs : 20000L);
  f_easy_setopt(c, CURLOPT_USERAGENT, ua()); if (!r->size) f_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
  if (caPath[0]) f_easy_setopt(c, CURLOPT_CAINFO, caPath);
  if (r->size) { f_easy_setopt(c, CURLOPT_TIMEOUT_MS, 10000L); f_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_header); f_easy_setopt(c, CURLOPT_HEADERDATA, r); f_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_size_data); hs = f_slist_append(hs, "Range: bytes=0-0"); }
  /* one header per line; one the caller names (User-Agent...) replaces curl's own */
  for (char *h = r->hdr, *nl; h && *h; h = nl ? nl + 1 : NULL) { nl = strchr(h, '\n'); if (nl) *nl = 0; if (*h) hs = f_slist_append(hs, h); if (nl) *nl = '\n'; }
  if (r->post) { if (r->ctype) { if (*r->ctype) { char ct[160]; snprintf(ct, sizeof ct, "Content-Type: %s", r->ctype); hs = f_slist_append(hs, ct); } } else if (!r->postLen) hs = f_slist_append(hs, "Content-Type: application/json");
    f_easy_setopt(c, CURLOPT_POSTFIELDS, r->post); f_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)(r->postLen ? r->postLen : strlen(r->post))); }
  if (r->method) f_easy_setopt(c, CURLOPT_CUSTOMREQUEST, r->method);
  if (hs) f_easy_setopt(c, CURLOPT_HTTPHEADER, hs);
  CURLcode rc = f_easy_perform(c);
  long code = 0; f_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  r->status = rc == CURLE_OK ? (int)code : 0;
  if (r->size == 2) { char *e = NULL; f_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &e); free(r->body); r->body = (unsigned char *)strdup(e ? e : ""); r->len = strlen((char *)r->body); r->status = (int)code; }
  else if (r->size) { char t[32]; int k = snprintf(t, sizeof t, "%.0f", r->total > 0 ? r->total : 0); free(r->body); r->body = (unsigned char *)strdup(t); r->len = (size_t)k; r->status = (int)code; } /* the server's answer even without a size (a 404 says why a film will not play); 0 = no answer */
  net_stats(c); if (hs) f_slist_free_all(hs);
  if (!kept) f_easy_cleanup(c);
}
static void parse_body(Req *r) {
  if (r->body && r->len > 1 && r->len < 16u * 1024 * 1024 && r->status >= 200 && r->status < 300) { const unsigned char *b = r->body; while (*b == ' ' || *b == '\n' || *b == '\r' || *b == '\t') b++; if (*b == '{' || *b == '[') r->json = cJSON_ParseWithLength((const char *)r->body, r->len); }
}
/* disk copies get a thread of their own: behind six stream addons (45 s each) a cached title page would wait as long */
static void *file_worker(void *arg) { plat_thread_background(5);
  (void)arg;
  for (;;) {
    pthread_mutex_lock(&mu);
    while (!fqueue) pthread_cond_wait(&fcv, &mu);
    Req *r = fqueue; fqueue = r->next; if (!fqueue) fqueueTail = NULL; r->next = NULL;
    pthread_mutex_unlock(&mu);
    if (!r->cancelled) { FILE *f = fopen(r->url, "rb"); r->status = 404;
      if (f) { fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        if (n > 0 && (r->body = malloc((size_t)n + 1)) && fread(r->body, 1, (size_t)n, f) == (size_t)n) { r->body[n] = 0; r->len = (size_t)n; r->status = 200; }
        fclose(f); }
      parse_body(r); }
    pthread_mutex_lock(&mu);
    r->done = 1; r->next = finished; finished = r;
    pthread_mutex_unlock(&mu);
  }
  return NULL;
}
unsigned net_file(const char *path, NetCb cb, void *ud) {
  Req *r = calloc(1, sizeof *r); r->url = strdup(path); r->cb = cb; r->ud = ud; r->file = 1;
  pthread_mutex_lock(&mu);
  r->id = nextId++; if (fqueueTail) fqueueTail->next = r; else fqueue = r; fqueueTail = r; npending++;
  pthread_cond_signal(&fcv); pthread_mutex_unlock(&mu);
  return r->id;
}
static void *worker(void *arg) { plat_thread_background(5);
  int slot = (int)(size_t)arg;
  for (;;) {
    pthread_mutex_lock(&mu);
    if (slot >= BG0) while (!bgQ) pthread_cond_wait(&bgCv, &mu); else if (slot >= SLOW0) while (!slowQ) pthread_cond_wait(&slowCv, &mu); else while (!queue) pthread_cond_wait(&cv, &mu);
    Req *r; if (slot >= BG0) { r = bgQ; bgQ = r->next; if (!bgQ) bgTail = NULL; } else if (slot >= SLOW0) { r = slowQ; slowQ = r->next; if (!slowQ) slowTail = NULL; } else { r = queue; queue = r->next; if (!queue) queueTail = NULL; }
    r->next = NULL; inflight[slot] = r;
    pthread_mutex_unlock(&mu);
    if (!r->cancelled) { perform(r, slot);
      /* a JSON answer is parsed here, off the drawing thread (catalogs, metadata, streams: tens of ms each on a TV) */
      parse_body(r); }
    pthread_mutex_lock(&mu);
    inflight[slot] = NULL; r->done = 1; r->next = finished; finished = r;
    pthread_mutex_unlock(&mu);
  }
  return NULL;
}
#if !SB_STATIC_CURL
/* OpenSSL 1.0 (the TV's, under its libcurl) is safe across threads only with locks the app hands it: without them
   its shared tables were changed by two requests at once, and a long session ended in a crash inside it (a bad
   pointer in memcmp, called from libcrypto). 1.1 and later lock by themselves (no CRYPTO_num_locks to find): left as
   they are, as is a library someone already gave locks to */
static pthread_mutex_t *sslLocks;
static void ssl_lock(int mode, int n, const char *file, int line) { (void)file; (void)line; if (mode & 1) pthread_mutex_lock(&sslLocks[n]); else pthread_mutex_unlock(&sslLocks[n]); }
static unsigned long ssl_id(void) { return (unsigned long)pthread_self(); }
static void (*sslSetNum)(void *, unsigned long);
static void ssl_threadid(void *id) { sslSetNum(id, (unsigned long)pthread_self()); } /* (each thread its own id: with one id for all, OpenSSL took them for one thread and they blocked each other) */
static void ssl_locks(void) {
  static const char *names[] = { "libcrypto.so.1.0.0", "libcrypto.so.1.0.2", "libcrypto.so.10", "libcrypto.so", NULL };
  void *h = NULL; for (int i = 0; names[i] && !h; i++) h = dlopen(names[i], RTLD_NOW | RTLD_NOLOAD); /* the one curl brought in */
  if (!h) { fprintf(stderr, "[net] tls: no OpenSSL 1.0 loaded (nothing to lock)\n"); return; }
  int (*num)(void) = NULL; void (*setLock)(void (*)(int, int, const char *, int)) = NULL; void *(*getLock)(void) = NULL; void (*setId)(unsigned long (*)(void)) = NULL; int (*setTid)(void (*)(void *)) = NULL;
  *(void **)(&num) = dlsym(h, "CRYPTO_num_locks"); *(void **)(&setLock) = dlsym(h, "CRYPTO_set_locking_callback"); *(void **)(&getLock) = dlsym(h, "CRYPTO_get_locking_callback");
  *(void **)(&setId) = dlsym(h, "CRYPTO_set_id_callback"); *(void **)(&setTid) = dlsym(h, "CRYPTO_THREADID_set_callback");
  if (!num || !setLock) { fprintf(stderr, "[net] tls: OpenSSL locks by itself\n"); return; }
  if (getLock && getLock()) { fprintf(stderr, "[net] tls: OpenSSL already has locks\n"); return; }
  int n = num(); if (n <= 0) return; sslLocks = calloc((size_t)n, sizeof *sslLocks); if (!sslLocks) return;
  for (int i = 0; i < n; i++) pthread_mutex_init(&sslLocks[i], NULL);
  *(void **)(&sslSetNum) = dlsym(h, "CRYPTO_THREADID_set_numeric"); /* from the library itself: it is not in the global names */
  if (setTid && sslSetNum) setTid(ssl_threadid); else if (setId) setId(ssl_id); else { fprintf(stderr, "[net] tls: no thread id call: left as it was\n"); return; }
  setLock(ssl_lock); fprintf(stderr, "[net] tls: OpenSSL 1.0 given %d locks for the request threads\n", n);
}
#endif
int net_init(const char *ca) {
  (void)ua(); /* made once here, before any worker thread reads it */
  if (!resolve()) return 0;
#if !SB_STATIC_CURL
  ssl_locks(); /* before the first request and the worker threads */
#endif
  f_global_init(CURL_GLOBAL_ALL);
  if (ca) snprintf(caPath, sizeof caPath, "%s", ca);
  keep_limits(); share_make();
  for (int i = 0; i < WORKERS; i++) { pthread_t t; pthread_create(&t, NULL, worker, (void *)(size_t)i); pthread_detach(t); }
  { pthread_t t; pthread_create(&t, NULL, file_worker, NULL); pthread_detach(t); }
  return 1;
}
static void push_req(Req *r) { /* under mu */
  if (r->bg) { if (bgTail) bgTail->next = r; else bgQ = r; bgTail = r; pthread_cond_signal(&bgCv); }
  else if (r->timeoutMs > 20000) { if (slowTail) slowTail->next = r; else slowQ = r; slowTail = r; pthread_cond_signal(&slowCv); }
  else { if (queueTail) queueTail->next = r; else queue = r; queueTail = r; pthread_cond_signal(&cv); }
}
static int bgNext; /* the next request goes to the previews' lane */
static unsigned enqueue_x(const char *url, const char *hdr, const char *post, const char *ctype, long timeoutMs, int size, NetCb cb, void *ud) {
  Req *r = calloc(1, sizeof *r); r->bg = bgNext; bgNext = 0; r->url = strdup(url); r->hdr = hdr ? strdup(hdr) : NULL; r->post = post ? strdup(post) : NULL; r->ctype = ctype ? strdup(ctype) : NULL; r->cb = cb; r->ud = ud; r->timeoutMs = timeoutMs; r->size = size;
  pthread_mutex_lock(&mu);
  r->id = nextId++; push_req(r); npending++;
  pthread_mutex_unlock(&mu);
  return r->id;
}
static unsigned enqueue_c(const char *url, const char *hdr, const char *post, const char *ctype, long timeoutMs, NetCb cb, void *ud) { return enqueue_x(url, hdr, post, ctype, timeoutMs, 0, cb, ud); }
static unsigned enqueue_t(const char *url, const char *hdr, const char *post, long timeoutMs, NetCb cb, void *ud) { return enqueue_x(url, hdr, post, NULL, timeoutMs, 0, cb, ud); }
static unsigned enqueue(const char *url, const char *hdr, const char *post, NetCb cb, void *ud) { return enqueue_t(url, hdr, post, 0, cb, ud); }
unsigned net_get_timeout(const char *url, int timeoutMs, NetCb cb, void *ud) { return enqueue_t(url, NULL, NULL, timeoutMs, cb, ud); }
unsigned net_get(const char *url, NetCb cb, void *ud) { return enqueue(url, NULL, NULL, cb, ud); }
unsigned net_get_hdr(const char *url, const char *hdr, NetCb cb, void *ud) { return enqueue(url, hdr, NULL, cb, ud); }
unsigned net_size(const char *url, NetCb cb, void *ud) { return enqueue_x(url, NULL, NULL, NULL, 10000, 1, cb, ud); }
unsigned net_resolve(const char *url, NetCb cb, void *ud) { bgNext = 1; return enqueue_x(url, NULL, NULL, NULL, 10000, 2, cb, ud); }
unsigned net_get_bg(const char *url, int timeoutMs, NetCb cb, void *ud) { bgNext = 1; return enqueue_x(url, NULL, NULL, NULL, timeoutMs, 0, cb, ud); }
unsigned net_post_bg(const char *url, const char *body, const char *contentType, const char *headers, int timeoutMs, NetCb cb, void *ud) { bgNext = 1; return enqueue_c(url, headers, body ? body : "", contentType ? contentType : "", timeoutMs, cb, ud); }
unsigned net_post_json(const char *url, const char *json, NetCb cb, void *ud) { return enqueue(url, NULL, json, cb, ud); }
unsigned net_post_hdr_t(const char *url, const char *body, const char *contentType, const char *headers, int timeoutMs, NetCb cb, void *ud) {
  return enqueue_c(url, headers, body ? body : "", contentType ? contentType : "", timeoutMs, cb, ud);
}
unsigned net_send(const char *method, const char *url, const char *headers, const void *body, size_t len, int timeoutMs, NetCb cb, void *ud) {
  Req *r = calloc(1, sizeof *r); r->url = strdup(url); r->hdr = headers ? strdup(headers) : NULL; r->method = strcmp(method, "POST") ? strdup(method) : NULL; r->cb = cb; r->ud = ud; r->timeoutMs = timeoutMs;
  r->post = malloc(len + 1); memcpy(r->post, body, len); r->post[len] = 0; r->postLen = len;
  pthread_mutex_lock(&mu);
  r->id = nextId++; push_req(r); npending++;
  pthread_mutex_unlock(&mu);
  return r->id;
}
/* only while it waits in a queue: a request already running finishes, so its kept connection is not torn down */
void net_cancel_queued(unsigned id) {
  pthread_mutex_lock(&mu);
  for (Req *r = queue; r; r = r->next) if (r->id == id) r->cancelled = 1;
  for (Req *r = slowQ; r; r = r->next) if (r->id == id) r->cancelled = 1;
  for (Req *r = bgQ; r; r = r->next) if (r->id == id) r->cancelled = 1;
  pthread_mutex_unlock(&mu);
}
void net_cancel(unsigned id) {
  pthread_mutex_lock(&mu);
  for (Req *r = queue; r; r = r->next) if (r->id == id) r->cancelled = 1;
  for (Req *r = slowQ; r; r = r->next) if (r->id == id) r->cancelled = 1;
  for (Req *r = bgQ; r; r = r->next) if (r->id == id) r->cancelled = 1;
  for (Req *r = fqueue; r; r = r->next) if (r->id == id) r->cancelled = 1;
  for (int i = 0; i < WORKERS; i++) if (inflight[i] && inflight[i]->id == id) inflight[i]->cancelled = 1;
  for (Req *r = finished; r; r = r->next) if (r->id == id) r->cancelled = 1;
  pthread_mutex_unlock(&mu);
}
int net_pending(void) { return npending; }
/* parsed answers are freed on a thread of their own: deleting a catalog's tree (thousands of small blocks, made by a
   worker) took 10-25 ms of a frame on the TV */
typedef struct JF { cJSON *j; struct JF *next; } JF;
static JF *jfHead; static pthread_mutex_t jfMu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t jfCv = PTHREAD_COND_INITIALIZER; static int jfThread;
static void *jf_run(void *a) { plat_thread_background(15); (void)a; for (;;) { pthread_mutex_lock(&jfMu); while (!jfHead) pthread_cond_wait(&jfCv, &jfMu); JF *f = jfHead; jfHead = NULL; pthread_mutex_unlock(&jfMu);
    while (f) { JF *n = f->next; cJSON_Delete(f->j); free(f); f = n; } } return NULL; }
void net_json_free(cJSON *j) {
  if (!j) return;
  pthread_mutex_lock(&jfMu);
  if (!jfThread) { pthread_t t; if (pthread_create(&t, NULL, jf_run, NULL) == 0) { pthread_detach(t); jfThread = 1; } }
  if (!jfThread) { pthread_mutex_unlock(&jfMu); cJSON_Delete(j); return; }
  JF *f = malloc(sizeof *f); f->j = j; f->next = jfHead; jfHead = f; pthread_cond_signal(&jfCv); pthread_mutex_unlock(&jfMu);
}
static const unsigned char *curBody; static cJSON *curJson;
cJSON *net_json(const unsigned char *body) {
  if (body && body == curBody && curJson) { cJSON *j = curJson; curJson = NULL; return j; } /* the tree the worker made: the caller owns it */
  return body ? cJSON_Parse((const char *)body) : NULL;
}
void net_pump(void) {
  pthread_mutex_lock(&mu); Req *list = finished; finished = NULL; pthread_mutex_unlock(&mu);
  /* finished is LIFO; deliver in completion order */
  Req *rev = NULL; while (list) { Req *n = list->next; list->next = rev; rev = list; list = n; }
  while (rev) {
    Req *r = rev; rev = r->next;
    curBody = r->body; curJson = r->json; r->json = NULL;
    { static int slowLog = -1; if (slowLog < 0) slowLog = getenv("SB_SLOWLOG") != NULL; Uint32 tc0 = slowLog ? SDL_GetTicks() : 0;
      if (r->cb) { if (r->cancelled) r->cb(-1, NULL, 0, r->ud); else r->cb(r->status, r->body ? r->body : (const unsigned char *)"", r->len, r->ud); }
      if (slowLog && SDL_GetTicks() - tc0 > 6) fprintf(stderr, "[netcb] %u ms for %.120s (%zu bytes)\n", SDL_GetTicks() - tc0, r->url, r->len); } /* development: which answers cost a frame */
    if (curJson) net_json_free(curJson); curJson = NULL; curBody = NULL; /* the callback did not want the tree */
    pthread_mutex_lock(&mu); npending--; pthread_mutex_unlock(&mu);
    free(r->url); free(r->hdr); free(r->post); free(r->ctype); free(r->method); free(r->body); free(r);
  }
}
unsigned char *net_fetch_sync(const char *url, size_t *len, int *status, int timeoutMs) { return net_fetch_sync_hdr(url, NULL, len, status, timeoutMs); }
/* a thread that fetches many small things from the same few hosts (the picture workers) keeps one handle: a new TLS connection per poster
   took the TV's connections down when a long Home was scrolled (curl 7, couldn't connect). Bounded like the other kept handles. */
static __thread int thKeep; static __thread CURL *thH;
void net_keep_this_thread(void) { thKeep = 1; }
unsigned char *net_fetch_sync_hdr(const char *url, const char *hdr, size_t *len, int *status, int timeoutMs) {
  Req r; memset(&r, 0, sizeof r); r.url = (char *)url;
  int kept = thKeep && f_easy_reset; CURL *c; if (kept) { c = thH; if (c) f_easy_reset(c); else c = thH = f_easy_init(); } else c = f_easy_init();
  if (!c) { *status = 0; *len = 0; return NULL; }
  if (kept) { f_easy_setopt(c, CURLOPT_MAXCONNECTS, (long)(keepConns ? keepConns : 2)); f_easy_setopt(c, CURLOPT_DNS_CACHE_TIMEOUT, 300L); }
  struct curl_slist *hs = hdr ? f_slist_append(NULL, hdr) : NULL; if (hs) f_easy_setopt(c, CURLOPT_HTTPHEADER, hs);
  f_easy_setopt(c, CURLOPT_URL, url); f_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data); f_easy_setopt(c, CURLOPT_WRITEDATA, &r);
  f_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L); f_easy_setopt(c, CURLOPT_MAXREDIRS, 8L); f_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  f_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 8000L); f_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)timeoutMs);
  f_easy_setopt(c, CURLOPT_USERAGENT, ua()); f_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
  if (caPath[0]) f_easy_setopt(c, CURLOPT_CAINFO, caPath);
  char eb[CURL_ERROR_SIZE]; eb[0] = 0; f_easy_setopt(c, CURLOPT_ERRORBUFFER, eb); /* curl's own words for a failure, for the log */
  CURLcode rc = f_easy_perform(c); long code = 0; f_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code); f_easy_setopt(c, CURLOPT_ERRORBUFFER, (char *)NULL);
  if (kept) net_stats(c); else f_easy_cleanup(c); if (hs) f_slist_free_all(hs);
  *status = rc == CURLE_OK ? (int)code : 0; *len = r.len;
  if (*status != 200 && *status != 206) { { static unsigned nf; if (nf++ < 200 || nf % 50 == 0) fprintf(stderr, "[net] fetch failed: curl %d, http %ld, %zu bytes%s%s\n", (int)rc, code, r.len, eb[0] ? ": " : "", eb); } free(r.body); return NULL; }
  return r.body;
}

/* ---- a download read as it arrives (worker threads only: it blocks) ---------------------------- */
typedef struct { NetSink sink; void *ud; int stopped; } StreamCtx;
static size_t stream_data(char *p, size_t s, size_t n, void *ud) { StreamCtx *x = ud; if (!x->sink((const unsigned char *)p, s * n, x->ud)) { x->stopped = 1; return 0; } return s * n; }
static int stream_poll(void *ud, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) { (void)a; (void)b; (void)c; (void)d; StreamCtx *x = ud; if (!x->sink(NULL, 0, x->ud)) { x->stopped = 1; return 1; } return 0; } /* a stalled transfer can still be stopped */
int net_stream_sync(const char *url, int timeoutMs, NetSink sink, void *ud) {
  if (!f_easy_init && !resolve()) return 0;
  CURL *c = f_easy_init(); if (!c) return 0;
  StreamCtx x = { sink, ud, 0 };
  f_easy_setopt(c, CURLOPT_URL, url); f_easy_setopt(c, CURLOPT_WRITEFUNCTION, stream_data); f_easy_setopt(c, CURLOPT_WRITEDATA, &x);
  f_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, stream_poll); f_easy_setopt(c, CURLOPT_XFERINFODATA, &x); f_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  f_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L); f_easy_setopt(c, CURLOPT_MAXREDIRS, 8L); f_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  f_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 8000L); f_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)timeoutMs);
  f_easy_setopt(c, CURLOPT_USERAGENT, ua()); /* no Accept-Encoding: the bytes counted are the bytes that crossed the line */
  if (caPath[0]) f_easy_setopt(c, CURLOPT_CAINFO, caPath);
  CURLcode rc = f_easy_perform(c); long code = 0; f_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code); f_easy_cleanup(c);
  if (x.stopped) return -1;
  return rc == CURLE_OK ? (int)code : 0;
}

/* ---- a GET passed through as it arrives, with the caller's headers (the stream proxy: worker threads only) ---------- */
typedef struct { NetHdrSink hs; NetSink sink; void *ud; int stopped; } PipeCtx;
static size_t pipe_hdr(char *p, size_t s, size_t n, void *ud) { PipeCtx *x = ud; size_t l = s * n; char line[1024]; size_t k = l < sizeof line - 1 ? l : sizeof line - 1; memcpy(line, p, k); line[k] = 0; x->hs(line, x->ud); return l; }
static size_t pipe_data(char *p, size_t s, size_t n, void *ud) { PipeCtx *x = ud; if (!x->sink((const unsigned char *)p, s * n, x->ud)) { x->stopped = 1; return 0; } return s * n; }
static int pipe_poll(void *ud, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) { (void)a; (void)b; (void)c; (void)d; PipeCtx *x = ud; if (!x->sink(NULL, 0, x->ud)) { x->stopped = 1; return 1; } return 0; }
int net_pipe_sync(const char *url, const char *headers, int head, NetHdrSink hs, NetSink sink, void *ud, int stallSec) {
  if (!f_easy_init && !resolve()) return 0;
  CURL *c = f_easy_init(); if (!c) return 0;
  PipeCtx x = { hs, sink, ud, 0 }; struct curl_slist *sl = NULL;
  char *hd = headers ? strdup(headers) : NULL; for (char *h = hd, *nl; h && *h; h = nl ? nl + 1 : NULL) { nl = strchr(h, '\n'); if (nl) *nl = 0; if (*h) sl = f_slist_append(sl, h); }
  f_easy_setopt(c, CURLOPT_URL, url); f_easy_setopt(c, CURLOPT_HEADERFUNCTION, pipe_hdr); f_easy_setopt(c, CURLOPT_HEADERDATA, &x);
  f_easy_setopt(c, CURLOPT_WRITEFUNCTION, pipe_data); f_easy_setopt(c, CURLOPT_WRITEDATA, &x);
  f_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, pipe_poll); f_easy_setopt(c, CURLOPT_XFERINFODATA, &x); f_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  f_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L); f_easy_setopt(c, CURLOPT_MAXREDIRS, 8L); f_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  f_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 10000L); f_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L); f_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, (long)(stallSec > 0 ? stallSec : 60));
  f_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0 (Web0S; Linux) StremioOrigin"); /* a User-Agent among the headers replaces it */
  if (head) f_easy_setopt(c, CURLOPT_NOBODY, 1L);
  if (sl) f_easy_setopt(c, CURLOPT_HTTPHEADER, sl);
  if (caPath[0]) f_easy_setopt(c, CURLOPT_CAINFO, caPath);
  CURLcode rc = f_easy_perform(c); long code = 0; f_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code); f_easy_cleanup(c); if (sl) f_slist_free_all(sl); free(hd);
  if (x.stopped) return -1;
  return rc == CURLE_OK ? (int)code : 0;
}

/* ---- ranged reads ---------------------------------------------------------------------------- */
struct NetRange { CURL *c; };
typedef struct { unsigned char *buf; long len, got; int status, isRange; long long reqStart, crStart, total; int norange; } RangeCtx;
static size_t range_hdr(char *p, size_t s, size_t n, void *ud) {
  RangeCtx *r = ud; size_t l = s * n; char line[512]; size_t k = l < sizeof line - 1 ? l : sizeof line - 1; memcpy(line, p, k); line[k] = 0;
  int code; if (sscanf(line, "HTTP/%*s %d", &code) == 1) { r->status = code; r->crStart = -1; } /* each redirect hop starts again */
  else if (!strncasecmp(line, "content-range:", 14)) { long long a, b, t; char star[4];
    if (sscanf(line + 14, " bytes %lld-%lld/%lld", &a, &b, &t) == 3) { r->crStart = a; r->total = t; }
    else if (sscanf(line + 14, " bytes %lld-%lld/%1s", &a, &b, star) == 3) r->crStart = a; }
  else if (!strncasecmp(line, "content-length:", 15) && r->status == 200) { long long cl = atoll(line + 15); if (cl > 0) r->total = cl; }
  return l;
}
static size_t range_data(char *p, size_t s, size_t n, void *ud) {
  RangeCtx *r = ud; size_t add = s * n;
  if (r->status >= 300 && r->status < 400) return add;                    /* a redirect's body */
  if (r->status == 200 && r->isRange) { r->norange = 1; return 0; }       /* the server ignored the Range */
  if (r->status == 206 && r->crStart >= 0 && r->crStart != r->reqStart) { r->norange = 1; return 0; } /* another part than asked */
  long need = r->len - r->got; if (need <= 0) return 0;
  long take = (long)add < need ? (long)add : need;
  memcpy(r->buf + r->got, p, take); r->got += take;
  return r->got >= r->len ? 0 : add;                                      /* had what was asked: stop (a 200 for the whole file must not keep downloading) */
}
NetRange *net_range_new(void) { if (!f_easy_init && !resolve()) return NULL; NetRange *r = calloc(1, sizeof *r); r->c = f_easy_init(); if (!r->c) { free(r); return NULL; } return r; }
void net_range_free(NetRange *r) { if (!r) return; if (r->c) f_easy_cleanup(r->c); free(r); }
int net_range_get(NetRange *h, const char *url, long long start, long len, unsigned char **out, long *got, long long *total, int *httpStatus, int timeoutMs) {
  *out = NULL; *got = 0; if (total) *total = -1; if (httpStatus) *httpStatus = 0;
  if (!h || len <= 0) return NR_NET;
  RangeCtx r; memset(&r, 0, sizeof r); r.buf = malloc(len); r.len = len; r.total = -1; r.crStart = -1; r.reqStart = start;
  r.isRange = start > 0; /* asked from past the start: a 200 means no Range support */
  char rg[64]; snprintf(rg, sizeof rg, "%lld-%lld", start, start + len - 1);
  CURL *c = h->c;
  f_easy_setopt(c, CURLOPT_URL, url); f_easy_setopt(c, CURLOPT_RANGE, rg);
  f_easy_setopt(c, CURLOPT_HEADERFUNCTION, range_hdr); f_easy_setopt(c, CURLOPT_HEADERDATA, &r);
  f_easy_setopt(c, CURLOPT_WRITEFUNCTION, range_data); f_easy_setopt(c, CURLOPT_WRITEDATA, &r);
  f_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L); f_easy_setopt(c, CURLOPT_MAXREDIRS, 5L); f_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  f_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 8000L); f_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)timeoutMs);
  f_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0 (Web0S; Linux) StremioOrigin"); f_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
  if (caPath[0]) f_easy_setopt(c, CURLOPT_CAINFO, caPath);
  CURLcode rc = f_easy_perform(c);
  if (httpStatus) *httpStatus = r.status;
  if (total) *total = r.total;
  if (r.norange) { free(r.buf); return NR_NORANGE; }
  if (r.got > 0 && (r.status == 200 || r.status == 206)) { *out = r.buf; *got = r.got; return NR_OK; } /* full, or a server that cut the response: what came */
  free(r.buf);
  if (rc == CURLE_OPERATION_TIMEDOUT) return NR_TIMEOUT;
  if (r.status == 429 || r.status >= 500) return NR_BUSY;
  if (r.status >= 400) return NR_HTTP;
  return NR_NET;
}
