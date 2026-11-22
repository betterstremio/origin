#ifdef _WIN32
#include <winsock2.h>
#endif
#include "platform/plat.h"
#include "core/net.h"
#include "core/update.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <stdint.h>
#include <curl/curl.h>
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif
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
static CURLSH *(*f_share_init)(void);
static CURLSHcode (*f_share_setopt)(CURLSH *, CURLSHoption, ...); /* (optional too) */
static curl_version_info_data *(*f_version_info)(CURLversion); /* optional: without it use blocking workers */
static CURLM *(*f_multi_init)(void);
static CURLMcode (*f_multi_cleanup)(CURLM *);
static CURLMcode (*f_multi_setopt)(CURLM *, CURLMoption, ...);
static CURLMcode (*f_multi_add_handle)(CURLM *, CURL *);
static CURLMcode (*f_multi_remove_handle)(CURLM *, CURL *);
static CURLMcode (*f_multi_perform)(CURLM *, int *);
static CURLMcode (*f_multi_wait)(CURLM *, struct curl_waitfd *, unsigned int, int, int *);
static CURLMsg *(*f_multi_info_read)(CURLM *, int *);
static char caPath[1024];
/* "StremioOrigin/1.0.0 (webOS; native)": the running version, the platform it runs on */
static const char *ua(void) {
  static char u[96];
  if (!u[0]) {
    const char *d = plat_device_name();
    snprintf(u, sizeof u, "StremioOrigin/%s (%s; native)", update_current(),
             strstr(d, "webOS")     ? "webOS"
             : strstr(d, "Android") ? "Android TV"
                                    : "Mac");
  }
  return u;
}

static int resolve(void) {
#if SB_STATIC_CURL
  f_easy_init = curl_easy_init;
  f_easy_setopt = curl_easy_setopt;
  f_easy_perform = curl_easy_perform;
  f_easy_getinfo = curl_easy_getinfo;
  f_easy_cleanup = curl_easy_cleanup;
  f_slist_append = curl_slist_append;
  f_slist_free_all = curl_slist_free_all;
  f_global_init = curl_global_init;
  f_easy_reset = curl_easy_reset;
  f_share_init = curl_share_init;
  f_share_setopt = curl_share_setopt;
  f_version_info = curl_version_info;
  f_multi_init = curl_multi_init;
  f_multi_cleanup = curl_multi_cleanup;
  f_multi_setopt = curl_multi_setopt;
  f_multi_add_handle = curl_multi_add_handle;
  f_multi_remove_handle = curl_multi_remove_handle;
  f_multi_perform = curl_multi_perform;
  f_multi_wait = curl_multi_wait;
  f_multi_info_read = curl_multi_info_read;
  return 1;
#else
  static const char *names[] = { "libcurl.so.5", "libcurl.so.4", "libcurl.so", NULL };
  void *h = NULL;
  for (int i = 0; names[i] && !h; i++)
    h = dlopen(names[i], RTLD_NOW);
  if (!h) {
    fprintf(stderr, "[net] no libcurl: %s\n", dlerror());
    return 0;
  }
#define L(v, n)                                 \
  do {                                          \
    *(void **)(&v) = dlsym(h, n);               \
    if (!v) {                                   \
      fprintf(stderr, "[net] missing %s\n", n); \
      return 0;                                 \
    }                                           \
  } while (0)
  L(f_easy_init, "curl_easy_init");
  L(f_easy_setopt, "curl_easy_setopt");
  L(f_easy_perform, "curl_easy_perform");
  L(f_easy_getinfo, "curl_easy_getinfo");
  L(f_easy_cleanup, "curl_easy_cleanup");
  L(f_slist_append, "curl_slist_append");
  L(f_slist_free_all, "curl_slist_free_all");
  L(f_global_init, "curl_global_init");
#undef L
  *(void **)(&f_easy_reset) = dlsym(h, "curl_easy_reset");
  *(void **)(&f_share_init) = dlsym(h, "curl_share_init");
  *(void **)(&f_share_setopt) = dlsym(h, "curl_share_setopt");
  *(void **)(&f_version_info) = dlsym(h, "curl_version_info");
#define M(name) *(void **)(&f_multi_##name) = dlsym(h, "curl_multi_" #name)
  M(init);
  M(cleanup);
  M(setopt);
  M(add_handle);
  M(remove_handle);
  M(perform);
  M(wait);
  M(info_read);
#undef M
  return 1;
#endif
}

/* ---- requests ------------------------------------------------------------------------------- */
typedef struct Req {
  unsigned id;
  char *url, *hdr, *post, *ctype, *method;
  size_t postLen;
  NetCb cb;
  void *ud;
  long timeoutMs;
  cJSON *json; /* the body already parsed on the worker */
  int status;
  unsigned char *body;
  size_t len, cap;
  int cancelled; /* under mu */
  int size;
  double total; /* net_size: only the headers are read (2: net_resolve, the final address kept) */
  int bg; /* the previews' lane */
  char *eff;
  int file; /* net_file: url is a local path */
  struct Req *next;
} Req;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static Req *queue, *queueTail, *slowQ, *slowTail, *bgQ, *bgTail, *finished, *finishedTail, *fqueue, *fqueueTail;
static Req *fileInflight;
static pthread_cond_t slowCv = PTHREAD_COND_INITIALIZER, bgCv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t fcv = PTHREAD_COND_INITIALIZER;
static unsigned nextId = 1;
static int npending;
#define WORKERS 16
#define SLOW0 5 /* 0-4: quick requests; 5-7: blocking fallback for the slow lane */
#define BG0                                                                                                          \
  8 /* 8-15: the previews' lane (stream lookups and link resolving for a card at rest): never the slots a title page \
       waits on. Eight, because every addon is asked at once: with two, an addon that never answers held a slot for  \
       its whole timeout and the fast ones waited behind it */
static Req *inflight[WORKERS];
#define SLOW_MAX 32
typedef struct {
  Req *req; /* published under mu; only the multi thread touches the easy handle and headers */
  CURL *easy;
  struct curl_slist *headers;
} SlowTransfer;
static SlowTransfer slowActive[SLOW_MAX];
static int slowLimit;
static curl_socket_t slowWake[2] = { CURL_SOCKET_BAD, CURL_SOCKET_BAD };
static int slowWakePending; /* under mu, at most one byte in the wake descriptor */

static int is_cancelled(const Req *r) {
  pthread_mutex_lock(&mu);
  int cancelled = r->cancelled;
  pthread_mutex_unlock(&mu);
  return cancelled;
}

static size_t on_data(char *p, size_t s, size_t n, void *ud) {
  Req *r = ud;
  size_t add = s * n;
  if (r->id && is_cancelled(r))
    return 0;
  if (add > SIZE_MAX - r->len - 1)
    return 0;
  size_t need = r->len + add + 1;
  if (need > r->cap) {
    size_t cap = need <= SIZE_MAX / 2 ? need * 2 : need;
    unsigned char *body = realloc(r->body, cap);
    if (!body)
      return 0;
    r->body = body;
    r->cap = cap;
  }
  memcpy(r->body + r->len, p, add);
  r->len += add;
  r->body[r->len] = 0;
  return add;
}
/* net_size: "Content-Range: bytes 0-0/<total>" (a 200 instead: its Content-Length); each redirect starts over */
static size_t on_header(char *p, size_t s, size_t n, void *ud) {
  Req *r = ud;
  size_t len = s * n;
  char h[256];
  size_t k = len < sizeof h - 1 ? len : sizeof h - 1;
  memcpy(h, p, k);
  h[k] = 0;
  if (!strncmp(h, "HTTP/", 5)) {
    r->total = 0;
    r->status = atoi(strchr(h, ' ') ? strchr(h, ' ') + 1 : "0");
  } else if (!strncasecmp(h, "content-range:", 14)) {
    char *sl = strrchr(h, '/');
    if (sl)
      r->total = atof(sl + 1);
  } else if (!strncasecmp(h, "content-length:", 15) && r->status == 200)
    r->total = atof(h + 15);
  return len;
}
static size_t on_size_data(char *p, size_t s, size_t n, void *ud) {
  (void)p;
  (void)s;
  (void)n;
  (void)ud;
  return 0;
} /* never the body: a 200 would be the whole film */
/* A worker keeps its curl handle: the connections it opened (with their TLS sessions and DNS answers) stay, and the
   next request to the same addon or API skips the handshake, the slowest part of a request on a TV. Every kept
   connection holds memory, so it is bounded: a device under 1200 MB (a Firestick) keeps them on the quick lane only,
   one per worker. */
