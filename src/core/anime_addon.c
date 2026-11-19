#include "core/anime_addon.h"
#include "core/net.h"
#include "core/store.h"
#include "core/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAN_KEY "animekitsu:manifest"
#define MAN_AGE (7L * 86400L)
static int on, asked;
static cJSON *man; /* the manifest */
static cJSON *merged; /* the list handed out while the account lacks the addon */
static const cJSON *srcSeen;
static int srcN, manVer, mergedVer;

static int is_kitsu(const cJSON *ad) {
  const char *u = js_str(ad, "transportUrl", "");
  return strstr(u, "anime-kitsu.strem.fun") != NULL ||
         !strcmp(js_str(js_obj(ad, "manifest"), "id", ""), "community.anime.kitsu");
}
static void man_in(cJSON *j, int fromNet, const unsigned char *b, size_t n) {
  if (!cJSON_IsObject(j) || !js_arr(j, "resources") || !js_str(j, "id", "")[0]) {
    net_json_free(j);
    return;
  }
  cJSON *old = man;
  man = j;
  manVer++;
  if (fromNet && b && n)
    cache_write(MAN_KEY, b, n);
  cJSON_Delete(old);
}
static void on_man_disk(int st, const unsigned char *b, size_t n, void *ud) {
  (void)ud;
  (void)n;
  cJSON *j = st == 200 ? net_json(b) : NULL;
  if (!on || !j) {
    net_json_free(j);
    asked = j != NULL; /* an unreadable file: the net next time */
    return;
  }
  man_in(j, 0, NULL, 0);
}
static void on_man_net(int st, const unsigned char *b, size_t n, void *ud) {
  (void)ud;
  cJSON *j = st == 200 ? net_json(b) : NULL;
  if (!on || !j) {
    net_json_free(j);
    return;
  }
  man_in(j, 1, b, n);
}
void anime_addon_set(int v) {
  v = v != 0;
  if (v == on && (!v || asked))
    return;
  on = v;
  if (!on) {
    cJSON_Delete(merged);
    merged = NULL;
    srcSeen = NULL;
    manVer++; /* (the list is rebuilt on the next ask; the manifest copy stays for the next time on) */
    return;
  }
  asked = 1;
  if (man)
    return;
  char p[1200];
  if (cache_file(MAN_KEY, MAN_AGE, p, sizeof p))
    net_file(p, on_man_disk, NULL);
  else
    net_get(ANIME_KITSU_URL, on_man_net, NULL);
}
int anime_addon_ready(void) {
  return on && man != NULL;
}
cJSON *anime_addons_merge(cJSON *raw) {
  if (!on || !man)
    return raw;
  const cJSON *ad;
  if (raw)
    cJSON_ArrayForEach(ad, raw)
      if (is_kitsu(ad))
        return raw; /* the account has it: its own entry serves */
  int n = raw ? cJSON_GetArraySize(raw) : 0;
  if (merged && srcSeen == raw && srcN == n && mergedVer == manVer)
    return merged;
  cJSON_Delete(merged);
  merged = raw ? cJSON_Duplicate(raw, 1) : cJSON_CreateArray();
  cJSON *e = cJSON_CreateObject();
  cJSON_AddStringToObject(e, "transportUrl", ANIME_KITSU_URL);
  cJSON_AddItemToObject(e, "manifest", cJSON_Duplicate(man, 1));
  cJSON_AddItemToObject(e, "flags", cJSON_CreateObject());
  cJSON_AddItemToArray(merged, e);
  srcSeen = raw;
  srcN = n;
  mergedVer = manVer;
  return merged;
}
