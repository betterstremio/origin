/* The Nuvio account (see nuvio.h). Nuvio's server is Supabase: every call is a POST with the app key and a bearer
   token, the data calls are RPCs under /rest/v1/rpc. The address and key come from the private keys file
   (tools/build.sh), never from the repo.

   Sync rule: Nuvio replaces a profile's whole list on each push, so a sync always pulls first, merges what changed
   on this TV into what came down, and pushes the merged whole. What came down last time is kept ("nvLibIds",
   "nvW") so a removal on either side can be told from something the other side has not seen yet. */
#include "core/nuvio.h"
#include <strings.h>
#include "core/stremio.h"
#include "core/nuvio_merge.h"
#include "core/recs.h"
#include "core/tmdb.h"
#include "screens/player.h"
#include "platform/plat.h"
#include "core/net.h"
#include "core/store.h"
#include "core/json.h"
#include "sbkeys.h"
#include "cJSON.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>

/* ---- server settings ------------------------------------------------------------------------- */
static char nvUrl[200], nvFallback[200], nvKey[600], nvLinkBase[200]; static int nvInit;
static void unscr(const unsigned char *s, const unsigned char *pad, int n, char *out, size_t cap) {
  int i; for (i = 0; i < n && i + 1 < (int)cap; i++) out[i] = (char)(s[i] ^ pad[i % 32]); out[i] = 0;
  size_t l = strlen(out); while (l && out[l - 1] == '/') out[--l] = 0;
}
static void init(void) {
  if (nvInit) return; nvInit = 1;
  unscr(SBK_NUVIO_SUPABASE_URL, SBK_NUVIO_SUPABASE_URL_PAD, SBK_NUVIO_SUPABASE_URL_LEN, nvUrl, sizeof nvUrl);
  unscr(SBK_NUVIO_SUPABASE_FALLBACK_URL, SBK_NUVIO_SUPABASE_FALLBACK_URL_PAD, SBK_NUVIO_SUPABASE_FALLBACK_URL_LEN, nvFallback, sizeof nvFallback);
  unscr(SBK_NUVIO_SUPABASE_ANON_KEY, SBK_NUVIO_SUPABASE_ANON_KEY_PAD, SBK_NUVIO_SUPABASE_ANON_KEY_LEN, nvKey, sizeof nvKey);
  unscr(SBK_DEVICE_LOGIN_WEB_BASE_URL, SBK_DEVICE_LOGIN_WEB_BASE_URL_PAD, SBK_DEVICE_LOGIN_WEB_BASE_URL_LEN, nvLinkBase, sizeof nvLinkBase);
  if (!nvLinkBase[0]) snprintf(nvLinkBase, sizeof nvLinkBase, "https://nuvio.tv/link");
  if (getenv("SB_NV_URL")) { snprintf(nvUrl, sizeof nvUrl, "%s", getenv("SB_NV_URL")); snprintf(nvKey, sizeof nvKey, "%s", getenv("SB_NV_KEY") ? getenv("SB_NV_KEY") : "k"); snprintf(nvFallback, sizeof nvFallback, "%s", getenv("SB_NV_URL2") ? getenv("SB_NV_URL2") : ""); } /* development: tests/nuvio's server */
}
int nv_configured(void) { init(); return nvUrl[0] && nvKey[0]; }

/* ---- time ------------------------------------------------------------------------------------ */
static double now_ms(void) { return (double)time(NULL) * 1000.0; }

/* ---- requests -------------------------------------------------------------------------------- */
enum { BEAR_KEY, BEAR_USER, BEAR_QR };
typedef void (*RawCb)(int st, const cJSON *j, void *ud);
typedef void (*BodyCb)(int st, const unsigned char *b, size_t n, void *ud); /* the big answers: copied and parsed on a thread */
typedef struct NvReq { char path[400]; char *body; int bear, retried, fb, get; RawCb cb; BodyCb bcb; void *ud; } NvReq;
static char qrBearer[2048];
static int lost;
static void send_req(NvReq *r);
static void refresh_then(NvReq *r);
static void finish(NvReq *r, int st, const cJSON *j) { if (r->bcb) r->bcb(st, NULL, 0, r->ud); else if (r->cb) r->cb(st, j, r->ud); free(r->body); free(r); }
static void on_resp(int st, const unsigned char *b, size_t n, void *ud) {
  NvReq *r = ud;
  if ((st == 0 || st == 408 || st >= 500) && !r->fb && nvFallback[0]) { r->fb = 1; send_req(r); return; } /* the second address on a network failure or a server error */
  if (st == 401 && r->bear == BEAR_USER && !r->retried) { r->retried = 1; refresh_then(r); return; }
  if (st < 200 || st >= 300) fprintf(stderr, "[nuvio] %s: HTTP %d\n", r->path, st);
  if (r->bcb) { r->bcb(st, b, n, r->ud); free(r->body); free(r); return; }
  cJSON *j = b && n ? cJSON_ParseWithLength((const char *)b, n) : NULL;
  finish(r, st, j); cJSON_Delete(j);
}
static void send_req(NvReq *r) {
  char url[700], hdr[3000]; snprintf(url, sizeof url, "%s%s", r->fb ? nvFallback : nvUrl, r->path);
  const char *bearer = r->bear == BEAR_USER ? store_get("nvAccess", nvKey) : r->bear == BEAR_QR ? qrBearer : nvKey;
  snprintf(hdr, sizeof hdr, "apikey: %s\nAuthorization: Bearer %s\nAccept: application/json", nvKey, bearer);
  if (r->get) net_send("GET", url, hdr, "", 0, 25000, on_resp, r); else net_post_hdr_t(url, r->body, "application/json", hdr, 25000, on_resp, r);
}
static void post_str(const char *path, char *bodyOwned, int bear, RawCb cb, BodyCb bcb, void *ud) {
  init(); NvReq *r = calloc(1, sizeof *r); snprintf(r->path, sizeof r->path, "%s", path); r->body = bodyOwned; r->bear = bear; r->cb = cb; r->bcb = bcb; r->ud = ud;
  if (!nv_configured()) { finish(r, 0, NULL); return; }
  if (bear == BEAR_USER && (!store_get("nvRefresh", NULL) || lost)) { finish(r, 401, NULL); return; }
  if (bear == BEAR_USER && atof(store_get("nvExp", "0")) < (double)time(NULL) + 60) { refresh_then(r); return; } /* about to run out: renewed first */
  send_req(r);
}
static void post(const char *path, cJSON *body, int bear, RawCb cb, void *ud) { /* takes body */
  char *t = body ? cJSON_PrintUnformatted(body) : strdup("{}"); cJSON_Delete(body); post_str(path, t, bear, cb, NULL, ud);
}
static void table_get(const char *table, const char *query, RawCb cb, void *ud) { /* a table read: GET /rest/v1/<table>?<query> */
  init(); NvReq *r = calloc(1, sizeof *r); snprintf(r->path, sizeof r->path, "/rest/v1/%s?%s", table, query); r->body = strdup(""); r->get = 1; r->bear = BEAR_USER; r->cb = cb; r->ud = ud;
  if (!nv_configured() || !store_get("nvRefresh", NULL) || lost) { finish(r, 401, NULL); return; }
  if (atof(store_get("nvExp", "0")) < (double)time(NULL) + 60) { refresh_then(r); return; }
  send_req(r);
}
static void rpc(const char *fn, cJSON *params, RawCb cb, void *ud) { char p[128]; snprintf(p, sizeof p, "/rest/v1/rpc/%s", fn); post(p, params ? params : cJSON_CreateObject(), BEAR_USER, cb, ud); }
static void rpc_body(const char *fn, char *body, BodyCb cb, void *ud) { char p[128]; snprintf(p, sizeof p, "/rest/v1/rpc/%s", fn); post_str(p, body, BEAR_USER, NULL, cb, ud); }