static CURL *handles[WORKERS];
static int keepSlots, keepConns;
static void keep_limits(void) {
  int mb = plat_mem_total_mb(), lowMemory = mb > 0 && mb < 1200;
  slowLimit = mb < 1200 ? 16 : SLOW_MAX;
  keepSlots = !f_easy_reset ? 0 : lowMemory ? SLOW0 : WORKERS;
  keepConns = lowMemory ? 1 : 2;
  fprintf(stderr, "[net] kept connections: %d workers, up to %d each\n", keepSlots, keepConns);
}
/* DNS answers and TLS sessions shared by every worker: a host one worker has met costs another no lookup, and its TLS
   resumes (a full handshake is the slow part on a TV's CPU). Small and fixed: curl keeps a few dozen of each */
static CURLSH *share;
static pthread_mutex_t shareMu[CURL_LOCK_DATA_LAST];
static void share_lock(CURL *c, curl_lock_data d, curl_lock_access a, void *u) {
  (void)c;
  (void)a;
  (void)u;
  if ((int)d >= 0 && d < CURL_LOCK_DATA_LAST)
    pthread_mutex_lock(&shareMu[d]);
}
static void share_unlock(CURL *c, curl_lock_data d, void *u) {
  (void)c;
  (void)u;
  if ((int)d >= 0 && d < CURL_LOCK_DATA_LAST)
    pthread_mutex_unlock(&shareMu[d]);
}
static void share_make(void) {
  if (!f_share_init || !f_share_setopt || !(share = f_share_init())) {
    fprintf(stderr, "[net] no shared DNS and TLS sessions\n");
    return;
  }
  for (int i = 0; i < CURL_LOCK_DATA_LAST; i++)
    pthread_mutex_init(&shareMu[i], NULL);
  f_share_setopt(share, CURLSHOPT_LOCKFUNC, share_lock);
  f_share_setopt(share, CURLSHOPT_UNLOCKFUNC, share_unlock);
  f_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
  f_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
}
static int nReq, nConn, nShake;
static double shakeSum;
static void net_stats(CURL *c) {
  long nc = 0;
  double t = 0;
  f_easy_getinfo(c, CURLINFO_NUM_CONNECTS, &nc);
  f_easy_getinfo(c, CURLINFO_APPCONNECT_TIME, &t);
  pthread_mutex_lock(&mu);
  nReq++;
  if (nc > 0)
    nConn++;
  if (nc > 0 && t > 0) {
    nShake++;
    shakeSum += t;
  }
  if (nReq % 40 == 0)
    fprintf(stderr, "[net] %d requests, %d opened a connection, handshake average %.0f ms\n", nReq, nConn,
            nShake ? shakeSum * 1000 / nShake : 0.0);
  pthread_mutex_unlock(&mu);
}
static int append_header(struct curl_slist **hs, const char *line) {
  struct curl_slist *next = f_slist_append(*hs, line);
  if (!next)
    return 0;
  *hs = next;
  return 1;
}
static int prepare(CURL *c, Req *r, struct curl_slist **hs) {
  if (share)
    f_easy_setopt(c, CURLOPT_SHARE, share);
  f_easy_setopt(c, CURLOPT_URL, r->url);
  f_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
  f_easy_setopt(c, CURLOPT_WRITEDATA, r);
  f_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  f_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
  f_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  f_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
  f_easy_setopt(c, CURLOPT_TIMEOUT_MS, r->timeoutMs > 0 ? r->timeoutMs : 20000L);
  f_easy_setopt(c, CURLOPT_USERAGENT, ua());
  if (!r->size)
    f_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
  if (caPath[0])
    f_easy_setopt(c, CURLOPT_CAINFO, caPath);
  if (r->size) {
    f_easy_setopt(c, CURLOPT_TIMEOUT_MS, 10000L);
    f_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_header);
    f_easy_setopt(c, CURLOPT_HEADERDATA, r);
    f_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_size_data);
    if (!append_header(hs, "Range: bytes=0-0"))
      return 0;
  }
  /* one header per line; one the caller names (User-Agent...) replaces curl's own */
  for (char *h = r->hdr, *nl; h && *h; h = nl ? nl + 1 : NULL) {
    nl = strchr(h, '\n');
    if (nl)
      *nl = 0;
    int ok = !*h || append_header(hs, h);
    if (nl)
      *nl = '\n';
    if (!ok)
      return 0;
  }
  if (r->post) {
    if (r->ctype && *r->ctype) {
      char ct[160];
      snprintf(ct, sizeof ct, "Content-Type: %s", r->ctype);
      if (!append_header(hs, ct))
        return 0;
    }
    f_easy_setopt(c, CURLOPT_POSTFIELDS, r->post);
    f_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)r->postLen);
  }
  if (r->method)
    f_easy_setopt(c, CURLOPT_CUSTOMREQUEST, r->method);
  return !*hs || f_easy_setopt(c, CURLOPT_HTTPHEADER, *hs) == CURLE_OK;
}
static void read_result(CURL *c, Req *r, CURLcode rc) {
  long code = 0;
  f_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  r->status = rc == CURLE_OK ? (int)code : 0;
  if (r->size == 2) {
    char *e = NULL;
    f_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &e);
    free(r->body);
    r->body = (unsigned char *)strdup(e ? e : "");
    r->len = strlen((char *)r->body);
    r->status = (int)code;
  } else if (r->size) {
    char t[32];
    int k = snprintf(t, sizeof t, "%.0f", r->total > 0 ? r->total : 0);
    free(r->body);
    r->body = (unsigned char *)strdup(t);
    r->len = (size_t)k;
    r->status = (int)code;
  } /* the server's answer even without a size (a 404 says why a film will not play); 0 = no answer */
  net_stats(c);
}
static int cancel_progress(void *ud, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  return is_cancelled(ud);
}
static void perform(Req *r, int slot) {
  int kept = slot >= 0 && slot < keepSlots;
  CURL *c = kept ? handles[slot] : NULL;
  if (c)
    f_easy_reset(c);
  else
    c = f_easy_init();
  if (!c)
    return;
  if (kept) {
    handles[slot] = c;
    f_easy_setopt(c, CURLOPT_MAXCONNECTS, (long)keepConns);
    f_easy_setopt(c, CURLOPT_DNS_CACHE_TIMEOUT, 300L);
  }
  struct curl_slist *hs = NULL;
  if (prepare(c, r, &hs)) {
    if (slot >= SLOW0 && slot < BG0 && f_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, cancel_progress) == CURLE_OK) {
      f_easy_setopt(c, CURLOPT_XFERINFODATA, r);
      f_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    }
    read_result(c, r, f_easy_perform(c));
  }
  if (!kept)
    f_easy_cleanup(c);
  if (hs)
    f_slist_free_all(hs);
}
static void parse_body(Req *r) {
  if (r->body && r->len > 1 && r->len < 16u * 1024 * 1024 && r->status >= 200 && r->status < 300) {
    const unsigned char *b = r->body;
    while (*b == ' ' || *b == '\n' || *b == '\r' || *b == '\t')
      b++;
    if (*b == '{' || *b == '[')
      r->json = cJSON_ParseWithLength((const char *)r->body, r->len);
  }
}
static Req *pop_req(Req **head, Req **tail) { /* under mu, with a nonempty queue */
  Req *r = *head;
  *head = r->next;
  if (!*head)
    *tail = NULL;
  r->next = NULL;
  return r;
}
static void finish_req(Req *r) { /* under mu */
  r->next = NULL;
  if (finishedTail)
    finishedTail->next = r;
  else
    finished = r;
  finishedTail = r;
}
/* disk copies get a thread of their own: behind six stream addons (45 s each) a cached title page would wait as long */
static void *file_worker(void *arg) {
  plat_thread_background(5);
  (void)arg;
  for (;;) {
    pthread_mutex_lock(&mu);
    while (!fqueue)
      pthread_cond_wait(&fcv, &mu);
    Req *r = pop_req(&fqueue, &fqueueTail);
    fileInflight = r;
    pthread_mutex_unlock(&mu);
    if (!is_cancelled(r)) {
      FILE *f = fopen(r->url, "rb");
      r->status = 404;
      if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && (r->body = malloc((size_t)n + 1)) && fread(r->body, 1, (size_t)n, f) == (size_t)n) {
          r->body[n] = 0;
          r->len = (size_t)n;
          r->status = 200;
        }
        fclose(f);
      }
      parse_body(r);
    }
    pthread_mutex_lock(&mu);
    fileInflight = NULL;
    finish_req(r);
    pthread_mutex_unlock(&mu);
  }
  return NULL;
}
unsigned net_file(const char *path, NetCb cb, void *ud) {
  Req *r = calloc(1, sizeof *r);
  r->url = strdup(path);
  r->cb = cb;
  r->ud = ud;
  r->file = 1;
  pthread_mutex_lock(&mu);
  unsigned id = r->id = nextId++;
  if (fqueueTail)
    fqueueTail->next = r;
  else
    fqueue = r;
  fqueueTail = r;
  npending++;
  pthread_cond_signal(&fcv);
  pthread_mutex_unlock(&mu);
  return id;
}
static void *worker(void *arg) {
  plat_thread_background(5);
  int slot = (int)(size_t)arg;
  for (;;) {
    pthread_mutex_lock(&mu);
    if (slot >= BG0)
      while (!bgQ)
        pthread_cond_wait(&bgCv, &mu);
    else if (slot >= SLOW0)
      while (!slowQ)
        pthread_cond_wait(&slowCv, &mu);
    else
      while (!queue)
        pthread_cond_wait(&cv, &mu);
    Req *r;
    if (slot >= BG0)
      r = pop_req(&bgQ, &bgTail);
    else if (slot >= SLOW0)
      r = pop_req(&slowQ, &slowTail);
    else
      r = pop_req(&queue, &queueTail);
    inflight[slot] = r;
    pthread_mutex_unlock(&mu);
    if (!is_cancelled(r)) {
      perform(r, slot);
      /* a JSON answer is parsed here, off the drawing thread (catalogs, metadata, streams: tens of ms each on a TV) */
      parse_body(r);
    }
    pthread_mutex_lock(&mu);
    inflight[slot] = NULL;
    finish_req(r);
    pthread_mutex_unlock(&mu);
  }
  return NULL;
}
static void start_worker(int slot) {
  pthread_t t;
  if (pthread_create(&t, NULL, worker, (void *)(size_t)slot) == 0)
    pthread_detach(t);
}

