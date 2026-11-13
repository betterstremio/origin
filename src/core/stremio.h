/* Everything that talks to Stremio and the addons: the account (sign-in, user, addon list,
   library sync), catalogs and metadata (with disk caches), streams from the addons, subtitles.
   Results come back on the main thread through callbacks. */
#ifndef SB_STREMIO_H
#define SB_STREMIO_H
#include "cJSON.h"
#include "core/addons.h"

#define CINEMETA "https://v3-cinemeta.strem.io"
#define STREMIO_API "https://api.strem.io"

/* a catalog entry / library card */
typedef struct {
  char id[64], type[16], name[256], poster[512], background[512], logo[512], description[1024], metaLine[96];
  char videoId[80];
  float progress, left;
  int cw;
  int ep;
  char epName[256]; /* an episode card on a title page: its menu offers the streams */
} Item;
typedef struct {
  Item *items;
  int n;
  int total; /* total: the metas in the answer (a page may hold more than kept); -1: the source did not answer */
} ItemList;

/* url: the first page as asked (also the cache's name); path: the address before the extra part, extra: what the
   first page is asked with (a required property's first option, or none), skip: the catalog takes "skip" (pages) */
typedef struct {
  char title[96], type[16], url[ADDON_URL_MAX + 256], key[48], path[ADDON_URL_MAX], extra[200];
  int skip;
} CatalogDef;

int sb_init(void);
/* ---- account ---- */
int acct_other_kept(void); /* the other service is signed in on this device too */
void acct_switch(void); /* over to the other service: the data swapped on this device, nothing downloaded first */
void acct_begin_add(void); /* the account in use set aside, to sign in to the other service */
int acct_restore_kept(
    void); /* no account in use and one kept: back to it (1). Also at start: an add that was left half way */
void sb_account_moved(void); /* another account is in use: anything still out for the old one is dropped */
int acct_is_nuvio(void); /* the account in use is a Nuvio one (core/nuvio.c); otherwise everything below is Stremio's */
int sb_signed_in(void);
const char *sb_auth_key(void);
const char *sb_user_email(void);
const char *sb_user_id(void);
typedef void (*SbCb)(int ok, const char *err, void *ud);
void sb_login(const char *email, const char *password, SbCb cb, void *ud);
void sb_finish_login(const char *authKey, SbCb cb, void *ud); /* QR sign-in: fetches the user */
void sb_logout(void);
void sb_forget_local(void); /* this TV signs out (its kept account, addons, library), the session itself left alone */
void sb_sync_addons(void (*cb)(int changed, void *ud), void *ud);
void sb_pull_library(void (*cb)(int changed, void *ud), void *ud);
void sb_sync_soon(int force); /* pull if the last one was a while ago (core/sync.c: boards rebuild when it changed) */
unsigned sb_lib_sync_age(void); /* ms since the last library pull began (huge when none yet) */
cJSON *sb_addons(void); /* the installed addon list (array), including those off on this TV */
int sb_addon_on(const cJSON *addon); /* not switched off on this TV ("adOn_<key>") */
void sb_addon_key(const cJSON *addon, char *out, size_t cap); /* its manifest id, or its URL */

/* ---- catalogs ---- */
int sb_pinned_defs(CatalogDef *out, int max); /* the catalogs that always head Home, when their addon is installed */
int sb_home_defs(CatalogDef *out, int max);
int sb_disc_defs(CatalogDef *out, int max);
/* cached copy first (cb with cached=1), then the fresh one if it differs; the list is malloc'd for the callback owner
 */
void sb_catalog(const CatalogDef *def, void (*cb)(ItemList *list, int cached, void *ud), void *ud);
void sb_catalog_url(const char *url, const char *type, int n, void (*cb)(ItemList *list, int cached, void *ud),
                    void *ud);
/* a search's catalog: never cached on disk; bg: on the previews' lane; returns the request's id */
unsigned sb_search_catalog(const char *url, const char *type, int n, int bg,
                           void (*cb)(ItemList *list, int cached, void *ud), void *ud);
/* the next page of a catalog that pages: everything from item `skip` on (never cached). list NULL: the request failed
   (it can be asked again); list->n 0: the end. more: the answer's hasMore (1 / 0), -1 when it did not say */
void sb_catalog_page(const CatalogDef *def, int skip, void (*cb)(ItemList *list, int more, void *ud), void *ud);
/* every installed catalog that searches (its other properties optional), asked for q; 0 when none (Cinemeta's then) */
int sb_source_def(const char *addonId, const char *base, const char *type, const char *catalog, const char *genre,
                  const char *title, CatalogDef *d);
int sb_name_defs(const char *title, CatalogDef *out, int max);
int sb_search_defs(const char *q, CatalogDef *out, int max);
void itemlist_free(ItemList *l); /* a malloc'd list (catalog answers) */
void itemlist_clear(ItemList *l); /* the items of a list kept elsewhere */

/* ---- metadata ---- */
void sb_meta(const char *type, const char *id, void (*cb)(cJSON *meta, void *ud),
             void *ud); /* meta belongs to the cache (do not free) */
cJSON *sb_meta_cached(const char *type, const char *id);
void sb_fill_item(Item *it, const cJSON *m, const char *type);
const char *sb_poster_of(const Item *it, char *buf, size_t cap);
const char *sb_bg_small(const Item *it, char *buf, size_t cap);
const char *sb_bg_mid(const Item *it, char *buf, size_t cap);
const char *sb_bg_card(const Item *it, char *buf, size_t cap);
const char *sb_bg_large(const Item *it, char *buf,
                        size_t cap); /* the largest there is: for a picture drawn nearly full screen */