/* the session: kept fresh with the refresh token; requests that arrive meanwhile wait for it */
static NvReq *waiting[64]; static int nWaiting, refreshing;
static void keep_tokens(const cJSON *j) {
  const cJSON *s = js_obj(j, "session"); const cJSON *src = js_str(j, "access_token", NULL) ? j : s;
  const char *at = js_str(src, "access_token", NULL), *rt = js_str(src, "refresh_token", NULL); if (!at) return;
  store_set("nvAccess", at); if (rt && *rt) store_set("nvRefresh", rt);
  double exp = js_num(src, "expires_at", 0); if (!exp) exp = (double)time(NULL) + js_num(src, "expires_in", 3600);
  char e[32]; snprintf(e, sizeof e, "%.0f", exp); store_set("nvExp", e);
  const cJSON *u = js_obj(src, "user"); if (u) { cJSON *keep = cJSON_CreateObject(); cJSON_AddStringToObject(keep, "id", js_str(u, "id", "")); cJSON_AddStringToObject(keep, "email", js_str(u, "email", "")); store_set_node("nvUser", keep); }
  store_flush();
}
static void on_refresh(int st, const cJSON *j, void *ud) {
  (void)ud; refreshing = 0; int ok = st == 200 && js_str(j, "access_token", NULL);
  if (ok) keep_tokens(j);
  else if (st == 400 || st == 401 || st == 403) { lost = 1; fprintf(stderr, "[nuvio] the session was refused (HTTP %d): signed out\n", st); }
  int n = nWaiting; NvReq *w[64]; memcpy(w, waiting, sizeof(NvReq *) * n); nWaiting = 0;
  for (int i = 0; i < n; i++) { if (ok) { w[i]->retried = 1; send_req(w[i]); } else finish(w[i], st ? 401 : 0, NULL); }
}
static void refresh_then(NvReq *r) {
  if (nWaiting < 64) waiting[nWaiting++] = r; else { finish(r, 0, NULL); return; }
  if (refreshing) return; refreshing = 1;
  cJSON *b = cJSON_CreateObject(); cJSON_AddStringToObject(b, "refresh_token", store_get("nvRefresh", ""));
  NvReq *q = calloc(1, sizeof *q); snprintf(q->path, sizeof q->path, "/auth/v1/token?grant_type=refresh_token"); q->body = cJSON_PrintUnformatted(b); cJSON_Delete(b);
  q->bear = BEAR_KEY; q->cb = on_refresh; send_req(q);
}

/* ---- account --------------------------------------------------------------------------------- */
int nv_signed_in(void) { return store_get("nvRefresh", NULL) != NULL && !lost; }
int nv_session_lost(void) { return lost; }
const char *nv_email(void) { return js_str(store_node("nvUser"), "email", ""); }
static int syncGen = 1; /* moves with the profile: anything still out for the old one is dropped */
static const char *const SCOPED[] = { "addons", "lib", "streams", "recsDismissed", "nvLastSync" };
#define NSCOPED ((int)(sizeof SCOPED / sizeof *SCOPED))
void nv_scope(int index, char *out, size_t cap) { /* the file names of a profile's own data */
  char o[96]; size_t k = 0; for (const char *p = store_get("nvOwner", ""); *p && k + 1 < sizeof o; p++) if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-') o[k++] = *p; o[k] = 0;
  snprintf(out, cap, "nv-%s-p%d", o, index);
}
void nv_logout(void) {
  if (store_get("nvAccess", NULL) && !lost) post("/auth/v1/logout", NULL, BEAR_USER, NULL, NULL);
  static const char *const k[] = { "nvAccess", "nvRefresh", "nvExp", "nvUser", "nvProfiles", "nvProfile", "nvOwner", "acctKind", "nvAvCat", "nvAvAt" };
  for (unsigned i = 0; i < sizeof k / sizeof *k; i++) store_remove(k[i]);
  for (int i = 0; i < NSCOPED; i++) store_remove(SCOPED[i]);
  syncGen++; store_scope_forget("nv-"); store_flush(); lost = 0; lib_invalidate(); recs_account_changed();
}

/* ---- two services on one device ---------------------------------------------------------------- */
/* one is in use (acctKind); the other's own data waits in a file: Stremio's in acct/stremio.json, a Nuvio profile's in its usual one.
   Nuvio's session keys have names of their own and simply stay. A Stremio user who never adds Nuvio never comes here */
static const char *const ACCT_KEYS[] = { "authKey", "user", "addons", "lib", "streams", "recsDismissed", "nvLastSync" };
#define NACCT ((int)(sizeof ACCT_KEYS / sizeof *ACCT_KEYS))
static int stremio_file(void) { char p[1400]; struct stat st; snprintf(p, sizeof p, "%s/acct/stremio.json", store_data_dir()); return stat(p, &st) == 0; }
static int nuvio_kept(void) { return nv_signed_in() && nv_profile_current() > 0; }
int acct_other_kept(void) { return acct_is_nuvio() ? stremio_file() : nuvio_kept(); }
static void acct_moved(void) { syncGen++; sb_account_moved(); store_flush(); lib_invalidate(); recs_account_changed(); }
void acct_switch(void) {
  if (!acct_other_kept()) return; char s[160]; nv_scope(nv_profile_current(), s, sizeof s);
  if (acct_is_nuvio()) { store_scope_swap(s, "stremio", ACCT_KEYS, NACCT); store_remove("acctKind"); }
  else { store_scope_swap("stremio", s, ACCT_KEYS, NACCT); store_set("acctKind", "nuvio"); }
  acct_moved();
}
void acct_begin_add(void) { char s[160]; nv_scope(nv_profile_current(), s, sizeof s); /* (to a name no file has: nothing comes back) */
  if (acct_is_nuvio()) { store_scope_swap(s, "none", ACCT_KEYS, NACCT); store_remove("acctKind"); } else store_scope_swap("stremio", "none", ACCT_KEYS, NACCT);
  acct_moved();
}
int acct_restore_kept(void) {
  if (acct_is_nuvio() || sb_auth_key()) return 0;
  if (stremio_file()) store_scope_swap(NULL, "stremio", ACCT_KEYS, NACCT);
  else if (nuvio_kept()) { char s[160]; nv_scope(nv_profile_current(), s, sizeof s); store_scope_swap(NULL, s, ACCT_KEYS, NACCT); store_set("acctKind", "nuvio"); }
  else return 0;
  acct_moved(); return 1;
}