/* Give curl_multi_wait a descriptor even while curl has no sockets to poll. A queue or cancellation event
   wakes it immediately, including on curl versions that predate curl_multi_wakeup. */
static void slow_wake_close(void) {
  for (int i = 0; i < 2; i++) {
    if (slowWake[i] == CURL_SOCKET_BAD)
      continue;
#ifdef _WIN32
    closesocket(slowWake[i]);
#else
    close(slowWake[i]);
#endif
    slowWake[i] = CURL_SOCKET_BAD;
  }
}
static int slow_wake_open(void) {
#ifdef _WIN32
  struct sockaddr_in addr[2] = { 0 };
  unsigned long nonblock = 1;
  for (int i = 0; i < 2; i++) {
    int len = sizeof addr[i];
    addr[i].sin_family = AF_INET;
    addr[i].sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    slowWake[i] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (slowWake[i] == CURL_SOCKET_BAD || bind(slowWake[i], (struct sockaddr *)&addr[i], len) != 0 ||
        getsockname(slowWake[i], (struct sockaddr *)&addr[i], &len) != 0 ||
        ioctlsocket(slowWake[i], FIONBIO, &nonblock) != 0)
      return 0;
  }
  return connect(slowWake[0], (struct sockaddr *)&addr[1], sizeof addr[1]) == 0 &&
         connect(slowWake[1], (struct sockaddr *)&addr[0], sizeof addr[0]) == 0;
#else
  return pipe(slowWake) == 0 && fcntl(slowWake[0], F_SETFL, O_NONBLOCK) == 0 &&
         fcntl(slowWake[1], F_SETFL, O_NONBLOCK) == 0;
#endif
}
static void slow_notify(void) { /* under mu */
  if (slowWake[1] == CURL_SOCKET_BAD || slowWakePending)
    return;
#ifdef _WIN32
  slowWakePending = send(slowWake[1], "w", 1, 0) == 1;
#else
  ssize_t n;
  do {
    n = write(slowWake[1], "w", 1);
  } while (n < 0 && errno == EINTR);
  slowWakePending = n == 1;
#endif
}
static void slow_drain_wake(void) { /* under mu, before looking for new work or cancellations */
  if (!slowWakePending)
    return;
  char byte;
#ifdef _WIN32
  recv(slowWake[0], &byte, 1, 0);
#else
  while (read(slowWake[0], &byte, 1) < 0 && errno == EINTR)
    ;
#endif
  slowWakePending = 0;
}
static int slow_add(CURLM *m, SlowTransfer *t) {
  t->easy = f_easy_init();
  if (!t->easy)
    return 0;
  if (!prepare(t->easy, t->req, &t->headers))
    return 0;
  f_easy_setopt(t->easy, CURLOPT_DNS_CACHE_TIMEOUT, 300L);
  if (f_easy_setopt(t->easy, CURLOPT_PRIVATE, (void *)t) != CURLE_OK)
    return 0;
  return f_multi_add_handle(m, t->easy) == CURLM_OK;
}
static void slow_finish(SlowTransfer *t) {
  Req *r = t->req;
  if (t->easy)
    f_easy_cleanup(t->easy);
  if (t->headers)
    f_slist_free_all(t->headers);
  t->easy = NULL;
  t->headers = NULL;
  if (!is_cancelled(r))
    parse_body(r);
  pthread_mutex_lock(&mu);
  t->req = NULL;
  finish_req(r);
  pthread_mutex_unlock(&mu);
}
static void *slow_worker(void *arg) {
  CURLM *m = arg;
  int active = 0;
  CURLMcode rc;
  plat_thread_background(5);
  for (;;) {
    pthread_mutex_lock(&mu);
    while (!active && !slowQ)
      pthread_cond_wait(&slowCv, &mu);
    slow_drain_wake();
    pthread_mutex_unlock(&mu);

    for (int i = 0; i < slowLimit; i++) {
      SlowTransfer *t = &slowActive[i];
      if (t->req && is_cancelled(t->req)) {
        rc = f_multi_remove_handle(m, t->easy);
        if (rc != CURLM_OK)
          goto failed;
        slow_finish(t);
        active--;
      }
    }
    if (active < slowLimit) {
      pthread_mutex_lock(&mu);
      for (int i = 0; slowQ && i < slowLimit; i++)
        if (!slowActive[i].req)
          slowActive[i].req = pop_req(&slowQ, &slowTail);
      pthread_mutex_unlock(&mu);
    }
    int retry = 0;
    for (int i = 0; i < slowLimit; i++) {
      SlowTransfer *t = &slowActive[i];
      if (t->req && !t->easy) {
        if (!is_cancelled(t->req) && slow_add(m, t))
          active++;
        else {
          slow_finish(t);
          retry = 1; /* a skipped request left a slot free */
        }
      }
    }
    if (!active)
      continue;

    int running;
    do {
      rc = f_multi_perform(m, &running);
    } while (rc == CURLM_CALL_MULTI_PERFORM);
    if (rc != CURLM_OK)
      break;
    int left;
    CURLMsg *msg;
    while ((msg = f_multi_info_read(m, &left))) {
      if (msg->msg != CURLMSG_DONE)
        continue;
      char *slot = NULL;
      if (f_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &slot) != CURLE_OK || !slot) {
        rc = CURLM_INTERNAL_ERROR;
        goto failed;
      }
      SlowTransfer *t = (SlowTransfer *)slot;
      CURLcode result = msg->data.result;
      rc = f_multi_remove_handle(m, t->easy);
      if (rc != CURLM_OK)
        goto failed;
      read_result(t->easy, t->req, result);
      slow_finish(t);
      active--;
      retry = 1;
    }
    if (retry)
      continue;
    struct curl_waitfd wake = { slowWake[0], CURL_WAIT_POLLIN, 0 };
    rc = f_multi_wait(m, &wake, 1, 1000, NULL);
    if (rc != CURLM_OK)
      break;
  }

