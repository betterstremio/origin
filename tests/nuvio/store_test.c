/* The per-profile swap of the store: A's data set aside, B's in its place, and back again to exactly what A had. */
#include "core/store.h"
#include "core/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
void plat_thread_background(int nice) {
  (void)nice;
}

static int fails, checks;
#define CHECK(c, what)                               \
  do {                                               \
    checks++;                                        \
    if (!(c)) {                                      \
      fails++;                                       \
      printf("FAIL %s (line %d)\n", what, __LINE__); \
    }                                                \
  } while (0)
static const char *const K[] = { "lib", "addons", "nvLastSync" };
int main(int argc, char **argv) {
  if (argc < 2)
    return 2;
  store_init(argv[1]);
  store_set("uiLang", "fr");
  store_set_node("lib", cJSON_Parse("{\"tt1\":{\"_id\":\"tt1\",\"name\":\"A's film\"}}"));
  store_set_node("addons", cJSON_Parse("[{\"transportUrl\":\"a\"}]"));
  store_set("nvLastSync", "111");
  char *before = cJSON_PrintUnformatted(store_node("lib"));
  store_scope_swap("nv-x-p1", "nv-x-p2", K, 3);
  CHECK(!store_node("lib") && !store_node("addons") && !store_get("nvLastSync", NULL),
        "a profile new here starts empty");
  CHECK(!strcmp(store_get("uiLang", ""), "fr"), "settings stay with the device");
  store_set_node("lib", cJSON_Parse("{\"tt2\":{\"_id\":\"tt2\",\"name\":\"B's film\"}}"));
  store_set("nvLastSync", "222");
  for (int i = 0; i < 20; i++) {
    store_scope_swap("nv-x-p2", "nv-x-p1", K, 3);
    char *now = cJSON_PrintUnformatted(store_node("lib"));
    CHECK(now && !strcmp(now, before), "A comes back byte for byte (at once after B)");
    free(now);
    CHECK(!strcmp(store_get("nvLastSync", ""), "111"), "with its last sync");
    store_scope_swap("nv-x-p1", "nv-x-p2", K, 3);
    CHECK(!strcmp(store_get("nvLastSync", ""), "222"), "B too");
    CHECK(cJSON_GetObjectItem(store_node("lib"), "tt2") != NULL, "B's title");
  }
  free(before);
  store_scope_forget("nv-");
  char p[1200];
  struct stat st;
  snprintf(p, sizeof p, "%s/acct/nv-x-p1.json", argv[1]);
  CHECK(stat(p, &st) != 0, "sign-out leaves no file behind");
  printf("%d checks, %d failed\n", checks, fails);
  return fails != 0;
}