/* sign-in with the code: an anonymous session first (the code is asked for under it), then Nuvio's device login */
typedef struct { NvDoneCb cb; void *ud; char email[128]; } Box;
static char qrNonce[48], qrDevice[160], qrUser[32], qrLink[400]; static double qrPoll = 3;
const char *nv_qr_code(void) { return qrUser; }
const char *nv_qr_link(void) { return qrLink; }
double nv_qr_poll_s(void) { return qrPoll; }
static void make_nonce(void) {
  static const char A[] = "abcdefghijklmnopqrstuvwxyz0123456789"; unsigned s = (unsigned)time(NULL) ^ SDL_GetTicks() * 2654435761u;
  for (int i = 0; i < 40; i++) { s = s * 1103515245u + 12345u; qrNonce[i] = A[(s >> 16) % 36]; } qrNonce[40] = 0;
}
static void on_qr_session(int st, const cJSON *j, void *ud) {
  Box *x = ud; const cJSON *row = cJSON_IsArray(j) ? cJSON_GetArrayItem(j, 0) : j;
  const char *dc = js_str(row, "device_code", NULL), *uc = js_str(row, "user_code", NULL), *link = js_str(row, "verification_uri_complete", NULL);
  if (st != 200 || !dc || !uc || !link) { x->cb(0, "Couldn\xe2\x80\x99t get a sign-in code", x->ud); free(x); return; }
  snprintf(qrDevice, sizeof qrDevice, "%s", dc); snprintf(qrUser, sizeof qrUser, "%s", uc); snprintf(qrLink, sizeof qrLink, "%s", link);
  qrPoll = js_num(row, "poll_interval_seconds", 3); if (qrPoll < 2) qrPoll = 2;
  x->cb(1, NULL, x->ud); free(x);
}
static void start_session(Box *x) {
  make_nonce(); cJSON *p = cJSON_CreateObject(); cJSON_AddStringToObject(p, "p_device_nonce", qrNonce); cJSON_AddStringToObject(p, "p_redirect_base_url", nvLinkBase);
  cJSON_AddStringToObject(p, "p_device_type", "tv"); cJSON_AddStringToObject(p, "p_device_name", plat_device_name());
  post("/rest/v1/rpc/start_device_login_session", p, BEAR_QR, on_qr_session, x);
}
static void on_anon(int st, const cJSON *j, void *ud) {
  Box *x = ud; const char *at = js_str(j, "access_token", NULL);
  if (st == 200 && at) { snprintf(qrBearer, sizeof qrBearer, "%s", at); start_session(x); return; }
  if (!x->email[0]) { x->email[0] = 1; post("/auth/v1/token?grant_type=anonymous", NULL, BEAR_KEY, on_anon, x); return; } /* (the second way in, once) */
  x->cb(0, "Couldn\xe2\x80\x99t reach Nuvio", x->ud); free(x);
}
void nv_qr_start(NvDoneCb cb, void *ud) {
  Box *x = calloc(1, sizeof *x); x->cb = cb; x->ud = ud; qrUser[0] = qrLink[0] = 0;
  if (!nv_configured()) { cb(0, "This build has no Nuvio settings", ud); free(x); return; }
  cJSON *b = cJSON_CreateObject(); cJSON *d = cJSON_AddObjectToObject(b, "data"); cJSON_AddStringToObject(d, "tv_client", "webos");
  post("/auth/v1/signup", b, BEAR_KEY, on_anon, x);
}
static void on_exchange(int st, const cJSON *j, void *ud) {
  Box *x = ud; const cJSON *s = js_obj(j, "session"); const cJSON *src = js_str(j, "access_token", NULL) ? j : s;
  if (st == 200 && js_str(src, "access_token", NULL) && js_str(src, "refresh_token", NULL)) { lost = 0; keep_tokens(j); store_set("acctKind", "nuvio"); store_flush(); x->cb(1, NULL, x->ud); }
  else x->cb(0, "Sign-in failed", x->ud);
  free(x);
}
static void on_poll(int st, const cJSON *j, void *ud) {
  Box *x = ud; const cJSON *row = cJSON_IsArray(j) ? cJSON_GetArrayItem(j, 0) : j; const char *status = js_str(row, "status", "");
  if (st == 200 && !strcmp(status, "approved")) {
    cJSON *b = cJSON_CreateObject(); cJSON_AddStringToObject(b, "code", qrDevice); cJSON_AddStringToObject(b, "device_nonce", qrNonce);
    post("/functions/v1/tv-logins-exchange", b, BEAR_QR, on_exchange, x); return;
  }
  double pi = js_num(row, "poll_interval_seconds", 0); if (pi >= 2) qrPoll = pi;
  if (st == 200 && (!strcmp(status, "expired") || !strcmp(status, "used") || !strcmp(status, "cancelled"))) x->cb(0, "expired", x->ud);
  else x->cb(0, st == 200 ? "pending" : "network", x->ud);
  free(x);
}
void nv_qr_poll(NvDoneCb cb, void *ud) {
  Box *x = calloc(1, sizeof *x); x->cb = cb; x->ud = ud;
  if (!qrDevice[0]) { cb(0, "expired", ud); free(x); return; }
  cJSON *p = cJSON_CreateObject(); cJSON_AddStringToObject(p, "p_code", qrDevice); cJSON_AddStringToObject(p, "p_device_nonce", qrNonce);
  post("/rest/v1/rpc/poll_tv_login_session", p, BEAR_QR, on_poll, x);
}
static void on_password(int st, const cJSON *j, void *ud) {
  Box *x = ud;
  if (st == 200 && js_str(j, "access_token", NULL) && js_str(j, "refresh_token", NULL)) { lost = 0; keep_tokens(j); store_set("acctKind", "nuvio"); store_flush(); x->cb(1, NULL, x->ud); }
  else { const char *m = js_str(j, "error_description", js_str(j, "msg", NULL)); x->cb(0, st == 400 ? "Wrong email or password" : m ? m : st ? "Sign-in failed" : "No connection", x->ud); }
  free(x);
}
void nv_login_email(const char *email, const char *password, NvDoneCb cb, void *ud) {
  Box *x = calloc(1, sizeof *x); x->cb = cb; x->ud = ud;
  cJSON *b = cJSON_CreateObject(); cJSON_AddStringToObject(b, "email", email); cJSON_AddStringToObject(b, "password", password);
  post("/auth/v1/token?grant_type=password", b, BEAR_KEY, on_password, x);
}

/* ---- profiles -------------------------------------------------------------------------------- */
static unsigned hex_color(const char *h) { if (!h) return 0x1E88E5; if (*h == '#') h++; return (unsigned)strtoul(h, NULL, 16) & 0xffffff; }
static void on_locks(int st, const cJSON *j, void *ud) {
  Box *x = ud; cJSON *ps = store_node("nvProfiles");
  if (st == 200 && cJSON_IsArray(j) && cJSON_IsArray(ps)) { const cJSON *row; cJSON_ArrayForEach(row, j) { int idx = (int)js_num(row, "profile_index", 0); int on = js_bool(row, "pin_enabled", 0);
      cJSON *p; cJSON_ArrayForEach(p, ps) if ((int)js_num(p, "i", 0) == idx) { cJSON_DeleteItemFromObjectCaseSensitive(p, "pin"); cJSON_AddBoolToObject(p, "pin", on); } }
    store_set_node("nvProfiles", ps); store_flush(); }
  if (x->cb) x->cb(1, NULL, x->ud); free(x);
}
/* Nuvio's avatar catalogue (get_avatar_catalog, public): a profile keeps only the picture's id, the catalogue gives its
   file in the "avatars" bucket. Kept as {id: path}; asked again when a profile's id is missing from it, at most every 15 min
   (Nuvio's own refresh), else once a day. Supporters' pictures (a private bucket) are not shown: colour and initial then */