failed:
  fprintf(stderr, "[net] slow multi error %d, switching to blocking workers\n", (int)rc);
  for (int i = 0; i < slowLimit; i++) {
    SlowTransfer *t = &slowActive[i];
    if (t->req && (!t->easy || f_multi_remove_handle(m, t->easy) == CURLM_OK))
      slow_finish(t);
  }
  f_multi_cleanup(m);
  for (int i = 0; i < slowLimit; i++)
    if (slowActive[i].req)
      slow_finish(&slowActive[i]);
  pthread_mutex_lock(&mu);
  slow_wake_close();
  slowWakePending = 0;
  pthread_mutex_unlock(&mu);
  for (int i = SLOW0 + 1; i < BG0; i++)
    start_worker(i);
  return worker((void *)(size_t)SLOW0);
}
static int start_slow_worker(void) {
  if (!f_multi_init || !f_multi_cleanup || !f_multi_setopt || !f_multi_add_handle || !f_multi_remove_handle ||
      !f_multi_perform || !f_multi_wait || !f_multi_info_read)
    return 0;
  CURLM *m = f_multi_init();
  if (!m)
    return 0;
  pthread_t t;
  if (f_multi_setopt(m, CURLMOPT_MAXCONNECTS, (long)keepConns) != CURLM_OK || !slow_wake_open() ||
      pthread_create(&t, NULL, slow_worker, m) != 0) {
    slow_wake_close();
    f_multi_cleanup(m);
    return 0;
  }
  pthread_detach(t);
  return 1;
}
#if !SB_STATIC_CURL
/* OpenSSL 1.0 (the TV's, under its libcurl) is safe across threads only with locks the app hands it: without them
   its shared tables were changed by two requests at once, and a long session ended in a crash inside it (a bad
   pointer in memcmp, called from libcrypto). 1.1 and later lock by themselves (no CRYPTO_num_locks to find): left as
   they are, as is a library someone already gave locks to */
