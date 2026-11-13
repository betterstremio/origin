/* addon addresses: the transport URL's base, and the request URLs built on it (issue #5: Meteor carries its whole
   setup in the address, often past 512 characters; addons with a ?query keep it on every request, as the official core) */
#include "core/addons.h"
#include <stdio.h>
#include <string.h>

/* addons.c asks the store for the addon list only in code this test does not reach */
cJSON *sb_addons(void) { return NULL; }
int sb_addon_on(const cJSON *a) { (void)a; return 1; }

static int fails;
static void eq(const char *what, const char *got, const char *want) {
  if (strcmp(got, want)) { fails++; printf("FAIL %s\n  got  %s\n  want %s\n", what, got, want); } else printf("ok   %s\n", what);
}
static void base_of(const char *transport, char *out, size_t cap) {
  cJSON *ad = cJSON_CreateObject(); cJSON_AddStringToObject(ad, "transportUrl", transport); addon_base(ad, out, cap); cJSON_Delete(ad);
}
int main(void) {
  char b[ADDON_URL_MAX], u[ADDON_URL_MAX + 512];
  base_of("https://v3-cinemeta.strem.io/manifest.json", b, sizeof b); eq("plain base", b, "https://v3-cinemeta.strem.io");
  addon_url(u, sizeof u, b, "meta", "movie", "tt0816692", NULL); eq("plain meta", u, "https://v3-cinemeta.strem.io/meta/movie/tt0816692.json");
  base_of("https://x.example/abc/", b, sizeof b); eq("no manifest.json, trailing slash", b, "https://x.example/abc");

  /* a ?query stays on every request, after the path */
  base_of("https://q.example/cfg/manifest.json?token=a1&lang=hu", b, sizeof b); eq("query base", b, "https://q.example/cfg?token=a1&lang=hu");
  addon_url(u, sizeof u, b, "stream", "series", "tt1:1:2", NULL); eq("query stream", u, "https://q.example/cfg/stream/series/tt1%3A1%3A2.json?token=a1&lang=hu");
  addon_url(u, sizeof u, b, "subtitles", "movie", "tt1", "videoSize=5"); eq("query subtitles", u, "https://q.example/cfg/subtitles/movie/tt1/videoSize=5.json?token=a1&lang=hu");
  addon_join(u, sizeof u, b, "/catalog/movie/top"); eq("query catalog path", u, "https://q.example/cfg/catalog/movie/top?token=a1&lang=hu");
  { char p[ADDON_URL_MAX + 512]; snprintf(p, sizeof p, "%s", u); addon_join(u, sizeof u, p, "/skip=20.json"); eq("query catalog page", u, "https://q.example/cfg/catalog/movie/top/skip=20.json?token=a1&lang=hu"); }

  /* a Meteor-sized address (1500 characters of setup) comes through whole */
  char tok[1501]; for (int i = 0; i < 1500; i++) tok[i] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"[(i * 7) % 64]; tok[1500] = 0;
  char t[1700], want[2000];
  snprintf(t, sizeof t, "https://meteor.example/%s/manifest.json", tok); base_of(t, b, sizeof b);
  snprintf(want, sizeof want, "https://meteor.example/%s", tok); eq("long base kept whole", b, want);
  addon_url(u, sizeof u, b, "stream", "movie", "tt0816692", NULL);
  snprintf(want, sizeof want, "https://meteor.example/%s/stream/movie/tt0816692.json", tok); eq("long stream URL kept whole", u, want);
  printf(fails ? "%d FAILED\n" : "all passed\n", fails); return fails != 0;
}
