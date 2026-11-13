/* The Nuvio merge, alone: tests/nuvio/run.sh builds this under AddressSanitizer with nuvio_merge.c and cJSON. */
#include "core/nuvio_merge.h"
#include "core/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, what)                               \
  do {                                               \
    checks++;                                        \
    if (!(c)) {                                      \
      fails++;                                       \
      printf("FAIL %s (line %d)\n", what, __LINE__); \
    }                                                \
  } while (0)

#define NOW 1800000000000.0
#define LAST 1790000000000.0
static cJSON *J(const char *s) {
  cJSON *j = cJSON_Parse(s);
  if (!j) {
    printf("bad json: %s\n", s);
    exit(2);
  }
  return j;
}
static cJSON *item(const char *id, const char *type, int removed, const char *mtime, const char *lastW, double off,
                   double dur, const char *vid) {
  char b[900];
  snprintf(
      b, sizeof b,
      "{\"_id\":\"%s\",\"name\":\"N %s\",\"type\":\"%s\",\"poster\":\"\",\"background\":\"\",\"removed\":%s,\"temp\":false,\"_ctime\":\"2026-01-01T00:00:00.000Z\",\"_mtime\":\"%s\",\"state\":{\"lastWatched\":\"%s\",\"timeOffset\":%.0f,\"duration\":%.0f,\"timesWatched\":0,\"video_id\":\"%s\",\"watched\":\"\"}}",
      id, id, type, removed ? "true" : "false", mtime, lastW, off, dur, vid);
  return J(b);
}
#define OLD "2026-01-01T00:00:00.000Z" /* before LAST */
#define NEW "2027-01-01T00:00:00.000Z" /* after LAST, before NOW */
static NmIn base(cJSON *lib, cJSON *rl, cJSON *rp, cJSON *rw) {
  NmIn in;
  memset(&in, 0, sizeof in);
  in.lib = lib;
  in.rLib = rl;
  in.rProg = rp;
  in.rWat = rw;
  in.last = LAST;
  in.now = NOW;
  in.profile = 2;
  in.client = "c1";
  return in;
}
static int has_changed(const NmOut *o, const char *id) {
  for (int i = 0; i < o->nChanged; i++)
    if (!strcmp(js_str(o->changed[i], "_id", ""), id))
      return 1;
  return 0;
}
static cJSON *obj_with(cJSON *a) {
  cJSON *o = cJSON_CreateObject();
  if (a)
    cJSON_AddItemToObject(o, js_str(a, "_id", ""), a);
  return o;
}