static pthread_mutex_t *sslLocks;
static void ssl_lock(int mode, int n, const char *file, int line) {
  (void)file;
  (void)line;
  if (mode & 1)
    pthread_mutex_lock(&sslLocks[n]);
  else
    pthread_mutex_unlock(&sslLocks[n]);
}
static unsigned long ssl_id(void) {
  return (unsigned long)pthread_self();
}
static void (*sslSetNum)(void *, unsigned long);
static void ssl_threadid(void *id) {
  sslSetNum(id, (unsigned long)pthread_self());
} /* (each thread its own id: with one id for all, OpenSSL took them for one thread and they blocked each other) */
static void ssl_locks(void) {
  static const char *names[] = { "libcrypto.so.1.0.0", "libcrypto.so.1.0.2", "libcrypto.so.10", "libcrypto.so", NULL };
  void *h = NULL;
  for (int i = 0; names[i] && !h; i++)
    h = dlopen(names[i], RTLD_NOW | RTLD_NOLOAD); /* the one curl brought in */
  if (!h) {
    fprintf(stderr, "[net] tls: no OpenSSL 1.0 loaded (nothing to lock)\n");
    return;
  }
  int (*num)(void) = NULL;
  void (*setLock)(void (*)(int, int, const char *, int)) = NULL;
  void *(*getLock)(void) = NULL;
  void (*setId)(unsigned long (*)(void)) = NULL;
  int (*setTid)(void (*)(void *)) = NULL;
  *(void **)(&num) = dlsym(h, "CRYPTO_num_locks");
  *(void **)(&setLock) = dlsym(h, "CRYPTO_set_locking_callback");
  *(void **)(&getLock) = dlsym(h, "CRYPTO_get_locking_callback");
  *(void **)(&setId) = dlsym(h, "CRYPTO_set_id_callback");
  *(void **)(&setTid) = dlsym(h, "CRYPTO_THREADID_set_callback");
  if (!num || !setLock) {
    fprintf(stderr, "[net] tls: OpenSSL locks by itself\n");
    return;
  }
  if (getLock && getLock()) {
    fprintf(stderr, "[net] tls: OpenSSL already has locks\n");
    return;
  }
  int n = num();
  if (n <= 0)
    return;
  sslLocks = calloc((size_t)n, sizeof *sslLocks);
  if (!sslLocks)
    return;
  for (int i = 0; i < n; i++)
    pthread_mutex_init(&sslLocks[i], NULL);
  *(void **)(&sslSetNum) =
      dlsym(h, "CRYPTO_THREADID_set_numeric"); /* from the library itself: it is not in the global names */
  if (setTid && sslSetNum)
    setTid(ssl_threadid);
  else if (setId)
    setId(ssl_id);
  else {
    fprintf(stderr, "[net] tls: no thread id call: left as it was\n");
    return;
  }
  setLock(ssl_lock);
  fprintf(stderr, "[net] tls: OpenSSL 1.0 given %d locks for the request threads\n", n);
}
#endif
int net_init(const char *ca) {
  (void)ua(); /* made once here, before any worker thread reads it */
  if (!resolve())
    return 0;
#if !SB_STATIC_CURL
  ssl_locks(); /* before the first request and the worker threads */
#endif
  if (f_global_init(CURL_GLOBAL_ALL) != CURLE_OK)
    return 0;
  if (ca)
    snprintf(caPath, sizeof caPath, "%s", ca);
  keep_limits();
  share_make();
  const curl_version_info_data *version = f_version_info ? f_version_info(CURLVERSION_NOW) : NULL;
  int asyncDns = version && (version->features & CURL_VERSION_ASYNCHDNS);
  int multi = asyncDns && start_slow_worker();
  if (multi)
    fprintf(stderr, "[net] slow lane: curl multi, up to %d transfers and %d kept connections\n", slowLimit, keepConns);
  else
    fprintf(stderr, "[net] slow lane: blocking workers (%s)\n", asyncDns ? "curl multi unavailable" : "no async DNS");
  for (int i = 0; i < WORKERS; i++)
    if (!multi || i < SLOW0 || i >= BG0)
      start_worker(i);
  {
    pthread_t t;
    if (pthread_create(&t, NULL, file_worker, NULL) == 0)
      pthread_detach(t);
  }
  return 1;
}
static void push_req(Req *r) { /* under mu */
  if (r->bg) {
    if (bgTail)
      bgTail->next = r;
    else
      bgQ = r;
    bgTail = r;
    pthread_cond_signal(&bgCv);
  } else if (r->timeoutMs > 20000) {
    if (!slowQ)
      slow_notify();
    if (slowTail)
      slowTail->next = r;
    else
      slowQ = r;
    slowTail = r;
    pthread_cond_signal(&slowCv);
  } else {
    if (queueTail)
      queueTail->next = r;
    else
      queue = r;
    queueTail = r;
    pthread_cond_signal(&cv);
  }
}
static int bgNext; /* the next request goes to the previews' lane */
static unsigned enqueue_x(const char *url, const char *hdr, const char *post, const char *ctype, long timeoutMs,
                          int size, NetCb cb, void *ud) {
  Req *r = calloc(1, sizeof *r);
  r->bg = bgNext;
  bgNext = 0;
  r->url = strdup(url);
  r->hdr = hdr ? strdup(hdr) : NULL;
  r->post = post ? strdup(post) : NULL;
  r->postLen = post ? strlen(post) : 0;
  r->ctype = post ? strdup(ctype ? ctype : "application/json") : NULL;
  r->cb = cb;
  r->ud = ud;
  r->timeoutMs = timeoutMs;
  r->size = size;
  pthread_mutex_lock(&mu);
  unsigned id = r->id = nextId++;
  push_req(r);
  npending++;
  pthread_mutex_unlock(&mu);
  return id;
}
static unsigned enqueue_c(const char *url, const char *hdr, const char *post, const char *ctype, long timeoutMs,
                          NetCb cb, void *ud) {
  return enqueue_x(url, hdr, post, ctype, timeoutMs, 0, cb, ud);
}
static unsigned enqueue_t(const char *url, const char *hdr, const char *post, long timeoutMs, NetCb cb, void *ud) {
  return enqueue_x(url, hdr, post, NULL, timeoutMs, 0, cb, ud);
}
static unsigned enqueue(const char *url, const char *hdr, const char *post, NetCb cb, void *ud) {
  return enqueue_t(url, hdr, post, 0, cb, ud);
}
unsigned net_get_timeout(const char *url, int timeoutMs, NetCb cb, void *ud) {
  return enqueue_t(url, NULL, NULL, timeoutMs, cb, ud);
}
unsigned net_get(const char *url, NetCb cb, void *ud) {
  return enqueue(url, NULL, NULL, cb, ud);
}
unsigned net_get_hdr(const char *url, const char *hdr, NetCb cb, void *ud) {
  return enqueue(url, hdr, NULL, cb, ud);
}
unsigned net_size(const char *url, NetCb cb, void *ud) {
  return enqueue_x(url, NULL, NULL, NULL, 10000, 1, cb, ud);
}
unsigned net_resolve(const char *url, NetCb cb, void *ud) {
  bgNext = 1;
  return enqueue_x(url, NULL, NULL, NULL, 10000, 2, cb, ud);
}
unsigned net_get_bg(const char *url, int timeoutMs, NetCb cb, void *ud) {
  bgNext = 1;
  return enqueue_x(url, NULL, NULL, NULL, timeoutMs, 0, cb, ud);
}
unsigned net_post_bg(const char *url, const char *body, const char *contentType, const char *headers, int timeoutMs,
                     NetCb cb, void *ud) {
  bgNext = 1;
  return enqueue_c(url, headers, body ? body : "", contentType ? contentType : "", timeoutMs, cb, ud);
}
unsigned net_post_json(const char *url, const char *json, NetCb cb, void *ud) {
  return enqueue(url, NULL, json, cb, ud);
}
unsigned net_post_hdr_t(const char *url, const char *body, const char *contentType, const char *headers, int timeoutMs,
                        NetCb cb, void *ud) {
  return enqueue_c(url, headers, body ? body : "", contentType ? contentType : "", timeoutMs, cb, ud);
}
unsigned net_send(const char *method, const char *url, const char *headers, const void *body, size_t len, int timeoutMs,
                  NetCb cb, void *ud) {
  Req *r = calloc(1, sizeof *r);
  r->url = strdup(url);
  r->hdr = headers ? strdup(headers) : NULL;
  r->cb = cb;
  r->ud = ud;
  r->timeoutMs = timeoutMs;
  if (len || strcmp(method, "GET")) {
    r->method = strcmp(method, "POST") ? strdup(method) : NULL;
    r->post = malloc(len + 1);
    if (len)
      memcpy(r->post, body, len);
    r->post[len] = 0;
    r->postLen = len;
  }
  pthread_mutex_lock(&mu);
  unsigned id = r->id = nextId++;
  push_req(r);
  npending++;
  pthread_mutex_unlock(&mu);
  return id;
}
/* Unlink under mu so a cancelled request can finish even when all workers are busy. */
static int cancel_queued(unsigned id, Req **head, Req **tail) {
  Req *prev = NULL;
  for (Req *r = *head; r; prev = r, r = r->next) {
    if (r->id != id)
      continue;
    if (prev)
      prev->next = r->next;
    else
      *head = r->next;
    if (*tail == r)
      *tail = prev;
    r->cancelled = 1;
    finish_req(r);
    return 1;
  }
  return 0;
}
/* A running request finishes so its connection can be reused. */
void net_cancel_queued(unsigned id) {
  pthread_mutex_lock(&mu);
  if (!cancel_queued(id, &queue, &queueTail) && !cancel_queued(id, &slowQ, &slowTail))
    cancel_queued(id, &bgQ, &bgTail);
  pthread_mutex_unlock(&mu);
}
void net_cancel(unsigned id) {
  pthread_mutex_lock(&mu);
  if (cancel_queued(id, &queue, &queueTail) || cancel_queued(id, &slowQ, &slowTail) ||
      cancel_queued(id, &bgQ, &bgTail) || cancel_queued(id, &fqueue, &fqueueTail)) {
    pthread_mutex_unlock(&mu);
    return;
  }
  if (fileInflight && fileInflight->id == id)
    fileInflight->cancelled = 1;
  for (int i = 0; i < WORKERS; i++)
    if (inflight[i] && inflight[i]->id == id)
      inflight[i]->cancelled = 1;
  for (int i = 0; i < slowLimit; i++) {
    Req *r = slowActive[i].req;
    if (r && r->id == id) {
      r->cancelled = 1;
      slow_notify();
      break;
    }
  }
  for (Req *r = finished; r; r = r->next)
    if (r->id == id)
      r->cancelled = 1;
  pthread_mutex_unlock(&mu);
}
int net_pending(void) {
  pthread_mutex_lock(&mu);
  int n = npending;
  pthread_mutex_unlock(&mu);
  return n;
}
/* parsed answers are freed on a thread of their own: deleting a catalog's tree (thousands of small blocks, made by a
   worker) took 10-25 ms of a frame on the TV */
