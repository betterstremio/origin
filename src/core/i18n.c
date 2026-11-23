#include <pthread.h>
#include "core/i18n.h"
#include "core/store.h"
#include "platform/plat.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>

static cJSON *dict;
static char lang[8] = "en";
static int rtl;
/* T() runs for every label in every frame: a hash index over the dictionary (a cJSON object is a list walked with
   strcmp, thousands of entries in a translated build) */
static const cJSON **idx;
static unsigned idxCap;
static unsigned hash_str(const char *s) {
  unsigned h = 2166136261u;
  while (*s)
    h = (h ^ (unsigned char)*s++) * 16777619u;
  return h;
}
static void index_build(void) {
  free(idx);
  idx = NULL;
  idxCap = 0;
  if (!dict)
    return;
  unsigned n = (unsigned)cJSON_GetArraySize(dict);
  idxCap = 64;
  while (idxCap < n * 2)
    idxCap <<= 1;
  idx = calloc(idxCap, sizeof *idx);
  const cJSON *it;
  cJSON_ArrayForEach(it, dict)
    if (it->string) {
      unsigned i = hash_str(it->string) & (idxCap - 1);
      while (idx[i] && strcmp(idx[i]->string, it->string))
        i = (i + 1) & (idxCap - 1);
      idx[i] = it;
    }
}
static const struct {
  const char *code, *name;
} languages[] = { { "en", "English" }, { "ar", "العربية" },  { "fr", "Français" },  { "es", "Español" },
                  { "de", "Deutsch" }, { "it", "Italiano" }, { "pt", "Português" }, { "pt-BR", "Português (Brasil)" },
                  { "ru", "Русский" }, { "tr", "Türkçe" },   { "pl", "Polski" },    { "nl", "Nederlands" } };