#define AVATAR_URL_PATH "/storage/v1/object/public/avatars/"
static double avAskedAt;
static void on_avatars(int st, const cJSON *j, void *ud) {
  (void)ud; if (st != 200 || !cJSON_IsArray(j)) return; cJSON *m = cJSON_CreateObject(); const cJSON *r;
  cJSON_ArrayForEach(r, j) { const char *id = js_str(r, "id", ""), *sp = js_str(r, "storage_path", ""); if (*id && *sp && !cJSON_GetObjectItemCaseSensitive(m, id)) cJSON_AddStringToObject(m, id, sp); }
  store_set_node("nvAvCat", m); char t[24]; snprintf(t, sizeof t, "%.0f", (double)time(NULL)); store_set("nvAvAt", t); store_flush();
}
static void avatars_pull(void) {
  const cJSON *cat = store_node("nvAvCat"), *p; int missing = 0; double now = (double)time(NULL), mono = SDL_GetTicks() / 1000.0;
  cJSON_ArrayForEach(p, store_node("nvProfiles")) { const char *aid = js_str(p, "aid", ""); if (*aid && !js_str(p, "au", NULL) && !js_str(cat, aid, NULL)) missing = 1; }
  if (!missing && cat && now - atof(store_get("nvAvAt", "0")) < 86400) return;
  if (avAskedAt && mono - avAskedAt < 900) return; avAskedAt = mono;
  rpc("get_avatar_catalog", NULL, on_avatars, NULL);
}
static void on_profiles(int st, const cJSON *j, void *ud) {
  Box *x = ud;
  if (st != 200 || !cJSON_IsArray(j)) { if (x->cb) x->cb(store_node("nvProfiles") != NULL, st == 401 ? "signed out" : "Couldn\xe2\x80\x99t load your profiles", x->ud); free(x); return; }
  cJSON *out = cJSON_CreateArray(); const cJSON *row;
  cJSON_ArrayForEach(row, j) { int idx = (int)js_num(row, "profile_index", 0); if (idx < 1) continue; cJSON *p = cJSON_CreateObject();
    char nm[64]; snprintf(nm, sizeof nm, "%s", js_str(row, "name", "")); if (!nm[0]) snprintf(nm, sizeof nm, "Profile %d", idx);
    cJSON_AddNumberToObject(p, "i", idx); cJSON_AddStringToObject(p, "name", nm); cJSON_AddNumberToObject(p, "color", hex_color(js_str(row, "avatar_color_hex", NULL)));
    cJSON_AddBoolToObject(p, "pin", 0); cJSON_AddBoolToObject(p, "pa", idx != 1 && js_bool(row, "uses_primary_addons", 0));
    const char *aid = js_str(row, "avatar_id", ""), *au = js_str(row, "avatar_url", ""); if (*aid) cJSON_AddStringToObject(p, "aid", aid); if (*au) cJSON_AddStringToObject(p, "au", au);
    cJSON_AddItemToArray(out, p); }
  if (!cJSON_GetArraySize(out)) { cJSON *p = cJSON_CreateObject(); cJSON_AddNumberToObject(p, "i", 1); cJSON_AddStringToObject(p, "name", "Profile 1"); cJSON_AddNumberToObject(p, "color", 0x1E88E5); cJSON_AddBoolToObject(p, "pin", 0); cJSON_AddBoolToObject(p, "pa", 0); cJSON_AddItemToArray(out, p); }
  store_set_node("nvProfiles", out); store_flush(); avatars_pull();
  rpc("sync_pull_profile_locks", NULL, on_locks, x);
}
static void on_owner(int st, const cJSON *j, void *ud) { /* whose rows these are: the account the profile data belongs to */
  Box *x = ud; const cJSON *row = cJSON_IsArray(j) ? cJSON_GetArrayItem(j, 0) : j; const char *id = cJSON_IsString(row) ? row->valuestring : js_str(row, "get_sync_owner", NULL);
  if (st == 200 && id && *id) { store_set("nvOwner", id); store_flush(); rpc("sync_pull_profiles", NULL, on_profiles, x); return; }
  if (x->cb) x->cb(0, st == 401 ? "signed out" : "Couldn\xe2\x80\x99t load your profiles", x->ud); free(x);
}
void nv_pull_profiles(NvDoneCb cb, void *ud) { Box *x = calloc(1, sizeof *x); x->cb = cb; x->ud = ud; if (store_get("nvOwner", NULL)) rpc("sync_pull_profiles", NULL, on_profiles, x); else rpc("get_sync_owner", NULL, on_owner, x); }
int nv_profile_count(void) { const cJSON *a = store_node("nvProfiles"); return cJSON_IsArray(a) ? cJSON_GetArraySize(a) : 0; }
int nv_profile_get(int i, NvProfile *o) {
  const cJSON *p = cJSON_GetArrayItem(store_node("nvProfiles"), i); if (!p) return 0; memset(o, 0, sizeof *o);
  o->index = (int)js_num(p, "i", 1); snprintf(o->name, sizeof o->name, "%s", js_str(p, "name", "")); o->color = (unsigned)js_num(p, "color", 0x1E88E5); o->pin = js_bool(p, "pin", 0); o->primaryAddons = js_bool(p, "pa", 0);
  const char *au = js_str(p, "au", ""), *sp = js_str(store_node("nvAvCat"), js_str(p, "aid", ""), NULL);
  if (!strncmp(au, "http", 4)) snprintf(o->avatar, sizeof o->avatar, "%s", au);
  else if (sp && !strncmp(sp, "http", 4)) snprintf(o->avatar, sizeof o->avatar, "%s", sp);
  else if (sp && *sp) { init(); snprintf(o->avatar, sizeof o->avatar, "%s" AVATAR_URL_PATH "%s", nvUrl, sp + (*sp == '/')); }
  return 1;
}
int nv_profile_current(void) { return atoi(store_get("nvProfile", "0")); }
const char *nv_profile_name(void) { static char nm[64]; NvProfile p; int cur = nv_profile_current(); nm[0] = 0;
  for (int i = 0; i < nv_profile_count(); i++) if (nv_profile_get(i, &p) && p.index == cur) snprintf(nm, sizeof nm, "%s", p.name); return nm; }
static void on_pin(int st, const cJSON *j, void *ud) {
  Box *x = ud; int ok = st == 200 && (cJSON_IsTrue(j) || (cJSON_IsArray(j) && cJSON_IsTrue(cJSON_GetArrayItem(j, 0))) || js_bool(cJSON_IsArray(j) ? cJSON_GetArrayItem(j, 0) : j, "valid", 0) || js_bool(cJSON_IsArray(j) ? cJSON_GetArrayItem(j, 0) : j, "verify_profile_pin", 0));
  x->cb(ok, ok ? NULL : st == 200 ? "Wrong PIN" : "Couldn\xe2\x80\x99t check the PIN", x->ud); free(x);
}
void nv_profile_verify_pin(int index, const char *pin, NvDoneCb cb, void *ud) {
  Box *x = calloc(1, sizeof *x); x->cb = cb; x->ud = ud; cJSON *p = cJSON_CreateObject(); cJSON_AddNumberToObject(p, "p_profile_id", index); cJSON_AddStringToObject(p, "p_pin", pin);
  rpc("verify_profile_pin", p, on_pin, x);
}
int nv_profile_kept(int index) { /* this device already holds that profile's addons and library */
  if (index == nv_profile_current()) return sb_addons() != NULL;
  char s[160], p[1400]; struct stat st; nv_scope(index, s, sizeof s); snprintf(p, sizeof p, "%s/acct/%s.json", store_data_dir(), s); return stat(p, &st) == 0;
}
/* one more profile, with the primary's addons. Nuvio's push takes the account's whole list and this device keeps only part of
   each row, so it always starts from a fresh pull and sends every row back with all it came with. A list that looks wrong: nothing sent */