typedef struct JF {
  cJSON *j;
  struct JF *next;
} JF;
static JF *jfHead;
static pthread_mutex_t jfMu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t jfCv = PTHREAD_COND_INITIALIZER;
static int jfThread;
static void *jf_run(void *a) {
  plat_thread_background(15);
  (void)a;
  for (;;) {
    pthread_mutex_lock(&jfMu);
    while (!jfHead)
      pthread_cond_wait(&jfCv, &jfMu);
    JF *f = jfHead;
    jfHead = NULL;
    pthread_mutex_unlock(&jfMu);
    while (f) {
      JF *n = f->next;
      cJSON_Delete(f->j);
      free(f);
      f = n;
    }
  }
  return NULL;
}
void net_json_free(cJSON *j) {
  if (!j)
    return;
  pthread_mutex_lock(&jfMu);
  if (!jfThread) {
    pthread_t t;
    if (pthread_create(&t, NULL, jf_run, NULL) == 0) {
      pthread_detach(t);
      jfThread = 1;
    }
  }
  if (!jfThread) {
    pthread_mutex_unlock(&jfMu);
    cJSON_Delete(j);
    return;
  }
  JF *f = malloc(sizeof *f);
  f->j = j;
  f->next = jfHead;
  jfHead = f;
  pthread_cond_signal(&jfCv);
  pthread_mutex_unlock(&jfMu);
}
static const unsigned char *curBody;
static cJSON *curJson;
cJSON *net_json(const unsigned char *body) {
  if (body && body == curBody && curJson) {
    cJSON *j = curJson;
    curJson = NULL;
    return j;
  } /* the tree the worker made: the caller owns it */
  return body ? cJSON_Parse((const char *)body) : NULL;
}
void net_pump(void) {
  pthread_mutex_lock(&mu);
  /* Keep the rest of this batch visible to cancellation, including from a callback. */
  Req *last = finishedTail;
  while (last) {
    Req *r = pop_req(&finished, &finishedTail);
    if (r == last)
      last = NULL;
    int cancelled = r->cancelled;
    pthread_mutex_unlock(&mu);
    curBody = r->body;
    curJson = r->json;
    r->json = NULL;
    {
      static int slowLog = -1;
      if (slowLog < 0)
        slowLog = getenv("SB_SLOWLOG") != NULL;
      Uint32 tc0 = slowLog ? SDL_GetTicks() : 0;
      if (r->cb) {
        if (cancelled)
          r->cb(-1, NULL, 0, r->ud);
        else
          r->cb(r->status, r->body ? r->body : (const unsigned char *)"", r->len, r->ud);
      }
      if (slowLog && SDL_GetTicks() - tc0 > 6)
        fprintf(stderr, "[netcb] %u ms for %.120s (%zu bytes)\n", SDL_GetTicks() - tc0, r->url, r->len);
    } /* development: which answers cost a frame */
    if (curJson)
      net_json_free(curJson);
    curJson = NULL;
    curBody = NULL; /* the callback did not want the tree */
    free(r->url);
    free(r->hdr);
    free(r->post);
    free(r->ctype);
    free(r->method);
    free(r->body);
    free(r);
    pthread_mutex_lock(&mu);
    npending--;
  }
  pthread_mutex_unlock(&mu);
}
unsigned char *net_fetch_sync(const char *url, size_t *len, int *status, int timeoutMs) {
  return net_fetch_sync_hdr(url, NULL, len, status, timeoutMs);
}
/* a thread that fetches many small things from the same few hosts (the picture workers) keeps one handle: a new TLS
   connection per poster took the TV's connections down when a long Home was scrolled (curl 7, couldn't connect).
   Bounded like the other kept handles. */
#if SB_TPK
static pthread_key_t keepKey, handleKey;
static pthread_once_t netKeys = PTHREAD_ONCE_INIT;
static int netKeysReady;
static void net_keys_destroy_handle(void *handle) {
  f_easy_cleanup(handle);
}
static void net_keys_init(void) {
  if (pthread_key_create(&keepKey, NULL) == 0) {
    if (pthread_key_create(&handleKey, net_keys_destroy_handle) == 0)
      netKeysReady = 1;
    else
      pthread_key_delete(keepKey);
  }
}
void net_keep_this_thread(void) {
  pthread_once(&netKeys, net_keys_init);
  if (netKeysReady)
    pthread_setspecific(keepKey, (void *)(intptr_t)1);
}
static CURL *kept_handle(int *kept) {
  pthread_once(&netKeys, net_keys_init);
  *kept = netKeysReady && pthread_getspecific(keepKey) && f_easy_reset;
  if (!*kept)
    return f_easy_init();
  CURL *handle = pthread_getspecific(handleKey);
  if (handle) {
    f_easy_reset(handle);
  } else {
    handle = f_easy_init();
    if (handle && pthread_setspecific(handleKey, handle) != 0)
      *kept = 0;
  }
  return handle;
}
#else
static __thread int thKeep;
static __thread CURL *thH;
void net_keep_this_thread(void) {
  thKeep = 1;
}
static CURL *kept_handle(int *kept) {
  *kept = thKeep && f_easy_reset;
  if (!*kept)
    return f_easy_init();
  if (thH)
    f_easy_reset(thH);
  else
    thH = f_easy_init();
  return thH;
}
#endif
unsigned char *net_fetch_sync_hdr(const char *url, const char *hdr, size_t *len, int *status, int timeoutMs) {
  Req r;
  memset(&r, 0, sizeof r);
  r.url = (char *)url;
  int kept;
  CURL *c = kept_handle(&kept);
  if (!c) {
    *status = 0;
    *len = 0;
    return NULL;
  }
  if (kept) {
    f_easy_setopt(c, CURLOPT_MAXCONNECTS, (long)(keepConns ? keepConns : 2));
    f_easy_setopt(c, CURLOPT_DNS_CACHE_TIMEOUT, 300L);
  }
  struct curl_slist *hs = hdr ? f_slist_append(NULL, hdr) : NULL;
  if (hs)
    f_easy_setopt(c, CURLOPT_HTTPHEADER, hs);
  f_easy_setopt(c, CURLOPT_URL, url);
  f_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
  f_easy_setopt(c, CURLOPT_WRITEDATA, &r);
  f_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  f_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
  f_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  f_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
  f_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)timeoutMs);
  f_easy_setopt(c, CURLOPT_USERAGENT, ua());
  f_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
  if (caPath[0])
    f_easy_setopt(c, CURLOPT_CAINFO, caPath);
  char eb[CURL_ERROR_SIZE];
  eb[0] = 0;
  f_easy_setopt(c, CURLOPT_ERRORBUFFER, eb); /* curl's own words for a failure, for the log */
  CURLcode rc = f_easy_perform(c);
  long code = 0;
  f_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  f_easy_setopt(c, CURLOPT_ERRORBUFFER, (char *)NULL);
  if (kept)
    net_stats(c);
  else
    f_easy_cleanup(c);
  if (hs)
    f_slist_free_all(hs);
  *status = rc == CURLE_OK ? (int)code : 0;
  *len = r.len;
  if (*status != 200 && *status != 206) {
    {
      static unsigned nf;
      if (nf++ < 200 || nf % 50 == 0)
        fprintf(stderr, "[net] fetch failed: curl %d, http %ld, %zu bytes%s%s\n", (int)rc, code, r.len,
                eb[0] ? ": " : "", eb);
    }
    free(r.body);
    return NULL;
  }
  return r.body;
}