static char *read_all(const char *p) {
  FILE *f = fopen(p, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *b = malloc(n + 1);
  size_t g = fread(b, 1, n, f);
  b[g] = 0;
  fclose(f);
  return b;
}
static const char *lang_name(const char *code) {
  for (int i = 0; i < i18n_lang_count(); i++)
    if (!strcmp(languages[i].code, code))
      return languages[i].name;
  return NULL;
}
static int brazilian_locale(const char *loc) {
  return !strncasecmp(loc, "pt", 2) && (loc[2] == '-' || loc[2] == '_') && !strncasecmp(loc + 3, "BR", 2) &&
         (!loc[5] || loc[5] == '.' || loc[5] == '@');
}
/* Use the platform locale, its last saved value, or the environment. Keep the region for supported variants. */
static char tvLang[8];
const char *i18n_tv_lang(void) {
  if (tvLang[0])
    return tvLang;
  const char *loc = plat_ui_locale();
  if (loc && *loc)
    store_set("tvUiLocale", loc);
  else
    loc = store_get("tvUiLocale", NULL);
  if (!loc || !*loc)
    loc = getenv("LANG");
  if (!loc || strlen(loc) < 2)
    loc = "en";
  if (brazilian_locale(loc)) {
    snprintf(tvLang, sizeof tvLang, "%s", "pt-BR");
    return tvLang;
  }
  tvLang[0] = (char)(loc[0] | 32);
  tvLang[1] = (char)(loc[1] | 32);
  tvLang[2] = 0;
  return tvLang;
}
/* first run: the subtitle and audio languages follow the TV's language too (only while never chosen: a choice
   made in Settings is never overridden) */
static void first_run_defaults(void) {
  static int done;
  if (done)
    return;
  done = 1;
  if (!store_get("subsPtBrMigrated", NULL)) {
    if (!strcmp(lang, "pt-BR") && !strcmp(store_get("subsLang", ""), "por"))
      store_set("subsLang", "pob");
    store_set("subsPtBrMigrated", "1");
  }
  if (store_get("subsLang", NULL) && store_get("defaultAudioTrack", NULL))
    return;
  static const char *M[][2] = { { "en", "eng" }, { "ar", "ara" }, { "fr", "fre" }, { "es", "spa" }, { "de", "ger" },
                                { "it", "ita" }, { "pt", "por" }, { "ru", "rus" }, { "tr", "tur" }, { "pl", "pol" },
                                { "nl", "dut" }, { "sv", "swe" }, { "he", "heb" }, { "hi", "hin" }, { "ja", "jpn" },
                                { "ko", "kor" }, { "zh", "chi" } };
  const char *tv = i18n_tv_lang(), *c = NULL;
  for (size_t i = 0; i < sizeof M / sizeof M[0]; i++)
    if (!strncmp(M[i][0], tv, 2))
      c = M[i][1];
  const char *sub = !strcmp(lang, "pt-BR") || !strcmp(tv, "pt-BR") ? "pob" : c;
  if (sub && !store_get("subsLang", NULL))
    store_set("subsLang", sub);
  if (c && !store_get("defaultAudioTrack", NULL))
    store_set("defaultAudioTrack", c);
}
static unsigned langGen = 1;
unsigned i18n_gen(void) {
  return langGen;
}
static void merge_strings(const char *path) {
  char *text = read_all(path);
  if (!text)
    return;
  cJSON *part = cJSON_Parse(text);
  free(text);
  if (!cJSON_IsObject(part)) {
    fprintf(stderr, "[i18n] bad %s\n", path);
    cJSON_Delete(part);
    return;
  }
  const cJSON *it;
  cJSON_ArrayForEach(it, part)
    if (cJSON_IsString(it) && it->string) {
      cJSON_DeleteItemFromObjectCaseSensitive(dict, it->string);
      cJSON_AddStringToObject(dict, it->string, it->valuestring);
    }
  cJSON_Delete(part);
}
/* Load the base dictionary, then the feature dictionaries for the same locale. */
static void load_strings(const char *dir, const char *code) {
  char path[1400];
  snprintf(path, sizeof path, "%s/%s.json", dir, code);
  merge_strings(path);
  DIR *d = opendir(dir);
  struct dirent *e;
  size_t ll = strlen(code);
  while (d && (e = readdir(d))) {
    size_t n = strlen(e->d_name);
    if (n <= ll + 6 || strncmp(e->d_name, code, ll) || e->d_name[ll] != '.' || strcmp(e->d_name + n - 5, ".json"))
      continue;
    snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
    merge_strings(path);
  }
  if (d)
    closedir(d);
}
static int init_(const char *assetDir, const char *want) {
  langGen++;
  /* "auto" uses the platform language when supported, otherwise English. */
  if (want && brazilian_locale(want))
    snprintf(lang, sizeof lang, "%s", "pt-BR");
  else if (want && lang_name(want))
    snprintf(lang, sizeof lang, "%s", want);
  else {
    const char *tv = i18n_tv_lang();
    snprintf(lang, sizeof lang, "%s", lang_name(tv) ? tv : "en");
  }
  first_run_defaults();
  cJSON_Delete(dict);
  dict = NULL;
  if (strcmp(lang, "en")) {
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/i18n", assetDir);
    dict = cJSON_CreateObject();
    if (dict) {
      if (!strcmp(lang, "pt-BR"))
        load_strings(dir, "pt");
      load_strings(dir, lang);
    }
  }
  index_build();
  rtl = !strcmp(lang, "ar");
  return 1;
}
/* the recommendations worker translates row titles off the main thread: it holds this while it does, and a language
 * change waits the few ms */
static pthread_mutex_t swapMu = PTHREAD_MUTEX_INITIALIZER;
void i18n_lock(void) {
  pthread_mutex_lock(&swapMu);
}
void i18n_unlock(void) {
  pthread_mutex_unlock(&swapMu);
}
int i18n_init(const char *assetDir, const char *want) {
  i18n_lock();
  int r = init_(assetDir, want);
  i18n_unlock();
  return r;
}
const char *i18n_lang(void) {
  return lang;
}
int i18n_rtl(void) {
  return rtl;
}
const char *T(const char *en) {
  if (!idx || !en)
    return en;
  unsigned i = hash_str(en) & (idxCap - 1);
  const cJSON *v;
  while ((v = idx[i]) && strcmp(v->string, en))
    i = (i + 1) & (idxCap - 1);
  return cJSON_IsString(v) && v->valuestring && v->valuestring[0] ? v->valuestring : en;
}
const char *i18n_key_of(const char *tr) {
  if (!dict || !tr)
    return NULL;
  const cJSON *it;
  cJSON_ArrayForEach(it, dict)
    if (cJSON_IsString(it) && it->valuestring && !strcmp(it->valuestring, tr))
      return it->string;
  return NULL;
}
static char bufs[8][1024];
static int bi;
static void subst(char *out, size_t cap, const char *s, const char *k1, const char *v1, const char *k2,
                  const char *v2) {
  size_t o = 0;
  while (*s && o + 1 < cap) {
    if (*s == '{') {
      const char *e = strchr(s, '}');
      if (e) {
        size_t kl = e - s - 1;
        const char *v = NULL;
        if (k1 && strlen(k1) == kl && !strncmp(s + 1, k1, kl))
          v = v1;
        else if (k2 && strlen(k2) == kl && !strncmp(s + 1, k2, kl))
          v = v2;
        if (v) {
          size_t vl = strlen(v);
          if (vl > cap - 1 - o)
            vl = cap - 1 - o;
          memcpy(out + o, v, vl);
          o += vl;
          s = e + 1;
          continue;
        }
      }
    }
    out[o++] = *s++;
  }
  out[o] = 0;
}
const char *T1(const char *en, const char *k1, const char *v1) {
  char *b = bufs[bi++ & 7];
  subst(b, 1024, T(en), k1, v1 ? v1 : "", NULL, NULL);
  return b;
}
const char *T2(const char *en, const char *k1, const char *v1, const char *k2, const char *v2) {
  char *b = bufs[bi++ & 7];
  subst(b, 1024, T(en), k1, v1 ? v1 : "", k2, v2 ? v2 : "");
  return b;
}
const char *i18n_lang_name(const char *code) {
  const char *name = lang_name(code);
  return name ? name : code;
}
int i18n_lang_count(void) {
  return (int)(sizeof languages / sizeof languages[0]);
}
const char *i18n_lang_code(int i) {
  return i >= 0 && i < i18n_lang_count() ? languages[i].code : "en";
}