#define NV_MAX_PROFILES 6
typedef struct { NvDoneCb cb; void *ud; char name[64]; int index; } AddBox;
static void add_fail(AddBox *x) { x->cb(0, "Couldn\xe2\x80\x99t add the profile", x->ud); free(x); }
static void row_copy(cJSON *to, const cJSON *row, const char *k) { const cJSON *v = row ? cJSON_GetObjectItemCaseSensitive(row, k) : NULL; cJSON_AddItemToObject(to, k, v ? cJSON_Duplicate(v, 1) : cJSON_CreateNull()); }
static cJSON *row_full(int index, const char *name, const char *color, const cJSON *row, int primaryAddons, int primaryPlugins) {
  cJSON *o = cJSON_CreateObject(); cJSON_AddNumberToObject(o, "profile_index", index); cJSON_AddStringToObject(o, "name", name); cJSON_AddStringToObject(o, "avatar_color_hex", color);
  row_copy(o, row, "avatar_id"); row_copy(o, row, "avatar_url"); row_copy(o, row, "profile_background_id"); row_copy(o, row, "profile_background_url");
  cJSON_AddBoolToObject(o, "uses_primary_addons", primaryAddons); cJSON_AddBoolToObject(o, "uses_primary_plugins", primaryPlugins); return o;
}
static void on_add_pushed(int st, const cJSON *j, void *ud) { (void)j; AddBox *x = ud;
  if (st < 200 || st >= 300) { add_fail(x); return; }
  char s[160], p[1400]; nv_scope(x->index, s, sizeof s); snprintf(p, sizeof p, "%s/acct/%s.json", store_data_dir(), s); remove(p); /* (a slot freed on the phone: never the old profile's copy) */
  nv_pull_profiles(x->cb, x->ud); free(x);
}
static void on_add_pulled(int st, const cJSON *j, void *ud) {
  static const char *const COLORS[NV_MAX_PROFILES + 1] = { "#1E88E5", "#1E88E5", "#E53935", "#43A047", "#FB8C00", "#8E24AA", "#00ACC1" };
  AddBox *x = ud; if (st != 200 || !cJSON_IsArray(j)) { add_fail(x); return; }
  int used[NV_MAX_PROFILES + 1] = { 0 }, n = 0; const cJSON *row;
  cJSON_ArrayForEach(row, j) { int idx = (int)js_num(row, "profile_index", 0); if (idx < 1 || idx > NV_MAX_PROFILES || used[idx]) { add_fail(x); return; } used[idx] = 1; n++;
    if (!strcasecmp(js_str(row, "name", ""), x->name)) { nv_pull_profiles(x->cb, x->ud); free(x); return; } } /* already there (a retry): nothing to send */
  if (n && !used[1]) { add_fail(x); return; }
  for (int i = 2; i <= NV_MAX_PROFILES && !x->index; i++) if (!used[i]) x->index = i;
  if (!x->index) { add_fail(x); return; }
  cJSON *list = cJSON_CreateArray(); if (!n) cJSON_AddItemToArray(list, row_full(1, "Profile 1", COLORS[1], NULL, 0, 0)); /* an account that never saved its first profile */
  cJSON_ArrayForEach(row, j) { int idx = (int)js_num(row, "profile_index", 0); char nm[64]; snprintf(nm, sizeof nm, "Profile %d", idx); const char *name = js_str(row, "name", ""), *color = js_str(row, "avatar_color_hex", COLORS[1]);
    cJSON_AddItemToArray(list, row_full(idx, *name ? name : nm, color, row, js_bool(row, "uses_primary_addons", 0), js_bool(row, "uses_primary_plugins", 0))); }
  cJSON_AddItemToArray(list, row_full(x->index, x->name, COLORS[x->index], NULL, 1, 0));
  cJSON *body = cJSON_CreateObject(); cJSON_AddNumberToObject(body, "p_client_max_profiles", NV_MAX_PROFILES); cJSON_AddItemToObject(body, "p_profiles", list);
  rpc("sync_push_profiles", body, on_add_pushed, x);
}
void nv_profile_add(const char *name, NvDoneCb cb, void *ud) { AddBox *x = calloc(1, sizeof *x); x->cb = cb; x->ud = ud; snprintf(x->name, sizeof x->name, "%s", name); rpc("sync_pull_profiles", NULL, on_add_pulled, x); }
void nv_profile_select(int index) {
  int cur = nv_profile_current(); if (cur == index) return;
  char from[160], to[160]; if (cur) nv_scope(cur, from, sizeof from); nv_scope(index, to, sizeof to);
  syncGen++; store_scope_swap(cur ? from : NULL, to, SCOPED, NSCOPED); /* the old profile's data set aside, this one's back: instant, nothing downloaded */
  char v[16]; snprintf(v, sizeof v, "%d", index); store_set("nvProfile", v); lib_invalidate(); recs_account_changed();
}
const char *nv_user_key(void) { static char b[128]; snprintf(b, sizeof b, "%s#%d", store_get("nvOwner", ""), nv_profile_current()); return b; } /* whose a remembered stream is */
static int addon_profile(void) { NvProfile p; int cur = nv_profile_current();
  for (int i = 0; i < nv_profile_count(); i++) if (nv_profile_get(i, &p) && p.index == cur) return p.primaryAddons ? 1 : cur; return cur ? cur : 1; }