static void t_pull_new_title(void) {
  cJSON *
      lib = cJSON_CreateObject(),
     *rl = J(
         "[{\"content_id\":\"tt1\",\"content_type\":\"movie\",\"name\":\"One\",\"poster\":\"p\",\"added_at\":1700000000000}]"),
     *rp = J("[]"), *rw = J("[]");
  NmIn in = base(lib, rl, rp, rw);
  NmOut o;
  nm_merge(&in, &o);
  cJSON *l = cJSON_GetObjectItem(lib, "tt1");
  CHECK(l && !js_bool(l, "removed", 1), "a saved title comes down visible");
  CHECK(has_changed(&o, "tt1"), "and is reported");
  CHECK(!strcmp(js_str(l, "poster", ""), "p"), "with its poster");
  CHECK(o.pushLib == NULL, "nothing to send back");
  nm_out_free(&o);
  cJSON_Delete(lib);
  cJSON_Delete(rl);
  cJSON_Delete(rp);
  cJSON_Delete(rw);
}
static void t_removed_here_stays(void) {
  cJSON *lib = obj_with(item("tt1", "movie", 1, NEW, OLD, 0, 0, "")),
        *rl = J("[{\"content_id\":\"tt1\",\"content_type\":\"movie\",\"name\":\"One\"}]"), *e = J("[]"),
        *ids = J("[\"tt1\"]");
  NmIn in = base(lib, rl, e, e);
  in.prevIds = ids;
  NmOut o;
  nm_merge(&in, &o);
  CHECK(js_bool(cJSON_GetObjectItem(lib, "tt1"), "removed", 0), "removed on the TV stays removed");
  CHECK(!o.pushLib || !strstr(o.pushLib, "tt1"), "and the push drops it");
  nm_out_free(&o);
  cJSON_Delete(lib);
  cJSON_Delete(rl);
  cJSON_Delete(e);
  cJSON_Delete(ids);
}
static void t_removed_on_phone(void) {
  cJSON *lib = obj_with(item("tt1", "movie", 0, OLD, OLD, 0, 0, "")), *e = J("[]"), *ids = J("[\"tt1\"]");
  NmIn in = base(lib, e, e, e);
  in.prevIds = ids;
  NmOut o;
  nm_merge(&in, &o);
  CHECK(js_bool(cJSON_GetObjectItem(lib, "tt1"), "removed", 0), "removed on the phone goes here too");
  nm_out_free(&o);
  cJSON_Delete(lib);
  cJSON_Delete(e);
  cJSON_Delete(ids);
}
static void t_shrink_refused(void) {
  cJSON *lib = cJSON_CreateObject(), *rl = cJSON_CreateArray(), *e = J("[]"), *ids = cJSON_CreateArray();
  for (int i = 0; i < 40; i++) {
    char b[120];
    snprintf(b, sizeof b, "{\"content_id\":\"tt%d\",\"content_type\":\"movie\",\"name\":\"x\"}", i);
    cJSON_AddItemToArray(rl, J(b));
    snprintf(b, sizeof b, "tt%d", i);
    cJSON_AddItemToArray(ids, cJSON_CreateString(b));
    cJSON_AddItemToObject(lib, b, item(b, "movie", i >= 5, NEW, OLD, 0, 0, ""));
  }
  NmIn in = base(lib, rl, e, e);
  in.prevIds = ids;
  NmOut o;
  nm_merge(&in, &o);
  CHECK(o.refusedShrink && !o.pushLib, "a push that empties most of the list is not sent");
  nm_out_free(&o);
  cJSON_Delete(lib);
  cJSON_Delete(rl);
  cJSON_Delete(e);
  cJSON_Delete(ids);
}
static void t_capped_no_push(void) {
  cJSON *lib = obj_with(item("tt9", "movie", 0, NEW, OLD, 0, 0, "")), *e = J("[]"), *ids = J("[\"tt9\"]");
  NmIn in = base(lib, e, e, e);
  in.prevIds = ids;
  in.libCapped = 1;
  NmOut o;
  nm_merge(&in, &o);
  CHECK(!o.pushLib, "a partly fetched library is never pushed");
  CHECK(!js_bool(cJSON_GetObjectItem(lib, "tt9"), "removed", 0), "nor anything inferred from it");
  nm_out_free(&o);
  cJSON_Delete(lib);
  cJSON_Delete(e);
  cJSON_Delete(ids);
}
static void t_progress_down(void) {
  cJSON *
      lib = cJSON_CreateObject(),
     *e = J("[]"),
     *rp = J(
         "[{\"content_id\":\"tt5\",\"content_type\":\"series\",\"video_id\":\"tt5:1:2\",\"season\":1,\"episode\":2,\"position\":1000,\"duration\":90000,\"last_watched\":1795000000000,\"progress_key\":\"tt5_s1e2\"},{\"content_id\":\"tt5\",\"content_type\":\"series\",\"video_id\":\"tt5:1:1\",\"season\":1,\"episode\":1,\"position\":5,\"duration\":90000,\"last_watched\":1794000000000,\"progress_key\":\"tt5_s1e1\"}]");
  NmIn in = base(lib, e, rp, e);
  NmOut o;
  nm_merge(&in, &o);
  const cJSON *s = js_obj(cJSON_GetObjectItem(lib, "tt5"), "state");
  CHECK(s && !strcmp(js_str(s, "video_id", ""), "tt5:1:2"), "the newest row decides");
  CHECK(js_num(s, "timeOffset", 0) == 1000, "at its position");
  CHECK(!o.pushProg, "nothing changed here: nothing sent");
  nm_out_free(&o);
  cJSON_Delete(lib);
  cJSON_Delete(e);
  cJSON_Delete(rp);
}
static void t_progress_up(void) {
  cJSON *
      lib = obj_with(item("tt6", "movie", 0, NEW, NEW, 7000, 120000, "tt6")),
     *e = J("[]"),
     *rp = J(
         "[{\"content_id\":\"tt7\",\"content_type\":\"movie\",\"video_id\":\"tt7\",\"position_ms\":9,\"duration_ms\":100000,\"last_watched\":1,\"progress_key\":\"tt7\"}]");
  NmIn in = base(lib, e, rp, e);
  NmOut o;
  nm_merge(&in, &o);
  CHECK(o.pushProg && strstr(o.pushProg, "\"tt6\"") && strstr(o.pushProg, "\"tt7\""),
        "the whole set goes back, ours included");
  CHECK(o.pushProg && strstr(o.pushProg, "\"p_profile_id\":2") && strstr(o.pushProg, "\"p_origin_client_id\":\"c1\""),
        "for this profile");
  CHECK(o.pushProg && !strstr(o.pushProg, "position_ms"), "only Nuvio's fields");
  nm_out_free(&o);
  cJSON_Delete(lib);
  cJSON_Delete(e);
  cJSON_Delete(rp);
}
static void t_progress_finished(void) {
  cJSON *
      lib = obj_with(item("tt6", "movie", 0, NEW, NEW, 0, 120000, "tt6")),
     *e = J("[]"),
     *rp = J(
         "[{\"content_id\":\"tt6\",\"content_type\":\"movie\",\"video_id\":\"tt6\",\"position\":9,\"duration\":100000,\"last_watched\":1,\"progress_key\":\"tt6\"}]");
  NmIn in = base(lib, e, rp, e);
  NmOut o;
  nm_merge(&in, &o);
  CHECK(o.delProg && strstr(o.delProg, "tt6"), "a finished title's row is deleted");
  CHECK(!o.pushProg, "and no empty list goes up");
  nm_out_free(&o);
  cJSON_Delete(lib);
  cJSON_Delete(e);
  cJSON_Delete(rp);
}
static void t_watched(void) {
  cJSON *
      lib = obj_with(item("tt8", "series", 0, OLD, OLD, 0, 0, "")),
     *e = J("[]"),
     *rw = J(
         "[{\"content_id\":\"tt8\",\"content_type\":\"series\",\"title\":\"E\",\"season\":1,\"episode\":3,\"watched_at\":1795000000000}]"),
     *pw = J("[\"tt8|1|2\"]");
  cJSON *s = cJSON_GetObjectItem(cJSON_GetObjectItem(lib, "tt8"), "state");
  cJSON_AddItemToObject(s, "nvEps", J("[\"1:2\"]"));
  NmIn in = base(lib, e, e, rw);
  in.prevW = pw;
  NmOut o;
  nm_merge(&in, &o);
  const cJSON *a = js_arr(s, "nvEps");
  int has3 = 0, has2 = 0;
  const cJSON *x;
  cJSON_ArrayForEach(x, a) {
    has3 |= !strcmp(x->valuestring, "1:3");
    has2 |= !strcmp(x->valuestring, "1:2");
  }
  CHECK(has3, "an episode watched on the phone is watched here");
  CHECK(!has2, "one unwatched on the phone is unwatched here");
  CHECK(!o.pushW, "nothing new to send");
  nm_out_free(&o);
  cJSON_Delete(lib);
  cJSON_Delete(e);
  cJSON_Delete(rw);
  cJSON_Delete(pw);
}
static void t_watched_up(void) {
  cJSON *lib = obj_with(item("tt3", "movie", 0, NEW, NEW, 0, 0, "")), *e = J("[]");
  cJSON *s = cJSON_GetObjectItem(cJSON_GetObjectItem(lib, "tt3"), "state");
  cJSON_ReplaceItemInObject(s, "timesWatched", cJSON_CreateNumber(1));
  NmIn in = base(lib, e, e, e);
  NmOut o;
  nm_merge(&in, &o);
  CHECK(o.pushW && strstr(o.pushW, "tt3"), "watched here goes up");
  nm_out_free(&o);
  cJSON_Delete(lib);
  cJSON_Delete(e);
}
static void t_big(void) { /* linear: 20000 rows against a library of 20000 */
  cJSON *lib = cJSON_CreateObject(), *rl = cJSON_CreateArray(), *e = J("[]");
  for (int i = 0; i < 20000; i++) {
    char b[160];
    snprintf(b, sizeof b, "{\"content_id\":\"tt%d\",\"content_type\":\"movie\",\"name\":\"x\"}", i);
    cJSON_AddItemToArray(rl, J(b));
    snprintf(b, sizeof b, "tt%d", i);
    cJSON_AddItemToObject(lib, b, item(b, "movie", 0, OLD, OLD, 0, 0, ""));
  }
  NmIn in = base(lib, rl, e, e);
  NmOut o;
  nm_merge(&in, &o);
  CHECK(cJSON_GetArraySize(o.libIds) == 20000, "a big library merges whole");
  nm_out_free(&o);
  cJSON_Delete(lib);
  cJSON_Delete(rl);
  cJSON_Delete(e);
}
int main(void) {
  t_pull_new_title();
  t_removed_here_stays();
  t_removed_on_phone();
  t_shrink_refused();
  t_capped_no_push();
  t_progress_down();
  t_progress_up();
  t_progress_finished();
  t_watched();
  t_watched_up();
  t_big();
  printf("%d checks, %d failed\n", checks, fails);
  return fails != 0;
}
