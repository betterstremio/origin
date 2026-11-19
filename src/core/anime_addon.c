#include "core/anime_addon.h"
#include "core/net.h"
#include "core/store.h"
#include "core/stremio.h"
#include "core/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAN_KEY "animekitsu:manifest"
#define MAN_AGE (7L * 86400L)
static int on, pending;
static cJSON *man; /* the manifest */
static cJSON *merged; /* the list handed out while the account lacks the addon */
static const cJSON *deviceAddon;
static const cJSON *srcSeen;
static int srcN, manVer, mergedVer;

static int is_kitsu(const cJSON *ad) {
  return !strcmp(js_str(ad, "transportUrl", ""), ANIME_KITSU_URL) ||
         !strcmp(js_str(js_obj(ad, "manifest"), "id", ""), "community.anime.kitsu");
}
static int man_in(cJSON *j) {
  if (!cJSON_IsObject(j) || !js_arr(j, "resources") || strcmp(js_str(j, "id", ""), "community.anime.kitsu")) {
    net_json_free(j);
    return 0;
  }
  cJSON *old = man;
  man = j;
  manVer++;
  cJSON_Delete(old);
  return 1;
}
static void on_man_net(int st, const unsigned char *b, size_t n, void *ud);
static void on_man_disk(int st, const unsigned char *b, size_t n, void *ud) {
  (void)ud;
  (void)n;
  pending = 0;
  if (on && !man_in(st == 200 ? net_json(b) : NULL))
    pending = net_get(ANIME_KITSU_URL, on_man_net, NULL) != 0;
}
static void on_man_net(int st, const unsigned char *b, size_t n, void *ud) {
  (void)ud;
  pending = 0;
  if (on && man_in(st == 200 ? net_json(b) : NULL))
    cache_write(MAN_KEY, b, n);
}
void anime_addon_set(int v) {
  on = v != 0;
  if (!on) {
    cJSON_Delete(merged);
    merged = NULL;
    deviceAddon = NULL;
    srcSeen = NULL;
    manVer++; /* (the list is rebuilt on the next ask; the manifest copy stays for the next time on) */
    return;
  }
  if (pending || anime_addon_ready())
    return;
  char p[1200];
  if (cache_file(MAN_KEY, MAN_AGE, p, sizeof p))
    pending = net_file(p, on_man_disk, NULL) != 0;
  else
    pending = net_get(ANIME_KITSU_URL, on_man_net, NULL) != 0;
}
int anime_addon_ready(void) {
  if (!on)
    return 0;
  if (man)
    return 1;
  const cJSON *ad;
  cJSON_ArrayForEach(ad, sb_addons_account())
    if (is_kitsu(ad) && sb_addon_on(ad))
      return 1;
  return 0;
}
int anime_addon_is_local(const cJSON *ad) {
  return ad && ad == deviceAddon;
}
cJSON *anime_addons_merge(cJSON *raw) {
  if (!on || !man)
    return raw;
  const cJSON *ad;
  if (raw)
    cJSON_ArrayForEach(ad, raw)
      if (is_kitsu(ad) && sb_addon_on(ad))
        return raw; /* the account has it: its own entry serves */
  int n = raw ? cJSON_GetArraySize(raw) : 0;
  if (merged && srcSeen == raw && srcN == n && mergedVer == manVer)
    return merged;
  cJSON_Delete(merged);
  merged = raw ? cJSON_Duplicate(raw, 1) : cJSON_CreateArray();
  cJSON *e = cJSON_CreateObject();
  cJSON_AddStringToObject(e, "transportUrl", ANIME_KITSU_URL);
  cJSON_AddItemToObject(e, "manifest", cJSON_Duplicate(man, 1));
  cJSON_AddItemToArray(merged, e);
  deviceAddon = e;
  srcSeen = raw;
  srcN = n;
  mergedVer = manVer;
  return merged;
}