/* ---- addons: each manifest kept with the list; a dead or slow one never takes its addon away ------ */
#define MANIFEST_MAX_AGE (24 * 3600)
typedef struct { void (*cb)(int, void *); void *ud; int n, left, gen, fetched[64]; char urls[64][ADDON_URL_MAX], names[64][96]; cJSON *man[64]; } AddReq;
static int addonsForce, addonsRetries; static double addonsRetryAt;
void nv_addons_force_next(void) { addonsForce = 1; }
static void add_end(AddReq *r, int changed) { if (r->cb) r->cb(changed, r->ud); for (int i = 0; i < 64; i++) cJSON_Delete(r->man[i]); free(r); }
static cJSON *old_entry(const char *url) { cJSON *a, *old = sb_addons(); if (old) cJSON_ArrayForEach(a, old) if (!strcmp(js_str(a, "transportUrl", ""), url)) return a; return NULL; }
static void add_done(AddReq *r) {
  if (r->gen != syncGen) { add_end(r, 0); return; } /* another profile since it was asked */
  cJSON *old = sb_addons(); cJSON *o[64]; int have[64], kept = 0, missing = 0, changed = 0, nOld = old ? cJSON_GetArraySize(old) : 0; double now = (double)time(NULL);
  for (int i = 0; i < r->n; i++) { o[i] = old_entry(r->urls[i]); have[i] = r->man[i] || o[i]; kept += have[i]; if (!have[i]) { missing++; const char *h = strstr(r->urls[i], "://"); fprintf(stderr, "[nuvio] addon waiting for its manifest: %.40s\n", h ? h + 3 : r->urls[i]); } }
  if (!kept) { fprintf(stderr, "[nuvio] addons: none answered, list kept (%d)\n", nOld); if (missing && addonsRetries < 3) { addonsRetries++; addonsRetryAt = now + 60; } add_end(r, 0); return; }
  for (int i = 0; i < r->n; i++) if (r->man[i] && r->names[i][0]) { cJSON_DeleteItemFromObjectCaseSensitive(r->man[i], "name"); cJSON_AddStringToObject(r->man[i], "name", r->names[i]); } /* the name you gave it in Nuvio */
  changed = !old || nOld != kept;
  if (!changed) { cJSON *a = old->child; for (int i = 0; i < r->n; i++) { if (!have[i]) continue; if (a != o[i]) changed = 1; else if (r->man[i] && !cJSON_Compare(js_obj(a, "manifest"), r->man[i], 1)) changed = 1; if (a) a = a->next; } }
  if (!changed) { for (int i = 0; i < r->n; i++) if (r->fetched[i] && r->man[i] && o[i]) { cJSON_DeleteItemFromObjectCaseSensitive(o[i], "nvAt"); cJSON_AddNumberToObject(o[i], "nvAt", now); } if (old) store_set_node("addons", old); }
  else { cJSON *out = cJSON_CreateArray(); /* the kept ones moved across, not copied */
    for (int i = 0; i < r->n; i++) { if (!have[i]) continue; cJSON *e;
      if (r->man[i]) { e = cJSON_CreateObject(); cJSON_AddStringToObject(e, "transportUrl", r->urls[i]); cJSON_AddItemToObject(e, "manifest", r->man[i]); r->man[i] = NULL; cJSON_AddObjectToObject(e, "flags"); cJSON_AddNumberToObject(e, "nvAt", now); }
      else e = cJSON_DetachItemViaPointer(old, o[i]);
      cJSON_AddItemToArray(out, e); }
    store_set_node("addons", out); }
  fprintf(stderr, "[nuvio] addons: %d -> %d%s%s\n", nOld, kept, changed ? " (changed)" : "", missing ? " (some still to come)" : "");
  if (missing && addonsRetries < 3) { addonsRetries++; addonsRetryAt = now + 60; } else if (!missing) addonsRetries = 0;
  add_end(r, changed);
}
typedef struct { AddReq *r; int i; } AddOne;
static void on_manifest(int st, const unsigned char *b, size_t n, void *ud) {
  AddOne *o = ud; AddReq *r = o->r; cJSON *m = st == 200 && b && n ? cJSON_ParseWithLength((const char *)b, n) : NULL;
  if (m && !js_str(m, "id", NULL)) { cJSON_Delete(m); m = NULL; }
  if (!m) { const char *h = strstr(r->urls[o->i], "://"); fprintf(stderr, "[nuvio] manifest failed (HTTP %d), its copy stays: %.40s\n", st, h ? h + 3 : r->urls[o->i]); }
  r->man[o->i] = m; free(o); if (--r->left == 0) add_done(r);
}
static void addons_from_rows(AddReq *r, const cJSON *j) {
  /* in Nuvio's order (sort_order / position), each address as a manifest's; switched off in Nuvio: left out */
  const cJSON *rows[64]; int k = 0; const cJSON *row; int force = addonsForce; addonsForce = 0; double now = (double)time(NULL);
  cJSON_ArrayForEach(row, j) { const char *u = js_str(row, "url", js_str(row, "base_url", NULL)); const cJSON *en = cJSON_GetObjectItemCaseSensitive(row, "enabled");
    if (k < 64 && u && *u && !cJSON_IsFalse(en)) rows[k++] = row; }
  for (int a = 1; a < k; a++) { const cJSON *t = rows[a]; int b = a - 1; double tv = js_num(t, "sort_order", js_num(t, "position", a));
    while (b >= 0 && js_num(rows[b], "sort_order", js_num(rows[b], "position", b)) > tv) { rows[b + 1] = rows[b]; b--; } rows[b + 1] = t; }
  r->n = 0;
  for (int i = 0; i < k; i++) { const char *u = js_str(rows[i], "url", js_str(rows[i], "base_url", "")); const char *q = strchr(u, '?'); size_t base = q ? (size_t)(q - u) : strlen(u); while (base && u[base - 1] == '/') base--;
    int hasMan = base >= 14 && !strncmp(u + base - 14, "/manifest.json", 14); int dup = 0;
    char url[ADDON_URL_MAX]; if (hasMan) snprintf(url, sizeof url, "%s", u); else snprintf(url, sizeof url, "%.*s/manifest.json%s", (int)base, u, q ? q : "");
    for (int d = 0; d < r->n; d++) if (!strcmp(r->urls[d], url)) dup = 1;
    if (!dup) { const char *dn = js_str(rows[i], "display_name", js_str(rows[i], "custom_name", js_str(rows[i], "alias", ""))); snprintf(r->names[r->n], 96, "%s", dn); snprintf(r->urls[r->n++], sizeof r->urls[0], "%s", url); } }
  if (!r->n) { add_end(r, 0); return; } /* an empty answer never wipes the list here */
  /* a manifest younger than a day is not asked for again: a start costs one small query, not one per addon */
  for (int i = 0; i < r->n; i++) { cJSON *e = old_entry(r->urls[i]); r->fetched[i] = force || !e || now - js_num(e, "nvAt", 0) > MANIFEST_MAX_AGE; if (r->fetched[i]) r->left++; }
  if (!r->left) { add_done(r); return; }
  for (int i = 0; i < r->n; i++) if (r->fetched[i]) { AddOne *o = malloc(sizeof *o); o->r = r; o->i = i; net_get_timeout(r->urls[i], 15000, on_manifest, o); }
}
static void on_addons_rpc(int st, const cJSON *j, void *ud) { AddReq *r = ud; if (st == 200 && cJSON_IsArray(j)) addons_from_rows(r, j); else add_end(r, 0); }
static void on_addons_tv(int st, const cJSON *j, void *ud) {
  AddReq *r = ud; if (st == 200 && cJSON_IsArray(j) && cJSON_GetArraySize(j)) { addons_from_rows(r, j); return; }
  cJSON *p = cJSON_CreateObject(); cJSON_AddNumberToObject(p, "p_profile_id", addon_profile()); rpc("sync_pull_addons", p, on_addons_rpc, r);
}
static void on_addons(int st, const cJSON *j, void *ud) {
  AddReq *r = ud; if (st == 200 && cJSON_IsArray(j) && cJSON_GetArraySize(j)) { addons_from_rows(r, j); return; }
  char q[300]; snprintf(q, sizeof q, "owner_id=eq.%s&select=*&order=position.asc", store_get("nvOwner", "")); table_get("tv_addons", q, on_addons_tv, r);
}
/* the profile's saved keys (Nuvio's own apps keep them per profile): its TMDB key turns on the recommendations without
   anyone typing one. Only a few names come back: the rows hold other services' keys too, which are left alone */