const char *sb_logo_of(const Item *it, char *buf, size_t cap);
int sb_is_imdb(const char *id);
void sb_meta_line(const cJSON *m, char *out, size_t cap);
int sb_runtime_min(const char *s);
void sb_fmt_runtime(int mn, char *out, size_t cap);

/* ---- library / Continue Watching / watched ---- */
cJSON *lib_all(void); /* the whole library (an object keyed by id; belongs to the store) */
cJSON *lib_get(const char *id); /* the account's item or NULL (belongs to the store) */
int lib_has(const char *id); /* in the Library (not removed) */
void lib_toggle(const Item *it);
int lib_cw_items(ItemList *out); /* Continue Watching, newest first */
typedef struct {
  char vid[80];
  float pct, leftMin;
} CwInfo;
int lib_cw_info(const Item *it, CwInfo *out);
int lib_item_watched(const Item *it); /* the badge */
int lib_is_marked_watched(const Item *it);
void lib_toggle_watched(const Item *it, const cJSON *videos);
int lib_movie_watched(const cJSON *lit);
int lib_series_watched(const cJSON *lit, const cJSON *videos);
void lib_set_eps_watched(cJSON *lit, const cJSON *videos, const char **epIds, int n, int on);
void lib_set_movie_watched(cJSON *lit, int on);
void lib_ep_watched_map(const cJSON *lit, const cJSON *videos, int *outFlags); /* flags[i] per videos[i] */
cJSON *lib_new_item(const Item *it);
void lib_save(cJSON *lit); /* mtime, store, push */
/* force = push to the account now (pause, close, end); otherwise at most once a minute */
void lib_set_progress(const char *id, const Item *it, const char *vid, double timeOffsetMs, double durationMs,
                      int force);
void lib_invalidate(void);
void lib_commit(void); /* many items changed in place: one version bump, one save */
unsigned lib_version(void); /* bumps on every change: boards rebuild when it moves */

/* ---- streams ---- */
/* url: what plays ("p2p://<infoHash>/<fileIdx>?tr=..." for P2P streams, "ytid:<id>" for YouTube); ext: an externalUrl
   (opened on another device, never played); hdrs / rhdrs: proxyHeaders' request and response headers ("Name: value"
   lines) sent with every request for it; binge: behaviorHints.bingeGroup; size / hash: videoSize / videoHash */
#define SB_URL_MAX 4096 /* some addons' debrid links run past 2 KB (their whole setup sealed in the address) */
typedef struct {
  char addon[96], name[256], title[512], url[SB_URL_MAX], filename[256];
  int p2p;
  char ext[512], hdrs[512], rhdrs[256], binge[96], hash[24];
  double size;
  int notWeb, nsubs;
} Stream;
typedef struct {
  char name[96];
  Stream *items;
  int n;
} StreamGroup;
typedef void (*StreamPartCb)(StreamGroup *g, int groupIdx, void *ud);
typedef void (*StreamDoneCb)(int total, void *ud);
unsigned sb_streams(const char *type, const char *vid, StreamPartCb part, StreamDoneCb done, void *ud);
/* a preview's lookup: on the previews' own network lane, 5 s per addon, addons that keep failing left out for a while,
   and cancellable (the card left: what is still out stops) */
int sb_stream_is_limit(const Stream *s); /* the entry is an addon saying its rate limit is reached */
unsigned sb_streams_bg(const char *type, const char *vid, StreamPartCb part, StreamDoneCb done, void *ud);
void sb_streams_cancel(unsigned tok);
/* the same, also told after each answer how many addons are still out of how many (the "waiting for N addons" line) */
typedef void (*StreamAnsCb)(int pending, int count, void *ud);
unsigned sb_streams_ex(const char *type, const char *vid, StreamPartCb part, StreamDoneCb done, StreamAnsCb ans,
                       void *ud);
/* the addons that serve streams for this video, in the user's order (their names); then one of them alone, by that
 * place */
int sb_stream_addons(const char *type, const char *vid, char (*names)[96], int max);
unsigned sb_streams_one(const char *type, const char *vid, int which, StreamPartCb part, StreamDoneCb done, void *ud);
void sb_stream_info(const Stream *s, char *out, size_t cap);
/* the last played stream of a title/episode */
int sb_resume_stream(const char *id, const char *vid, Stream *out);
void sb_remember_stream(const char *id, const char *vid, const Stream *s);

/* ---- subtitles ---- */
/* from: where it came from (the stream itself, a subtitles addon, the OpenSubtitles fallback); addon: that source's
   name; label: the addon's own label for it ("" when none); order: the addon's place in the list */
enum { SUB_STREAM = 0, SUB_ADDON = 1, SUB_FALLBACK = 2 };
typedef struct {
  char lang[24], name[64], url[1024];
  int n;
  char addon[64], label[96];
  int from, order;
  char rel[96];
  int hm;
  double fps;
} SubEntry; /* rel: the file's own name when told; hm: found by the video's hash; fps: the frame rate it was timed for
               (0 unknown) */
/* the subtitles a stream object carried (stream.subtitles), kept from its addon's answer; *out malloc'd (NULL when
 * none) */
int sb_stream_subs(const Stream *s, SubEntry **out);

/* ---- search ---- */
unsigned sb_suggest(const char *q, void (*cb)(ItemList *list, char names[6][128], int nnames, void *ud), void *ud);
#endif