/* ---- a download read as it arrives (worker threads only: it blocks) ---------------------------- */
typedef struct {
  NetSink sink;
  void *ud;
  int stopped;
} StreamCtx;
static size_t stream_data(char *p, size_t s, size_t n, void *ud) {
  StreamCtx *x = ud;
  if (!x->sink((const unsigned char *)p, s * n, x->ud)) {
    x->stopped = 1;
    return 0;
  }
  return s * n;
}
static int stream_poll(void *ud, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  StreamCtx *x = ud;
  if (!x->sink(NULL, 0, x->ud)) {
    x->stopped = 1;
    return 1;
  }
  return 0;
} /* a stalled transfer can still be stopped */
int net_stream_sync(const char *url, int timeoutMs, NetSink sink, void *ud) {
  if (!f_easy_init && !resolve())
    return 0;
  CURL *c = f_easy_init();
  if (!c)
    return 0;
  StreamCtx x = { sink, ud, 0 };
  f_easy_setopt(c, CURLOPT_URL, url);
  f_easy_setopt(c, CURLOPT_WRITEFUNCTION, stream_data);
  f_easy_setopt(c, CURLOPT_WRITEDATA, &x);
  f_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, stream_poll);
  f_easy_setopt(c, CURLOPT_XFERINFODATA, &x);
  f_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  f_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  f_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
  f_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  f_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
  f_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)timeoutMs);
  f_easy_setopt(c, CURLOPT_USERAGENT,
                ua()); /* no Accept-Encoding: the bytes counted are the bytes that crossed the line */
  if (caPath[0])
    f_easy_setopt(c, CURLOPT_CAINFO, caPath);
  CURLcode rc = f_easy_perform(c);
  long code = 0;
  f_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  f_easy_cleanup(c);
  if (x.stopped)
    return -1;
  return rc == CURLE_OK ? (int)code : 0;
}

/* ---- a GET passed through as it arrives, with the caller's headers (the stream proxy: worker threads only) ----------
 */
typedef struct {
  NetHdrSink hs;
  NetSink sink;
  void *ud;
  int stopped;
} PipeCtx;
static size_t pipe_hdr(char *p, size_t s, size_t n, void *ud) {
  PipeCtx *x = ud;
  size_t l = s * n;
  char line[1024];
  size_t k = l < sizeof line - 1 ? l : sizeof line - 1;
  memcpy(line, p, k);
  line[k] = 0;
  x->hs(line, x->ud);
  return l;
}
static size_t pipe_data(char *p, size_t s, size_t n, void *ud) {
  PipeCtx *x = ud;
  if (!x->sink((const unsigned char *)p, s * n, x->ud)) {
    x->stopped = 1;
    return 0;
  }
  return s * n;
}
static int pipe_poll(void *ud, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  PipeCtx *x = ud;
  if (!x->sink(NULL, 0, x->ud)) {
    x->stopped = 1;
    return 1;
  }
  return 0;
}
static int pipe_sync(const char *url, const char *headers, int head, NetHdrSink hs, NetSink sink, void *ud,
                     int stallSec, int media) {
  if (!f_easy_init && !resolve())
    return 0;
  CURL *c = f_easy_init();
  if (!c)
    return 0;
  PipeCtx x = { hs, sink, ud, 0 };
  struct curl_slist *sl = NULL;
  char *hd = headers ? strdup(headers) : NULL;
  for (char *h = hd, *nl; h && *h; h = nl ? nl + 1 : NULL) {
    nl = strchr(h, '\n');
    if (nl)
      *nl = 0;
    if (*h)
      sl = f_slist_append(sl, h);
  }
  f_easy_setopt(c, CURLOPT_URL, url);
  f_easy_setopt(c, CURLOPT_HEADERFUNCTION, pipe_hdr);
  f_easy_setopt(c, CURLOPT_HEADERDATA, &x);
  f_easy_setopt(c, CURLOPT_WRITEFUNCTION, pipe_data);
  f_easy_setopt(c, CURLOPT_WRITEDATA, &x);
  f_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, pipe_poll);
  f_easy_setopt(c, CURLOPT_XFERINFODATA, &x);
  f_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  f_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  f_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
  if (media) {
#if LIBCURL_VERSION_NUM >= 0x075500
    f_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    f_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    f_easy_setopt(c, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    f_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
#if LIBCURL_VERSION_NUM >= 0x073600
    f_easy_setopt(c, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
#endif
  }
  f_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  f_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  f_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
  f_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, (long)(stallSec > 0 ? stallSec : 60));
  f_easy_setopt(c, CURLOPT_USERAGENT,
                "Mozilla/5.0 (Web0S; Linux) StremioOrigin"); /* a User-Agent among the headers replaces it */
  if (head)
    f_easy_setopt(c, CURLOPT_NOBODY, 1L);
  if (sl)
    f_easy_setopt(c, CURLOPT_HTTPHEADER, sl);
  if (caPath[0])
    f_easy_setopt(c, CURLOPT_CAINFO, caPath);
  CURLcode rc = f_easy_perform(c);
  long code = 0;
  f_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  f_easy_cleanup(c);
  if (sl)
    f_slist_free_all(sl);
  free(hd);
  if (x.stopped)
    return -1;
  return rc == CURLE_OK ? (int)code : 0;
}
int net_pipe_sync(const char *url, const char *headers, int head, NetHdrSink hs, NetSink sink, void *ud, int stallSec) {
  return pipe_sync(url, headers, head, hs, sink, ud, stallSec, 0);
}
int net_media_sync(const char *url, const char *headers, NetHdrSink hs, NetSink sink, void *ud) {
  return pipe_sync(url, headers, 0, hs, sink, ud, 30, 1);
}