static void on_creds(int st, const cJSON *j, void *ud) {
  (void)ud; if (st != 200 || !cJSON_IsArray(j)) { fprintf(stderr, "[nuvio] saved keys: http %d\n", st); return; }
  int has = 0; const cJSON *r; cJSON_ArrayForEach(r, j) { const char *pv = js_str(r, "provider", ""); if (!strcmp(pv, "tmdb") && js_str(js_obj(r, "credential_json"), "api_key", "")[0]) has = 1; }
  if (strcmp(store_get("nvHasTmdb", ""), has ? "1" : "0")) store_set("nvHasTmdb", has ? "1" : "0"); /* (Settings > Keys offers it when another key is in use, or none) */
  cJSON_ArrayForEach(r, j) { const char *pv = js_str(r, "provider", ""); if (strcmp(pv, "tmdb")) continue;
    const char *k = js_str(js_obj(r, "credential_json"), "api_key", ""); fprintf(stderr, "[nuvio] the account has %s TMDB key\n", k[0] ? "a" : "no");
    if (k[0] && tmdb_key_offer(k, 1) == TKO_CHECKING) fprintf(stderr, "[nuvio] its TMDB key is being checked\n"); }
}
static double credsAt;
static void pull_creds(void) {
  double now = SDL_GetTicks() / 1000.0; if (credsAt && now - credsAt < 600) return; credsAt = now; /* (at most every 10 minutes) */
  cJSON *p = cJSON_CreateObject(); cJSON_AddNumberToObject(p, "p_profile_id", nv_profile_current()); rpc("sync_pull_provider_credentials", p, on_creds, NULL);
}
int nv_has_tmdb_key(void) { return nv_signed_in() && !strcmp(store_get("nvHasTmdb", ""), "1"); }
void nv_use_tmdb_key(void) { /* "Use the key from your Nuvio account": the user's own key goes, the account's is asked for at once */
  if (!nv_signed_in() || !nv_profile_current()) return;
  if (tmdb_has_key() && !tmdb_key_from_nuvio()) tmdb_key_clear();
  store_remove("tmdbNoNuvio"); credsAt = 0; pull_creds();
}
void nv_sync_addons(void (*cb)(int, void *), void *ud) {
  if (!nv_signed_in() || !nv_profile_current() || !store_get("nvOwner", NULL)) { if (cb) cb(0, ud); return; }
  if (!tmdb_builtin()) pull_creds();
  AddReq *r = calloc(1, sizeof *r); r->cb = cb; r->ud = ud; r->gen = syncGen;
  char q[300]; snprintf(q, sizeof q, "user_id=eq.%s&profile_id=eq.%d&select=*&order=sort_order.asc", store_get("nvOwner", ""), addon_profile());
  table_get("addons", q, on_addons, r);
}

/* ---- library, progress, watched ----------------------------------------------------------------
   Pulled here (bodies copied, not parsed), the device library copied a few titles a frame, the rest on a thread:
   parsing, merging and building what goes back. The results come back a few titles a frame. No frame pays for it. */
enum { J_PULL, J_SNAP, J_MERGE, J_APPLY };
typedef struct Job { void (*cb)(int, void *); void *ud; int phase, stage, off, page, gen, profile, libCapped, watCapped, changed, restarts, at, orphan; char *bodies[24]; int kind[24], nb;
  cJSON *snap, *walk; unsigned libV; double startMs, last; char scope[160], client[48]; int done; cJSON *rows[3], *prevIds, *prevW; NmIn in; NmOut out; } Job;