/* ---- ranged reads ---------------------------------------------------------------------------- */
struct NetRange {
  CURL *c;
  int follow; /* 1: redirects followed (an addon's link); 0: a resolved address, a redirect comes back as NR_MOVED */
  char eff[4096]; /* where the last good read came from (its redirects followed) */
  char loc[4096]; /* a redirect not followed: where it pointed */
  char why[256]; /* the last failure in curl's words, for the log (hosts only: debrid links carry tokens) */
  long conns, reads; /* connections opened over this handle's reads */
};
static void host_of(const char *u, char *o, size_t n) { /* "https://host" of an address, without its path or token */
  const char *p = u ? strstr(u, "://") : NULL;
  p = p ? p + 3 : (u ? u : "");
  size_t k = strcspn(p, "/?#");
  snprintf(o, n, "%.*s", (int)(k < 200 ? k : 200), p);
}
typedef struct {
  unsigned char *buf;
  long len, got;
  int status, isRange;
  long long reqStart, crStart, total;
  int norange;
} RangeCtx;
static size_t range_hdr(char *p, size_t s, size_t n, void *ud) {
  RangeCtx *r = ud;
  size_t l = s * n;
  char line[512];
  size_t k = l < sizeof line - 1 ? l : sizeof line - 1;
  memcpy(line, p, k);
  line[k] = 0;
  int code;
  if (sscanf(line, "HTTP/%*s %d", &code) == 1) {
    r->status = code;
    r->crStart = -1;
    r->total = -1;
  } /* each redirect hop starts again (its size too: a hop's length is not the file's) */
  else if (!strncasecmp(line, "content-range:", 14)) {
    long long a, b, t;
    char star[4];
    if (sscanf(line + 14, " bytes %lld-%lld/%lld", &a, &b, &t) == 3) {
      r->crStart = a;
      r->total = t;
    } else if (sscanf(line + 14, " bytes %lld-%lld/%1s", &a, &b, star) == 3)
      r->crStart = a;
  } else if (!strncasecmp(line, "content-length:", 15) && r->status == 200) {
    long long cl = atoll(line + 15);
    if (cl > 0)
      r->total = cl;
  }
  return l;
}
static size_t range_data(char *p, size_t s, size_t n, void *ud) {
  RangeCtx *r = ud;
  size_t add = s * n;
  if (r->status >= 300 && r->status < 400)
    return add; /* a redirect's body */
  if (r->status == 200 && r->isRange) {
    r->norange = 1;
    return 0;
  } /* the server ignored the Range */
  if (r->status == 206 && r->crStart >= 0 && r->crStart != r->reqStart) {
    r->norange = 1;
    return 0;
  } /* another part than asked */
  long need = r->len - r->got;
  if (need <= 0)
    return 0;
  long take = (long)add < need ? (long)add : need;
  memcpy(r->buf + r->got, p, take);
  r->got += take;
  if (take < (long)add)
    return 0; /* more than was asked: stop */
  if (r->got >= r->len && r->status != 206)
    return 0; /* a 200 for the whole file must not keep downloading */
  return add; /* a 206 that ends here completes: stopping it closed the connection, and every read paid a new one
                 (connect, TLS, slow start) */
}
NetRange *net_range_new(void) {
  if (!f_easy_init && !resolve())
    return NULL;
  NetRange *r = calloc(1, sizeof *r);
  r->c = f_easy_init();
  if (!r->c) {
    free(r);
    return NULL;
  }
  r->follow = 1;
  return r;
}
void net_range_follow(NetRange *r, int on) {
  if (r)
    r->follow = on;
}
const char *net_range_url(NetRange *r) {
  return r ? r->eff : "";
}
const char *net_range_location(NetRange *r) {
  return r ? r->loc : "";
}
const char *net_range_why(NetRange *r) {
  return r ? r->why : "";
}
long net_range_conns(NetRange *r) {
  return r ? r->conns : 0;
}
/* once: what the reading runs on (the TV's own libcurl on webOS), for the log */
void net_range_info(void) {
  static int done;
  if (done || (!f_easy_init && !resolve()))
    return;
  done = 1;
  const char *ver = "?", *ssl = "?";
  int h2 = 0;
  curl_version_info_data *v = f_version_info ? f_version_info(CURLVERSION_NOW) : NULL;
  if (v) {
    ver = v->version ? v->version : "?";
    ssl = v->ssl_version ? v->ssl_version : "none";
    h2 = (v->features & CURL_VERSION_HTTP2) != 0;
  }
  char rmem[64] = "?";
  FILE *f = fopen("/proc/sys/net/ipv4/tcp_rmem", "r");
  if (f) {
    if (!fgets(rmem, sizeof rmem, f))
      snprintf(rmem, sizeof rmem, "?");
    fclose(f);
    rmem[strcspn(rmem, "\r\n")] = 0;
  }
  fprintf(stderr, "[net] ranged reads: libcurl %s, %s, HTTP/2 %s, tcp_rmem %s\n", ver, ssl, h2 ? "yes" : "no", rmem);
}
void net_range_free(NetRange *r) {
  if (!r)
    return;
  if (r->c)
    f_easy_cleanup(r->c);
  free(r);
}
int net_range_get(NetRange *h, const char *url, long long start, long len, unsigned char **out, long *got,
                  long long *total, int *httpStatus, int timeoutMs) {
  *out = NULL;
  *got = 0;
  if (total)
    *total = -1;
  if (httpStatus)
    *httpStatus = 0;
  if (!h || len <= 0)
    return NR_NET;
  RangeCtx r;
  memset(&r, 0, sizeof r);
  r.buf = malloc(len);
  r.len = len;
  r.total = -1;
  r.crStart = -1;
  r.reqStart = start;
  r.isRange = start > 0; /* asked from past the start: a 200 means no Range support */
  char rg[64];
  snprintf(rg, sizeof rg, "%lld-%lld", start, start + len - 1);
  CURL *c = h->c;
  f_easy_setopt(c, CURLOPT_URL, url);
  f_easy_setopt(c, CURLOPT_RANGE, rg);
  f_easy_setopt(c, CURLOPT_HEADERFUNCTION, range_hdr);
  f_easy_setopt(c, CURLOPT_HEADERDATA, &r);
  f_easy_setopt(c, CURLOPT_WRITEFUNCTION, range_data);
  f_easy_setopt(c, CURLOPT_WRITEDATA, &r);
  f_easy_setopt(c, CURLOPT_FOLLOWLOCATION, h->follow ? 1L : 0L);
  f_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
  f_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  f_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 6000L);
  f_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)timeoutMs);
  f_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 32768L); /* a stalled body gives up after 10 s under 32 KB/s */
  f_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 10L);
  f_easy_setopt(c, CURLOPT_BUFFERSIZE, 262144L); /* fewer callbacks on the TV's CPU at 100+ Mb/s */
  f_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0 (Web0S; Linux) StremioOrigin");
  f_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
  if (share)
    f_easy_setopt(c, CURLOPT_SHARE, share); /* a reconnect then skips the DNS lookup and resumes TLS */
  if (caPath[0])
    f_easy_setopt(c, CURLOPT_CAINFO, caPath);
  char eb[CURL_ERROR_SIZE];
  eb[0] = 0;
  f_easy_setopt(c, CURLOPT_ERRORBUFFER, eb);
  CURLcode rc = f_easy_perform(c);
  f_easy_setopt(c, CURLOPT_ERRORBUFFER, (char *)NULL);
  long nc = 0;
  f_easy_getinfo(c, CURLINFO_NUM_CONNECTS, &nc);
  h->conns += nc;
  h->reads++;
  if (httpStatus)
    *httpStatus = r.status;
  if (total)
    *total = r.total;
  h->loc[0] = 0;
  if (r.norange) {
    free(r.buf);
    snprintf(h->why, sizeof h->why, "the server sent another part than asked (HTTP %d)", r.status);
    return NR_NORANGE;
  }
  if (r.got > 0 && (r.status == 200 || r.status == 206)) {
    char *e = NULL;
    f_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &e);
    snprintf(h->eff, sizeof h->eff, "%s", e ? e : url);
    *out = r.buf;
    *got = r.got;
    return NR_OK;
  } /* full, or a server that cut the response: what came */
  free(r.buf);
  if (!h->follow && r.status >= 300 && r.status < 400) {
    char *l = NULL;
    f_easy_getinfo(c, CURLINFO_REDIRECT_URL, &l);
    snprintf(h->loc, sizeof h->loc, "%s", l ? l : "");
    char hl[220];
    host_of(h->loc, hl, sizeof hl);
    snprintf(h->why, sizeof h->why, "HTTP %d to %s", r.status, hl[0] ? hl : "(no Location)");
    return NR_MOVED;
  } /* a resolved address that moves on: the caller asks the addon again */
  {
    long rcnt = 0;
    char *ip = NULL, *e = NULL, *l = NULL;
    f_easy_getinfo(c, CURLINFO_REDIRECT_COUNT, &rcnt);
    f_easy_getinfo(c, CURLINFO_PRIMARY_IP, &ip);
    f_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &e);
    f_easy_getinfo(c, CURLINFO_REDIRECT_URL, &l);
    char he[220], hl[220];
    host_of(e, he, sizeof he);
    host_of(l, hl, sizeof hl);
    snprintf(h->why, sizeof h->why, "curl %d (%s), HTTP %d, %ld redirects, at %s%s%s, ip %s", (int)rc,
             eb[0] ? eb : "no message", r.status, rcnt, he, hl[0] ? ", next " : "", hl, ip && ip[0] ? ip : "?");
  }
  if (rc == CURLE_OPERATION_TIMEDOUT)
    return NR_TIMEOUT;
  if (r.status == 429 || r.status >= 500)
    return NR_BUSY;
  if (r.status >= 400)
    return NR_HTTP;
  return NR_NET;
}