static Job *J; static int pendSync; static void (*pendCb)(int, void *); static void *pendUd;
static Uint32 syncT; static double pushAt, lastSyncDone; static int fillDirty; static double fillAt;
unsigned nv_lib_sync_age(void) { return syncT ? SDL_GetTicks() - syncT : 0xffffffffu; }
void nv_push_soon(void) { if (nv_signed_in() && !pushAt) pushAt = SDL_GetTicks() / 1000.0 + 5; }
static void job_free(Job *j) { for (int i = 0; i < j->nb; i++) free(j->bodies[i]); cJSON_Delete(j->snap); for (int i = 0; i < 3; i++) cJSON_Delete(j->rows[i]); cJSON_Delete(j->prevIds); cJSON_Delete(j->prevW); nm_out_free(&j->out); free(j); }
static void job_end(Job *j, int ok) {
  void (*cb)(int, void *) = j->cb; void *ud = j->ud; int ch = ok && j->changed; J = NULL; job_free(j); lastSyncDone = SDL_GetTicks() / 1000.0;
  if (cb) cb(ch, ud);
  if (pendSync) { pendSync = 0; nv_sync_library(pendCb, pendUd); }
}
static void job_step(Job *j);
static int count_rows(const char *b) { int n = 0; for (const char *p = b; (p = strstr(p, "\"content_id\"")); p += 12) n++; return n; } /* (the page's length, no parse here) */
static void on_pull(int st, const unsigned char *b, size_t n, void *ud) {
  Job *j = ud; if (j->orphan) { job_free(j); return; }
  if (j->gen != syncGen) { job_end(j, 0); return; }
  if (st != 200 || !b || !n || *b != '[' || j->nb == 24) { fprintf(stderr, "[nuvio] sync stopped at stage %d (HTTP %d)\n", j->stage, st); job_end(j, 0); return; }
  char *c = malloc(n + 1); if (!c) { job_end(j, 0); return; } memcpy(c, b, n); c[n] = 0; j->bodies[j->nb] = c; j->kind[j->nb++] = j->stage;
  int rows = count_rows(c);
  if (j->stage == 0) { if (rows >= 500) { if (j->off < 4500) { j->off += 500; job_step(j); return; } j->libCapped = 1; } j->stage = 1; }
  else if (j->stage == 1) { j->stage = 2; j->page = 1; }
  else { if (rows >= 900) { if (j->page < 10) { j->page++; job_step(j); return; } j->watCapped = 1; }
    j->last = atof(store_get("nvLastSync", "0")); j->snap = cJSON_CreateObject(); j->walk = lib_all()->child; j->libV = lib_version(); j->phase = J_SNAP; return; }
  job_step(j);
}
static void job_step(Job *j) {
  char body[160]; const char *fn;
  if (j->stage == 0) { fn = "sync_pull_library"; snprintf(body, sizeof body, "{\"p_profile_id\":%d,\"p_limit\":500,\"p_offset\":%d}", j->profile, j->off); }
  else if (j->stage == 1) { fn = "sync_pull_watch_progress"; snprintf(body, sizeof body, "{\"p_profile_id\":%d}", j->profile); }
  else { fn = "sync_pull_watched_items"; snprintf(body, sizeof body, "{\"p_profile_id\":%d,\"p_page\":%d,\"p_page_size\":900}", j->profile, j->page); }
  rpc_body(fn, strdup(body), on_pull, j);
}
static cJSON *read_json(const char *p) { FILE *f = fopen(p, "rb"); if (!f) return NULL; fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); char *b = n > 0 ? malloc((size_t)n + 1) : NULL; cJSON *j = NULL; if (b && fread(b, 1, (size_t)n, f) == (size_t)n) { b[n] = 0; j = cJSON_Parse(b); } free(b); fclose(f); return j; }
static void *merge_thread(void *a) {
  plat_thread_background(15); Job *j = a;
  for (int k = 0; k < 3; k++) j->rows[k] = cJSON_CreateArray();
  for (int i = 0; i < j->nb; i++) { cJSON *t = cJSON_Parse(j->bodies[i]); free(j->bodies[i]); j->bodies[i] = NULL; cJSON *to = j->rows[j->kind[i]];
    if (t) { cJSON *e; while ((e = t->child)) cJSON_AddItemToArray(to, cJSON_DetachItemViaPointer(t, e)); cJSON_Delete(t); } }
  char p[1400]; snprintf(p, sizeof p, "%s/acct/%s.keys.json", store_data_dir(), j->scope); cJSON *kept = read_json(p);
  j->prevIds = kept ? cJSON_DetachItemFromObjectCaseSensitive(kept, "ids") : NULL; j->prevW = kept ? cJSON_DetachItemFromObjectCaseSensitive(kept, "w") : NULL; cJSON_Delete(kept);
  NmIn *in = &j->in; memset(in, 0, sizeof *in); in->lib = j->snap; in->rLib = j->rows[0]; in->rProg = j->rows[1]; in->rWat = j->rows[2]; in->prevIds = j->prevIds; in->prevW = j->prevW; in->last = j->last; in->now = j->startMs; in->profile = j->profile; in->client = j->client; in->libCapped = j->libCapped; in->watCapped = j->watCapped;
  nm_merge(in, &j->out);
  { cJSON *o = cJSON_CreateObject(); cJSON_AddItemReferenceToObject(o, "ids", j->out.libIds); cJSON_AddItemReferenceToObject(o, "w", j->out.wKeys); char *t = cJSON_PrintUnformatted(o); cJSON_Delete(o);
    if (t) { char tmp[1440]; snprintf(tmp, sizeof tmp, "%s.tmp", p); FILE *f = fopen(tmp, "wb"); if (f) { size_t len = strlen(t), w = fwrite(t, 1, len, f); fclose(f); if (w == len) rename(tmp, p); else remove(tmp); } free(t); } }
  __atomic_store_n(&j->done, 1, __ATOMIC_RELEASE); return NULL;
}
void nv_sync_library(void (*cb)(int, void *), void *ud) {
  if (!nv_signed_in() || !nv_profile_current()) { if (cb) cb(0, ud); return; }
  if (J && J->gen == syncGen) { if (cb) cb(0, ud); return; } /* one at a time */
  if (J && J->phase == J_MERGE) { pendSync = 1; pendCb = cb; pendUd = ud; return; } /* the old profile's thread finishes first */
  if (J) { Job *o = J; J = NULL; if (o->cb) o->cb(0, o->ud); o->cb = NULL; if (o->phase == J_PULL) o->orphan = 1; else job_free(o); }
  if (!store_get("nvClient", NULL)) { char c[40]; unsigned s = (unsigned)time(NULL) ^ SDL_GetTicks() * 2246822519u; for (int i = 0; i < 32; i++) { s = s * 1103515245u + 12345u; c[i] = "abcdef0123456789"[(s >> 16) & 15]; } c[32] = 0; store_set("nvClient", c); }
  syncT = SDL_GetTicks(); pushAt = 0;
  Job *j = calloc(1, sizeof *j); j->cb = cb; j->ud = ud; j->gen = syncGen; j->profile = nv_profile_current(); j->startMs = now_ms(); nv_scope(j->profile, j->scope, sizeof j->scope); snprintf(j->client, sizeof j->client, "%s", store_get("nvClient", "")); J = j; job_step(j);
}
/* names and pictures for titles that came down without them (progress rows carry none): one change for all of them */
static void on_meta_fill(cJSON *m, void *ud) {
  char *id = ud; cJSON *l = lib_get(id); free(id); if (!l || !m) return;
  static const char *const k[4] = { "name", "poster", "background", "logo" };
  for (int i = 0; i < 4; i++) { const char *v = js_str(m, k[i], ""); if (*v && !js_str(l, k[i], "")[0]) { cJSON_DeleteItemFromObjectCaseSensitive(l, k[i]); cJSON_AddStringToObject(l, k[i], v); fillDirty = 1; fillAt = SDL_GetTicks() / 1000.0 + 1; } }
}
static void fill_names(void) {
  int n = 0; cJSON *it; cJSON_ArrayForEach(it, lib_all()) { if (n >= 40) break; if (js_str(it, "name", "")[0]) continue;
    sb_meta(js_str(it, "type", "movie"), js_str(it, "_id", ""), on_meta_fill, strdup(js_str(it, "_id", ""))); n++; }
}
#define SNAP_SLICE 40
#define APPLY_SLICE 40
static void send_body(const char *fn, char **body, int n, const char *what) { if (!*body) return; rpc_body(fn, *body, NULL, NULL); *body = NULL; if (n) fprintf(stderr, "[nuvio] %s pushed: %d\n", what, n); }
static void job_tick(Job *j) {
  if (j->phase == J_PULL) return;
  if (j->phase == J_MERGE) { if (!__atomic_load_n(&j->done, __ATOMIC_ACQUIRE)) return; j->phase = J_APPLY; j->at = 0; }
  if (j->gen != syncGen) { job_end(j, 0); return; }
  if (j->phase == J_SNAP) {
    if (lib_version() != j->libV) { if (++j->restarts > 3) { job_end(j, 0); return; } cJSON_Delete(j->snap); j->snap = cJSON_CreateObject(); j->walk = lib_all()->child; j->libV = lib_version(); return; } /* changed under the walk (it holds a pointer in) */
    for (int k = 0; k < SNAP_SLICE && j->walk; k++, j->walk = j->walk->next) { cJSON *d = cJSON_Duplicate(j->walk, 1); if (d && d->string) cJSON_AddItemToObject(j->snap, j->walk->string, d); else cJSON_Delete(d); }
    if (j->walk) return;
    j->phase = J_MERGE; pthread_t t; if (pthread_create(&t, NULL, merge_thread, j)) { j->phase = J_APPLY; job_end(j, 0); } else pthread_detach(t);
    return; }
  /* J_APPLY: into the live library; a title changed here since the walk began is left for the next sync */
  cJSON *lib = lib_all(); NmOut *o = &j->out;
  for (int k = 0; k < APPLY_SLICE && j->at < o->nChanged; k++, j->at++) { cJSON *it = o->changed[j->at]; const char *id = js_str(it, "_id", ""); cJSON *live = lib_get(id);
    if (live && nm_row_ms(cJSON_GetObjectItemCaseSensitive(live, "_mtime")) >= j->startMs - 1000) continue;
    cJSON_DetachItemViaPointer(j->snap, it);
    if (live) cJSON_ReplaceItemViaPointer(lib, live, it); else cJSON_AddItemToObject(lib, js_str(it, "_id", ""), it);
    j->changed = 1; }
  if (j->changed) store_set_node("lib", lib); /* (the store's serial moves: whoever walks the library starts its walk again) */
  if (j->at < o->nChanged) return;
  if (o->refusedShrink) fprintf(stderr, "[nuvio] library push refused: it would have emptied most of the list\n");
  send_body("sync_push_library", &o->pushLib, o->nLib, "library");
  send_body("sync_delete_watch_progress", &o->delProg, 0, "");
  send_body("sync_push_watch_progress", &o->pushProg, o->nProg, "progress");
  send_body("sync_delete_watched_items", &o->delW, 0, "");
  send_body("sync_push_watched_items", &o->pushW, o->nW, "watched");
  char t[32]; snprintf(t, sizeof t, "%.0f", j->startMs); store_set("nvLastSync", t);
  if (j->changed) lib_commit();
  fill_names(); job_end(j, 1);
}
void nv_tick(void) {
  double now = SDL_GetTicks() / 1000.0;
  if (J) job_tick(J);
  if (pushAt > 0 && now >= pushAt && !J && (!player_open() || now - lastSyncDone >= 180)) { pushAt = 0; nv_sync_library(NULL, NULL); } /* (during a film at most every 3 minutes; stopping sends it at once) */
  if (fillDirty && now >= fillAt) { fillDirty = 0; lib_commit(); }
  if (addonsRetryAt > 0 && (double)time(NULL) >= addonsRetryAt) { addonsRetryAt = 0; nv_sync_addons(NULL, NULL); }
  { static int asked; if (!asked && now > 4 && nv_signed_in()) { asked = 1; nv_pull_profiles(NULL, NULL); } } /* the names and PIN locks as they are now; nothing waits for it */
}
