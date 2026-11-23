#include "core/subs.h"
#include <ctype.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>

/* ---- language codes ---- */
static const char *ALIAS[][2] = {
  { "en", "eng" },  { "ar", "ara" },  { "fr", "fre" }, { "fra", "fre" }, { "es", "spa" },  { "spl", "spa" },
  { "de", "ger" },  { "deu", "ger" }, { "it", "ita" }, { "pt", "por" },   { "ru", "rus" },
  { "tr", "tur" },  { "pl", "pol" },  { "nl", "dut" }, { "nld", "dut" }, { "sv", "swe" },  { "he", "heb" },
  { "iw", "heb" },  { "hi", "hin" },  { "ja", "jpn" }, { "ko", "kor" },  { "zh", "chi" },  { "zho", "chi" },
  { "ro", "rum" },  { "ron", "rum" }, { "el", "gre" }, { "ell", "gre" }, { "cs", "cze" },  { "ces", "cze" },
  { "fa", "per" },  { "fas", "per" }, { "da", "dan" }, { "fi", "fin" },  { "no", "nor" },  { "nb", "nor" },
  { "nob", "nor" }, { "hu", "hun" },  { "bg", "bul" }, { "uk", "ukr" },  { "sr", "srp" },  { "hr", "hrv" },
  { "sk", "slo" },  { "slk", "slo" }, { "sl", "slv" }, { "id", "ind" },  { "ms", "may" },  { "msa", "may" },
  { "th", "tha" },  { "vi", "vie" },  { NULL, NULL }
};
/* a track's language as a name (the player's chips and lists) */
static const char *LANGS[][2] = {
  { "en", "English" },     { "eng", "English" },    { "fr", "French" },      { "fre", "French" },
  { "fra", "French" },     { "es", "Spanish" },     { "spa", "Spanish" },    { "de", "German" },
  { "ger", "German" },     { "deu", "German" },     { "it", "Italian" },     { "ita", "Italian" },
  { "pt", "Portuguese" },  { "por", "Portuguese" }, { "pob", "Portuguese (BR)" }, { "ru", "Russian" },     { "rus", "Russian" },
  { "ar", "Arabic" },      { "ara", "Arabic" },     { "tr", "Turkish" },     { "tur", "Turkish" },
  { "pl", "Polish" },      { "pol", "Polish" },     { "nl", "Dutch" },       { "dut", "Dutch" },
  { "nld", "Dutch" },      { "sv", "Swedish" },     { "swe", "Swedish" },    { "ja", "Japanese" },
  { "jpn", "Japanese" },   { "ko", "Korean" },      { "kor", "Korean" },     { "zh", "Chinese" },
  { "chi", "Chinese" },    { "zho", "Chinese" },    { "hi", "Hindi" },       { "hin", "Hindi" },
  { "he", "Hebrew" },      { "heb", "Hebrew" },     { "el", "Greek" },       { "gre", "Greek" },
  { "ell", "Greek" },      { "ro", "Romanian" },    { "rum", "Romanian" },   { "ron", "Romanian" },
  { "hu", "Hungarian" },   { "hun", "Hungarian" },  { "cs", "Czech" },       { "cze", "Czech" },
  { "ces", "Czech" },      { "da", "Danish" },      { "dan", "Danish" },     { "fi", "Finnish" },
  { "fin", "Finnish" },    { "no", "Norwegian" },   { "nor", "Norwegian" },  { "nb", "Norwegian" },
  { "nob", "Norwegian" },  { "uk", "Ukrainian" },   { "ukr", "Ukrainian" },  { "th", "Thai" },
  { "tha", "Thai" },       { "vi", "Vietnamese" },  { "vie", "Vietnamese" }, { "id", "Indonesian" },
  { "ind", "Indonesian" }, { "fa", "Persian" },     { "per", "Persian" },    { "fas", "Persian" },
  { NULL, NULL }
};
/* OpenSubtitles' own codes, ahead of the list above */
static const char *SUBNAMES[][2] = { { "spl", "Spanish (LA)" },
                                     { "pob", "Portuguese (BR)" },
                                     { "hrv", "Croatian" },
                                     { "bul", "Bulgarian" },
                                     { "bos", "Bosnian" },
                                     { "srp", "Serbian" },
                                     { "slo", "Slovak" },
                                     { "slk", "Slovak" },
                                     { "slv", "Slovenian" },
                                     { "est", "Estonian" },
                                     { "lav", "Latvian" },
                                     { "lit", "Lithuanian" },
                                     { "alb", "Albanian" },
                                     { "sqi", "Albanian" },
                                     { "mac", "Macedonian" },
                                     { "mkd", "Macedonian" },
                                     { "ice", "Icelandic" },
                                     { "isl", "Icelandic" },
                                     { "may", "Malay" },
                                     { "msa", "Malay" },
                                     { "cat", "Catalan" },
                                     { "baq", "Basque" },
                                     { "eus", "Basque" },
                                     { "glg", "Galician" },
                                     { "geo", "Georgian" },
                                     { "kat", "Georgian" },
                                     { "arm", "Armenian" },
                                     { "hye", "Armenian" },
                                     { "ben", "Bengali" },
                                     { "tam", "Tamil" },
                                     { "tel", "Telugu" },
                                     { "urd", "Urdu" },
                                     { "mal", "Malayalam" },
                                     { "sin", "Sinhala" },
                                     { "tgl", "Tagalog" },
                                     { "kur", "Kurdish" },
                                     { "mon", "Mongolian" },
                                     { "ze", "Chinese (bilingual)" },
                                     { "zht", "Chinese (traditional)" },
                                     { "zhe", "Chinese (bilingual)" },
                                     { "ell", "Greek" },
                                     { "ron", "Romanian" },
                                     { "ces", "Czech" },
                                     { "nld", "Dutch" },
                                     { NULL, NULL } };
static const char *lookup(const char *t[][2], const char *k) {
  for (int i = 0; t[i][0]; i++)
    if (!strcmp(t[i][0], k))
      return t[i][1];
  return NULL;
}
static const char *REGIONS[][2] = {
  { "ptbr", "pob" }, { "Portuguese (Brazil)", "pob" },
  { "Portuguese (BR)", "pob" }, { "Português (Brasil)", "pob" }, { "Brazilian", "pob" },
  { "Brazilian Portuguese", "pob" }, { "Português", "por" }, { NULL, NULL }
};
static const char *canonical(const char *code, char key[4]) {
  if (!code || !*code)
    return "";
  size_t len = strcspn(code, "-_");
  if (len == 2 || len == 3) {
    /* Only strip a subtag from a language tag. Add-ons also send free-form action labels. */
    if (code[len] && (strspn(code, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != strlen(code) ||
                      !code[len+1] || strcspn(code+len+1, "-_") > 8))
      return code;
    for (size_t i = 0; i < len; i++)
      key[i] = (char)tolower((unsigned char)code[i]);
    key[len] = 0;
    if (!strcmp(key, "pt") && code[len] && !strncasecmp(code + len + 1, "BR", 2) &&
        (!code[len+3] || code[len+3]=='-' || code[len+3]=='_'))
      return "pob";
    const char *alias = lookup(ALIAS, key);
    if (alias)
      return alias;
    return lookup(LANGS,key) || lookup(SUBNAMES,key) ? key : code;
  }
  for (int i=0; REGIONS[i][0]; i++)
    if (!strcasecmp(code, REGIONS[i][0]))
      return REGIONS[i][1];
  for (int i=0; LANGS[i][0]; i++)
    if (!strcasecmp(code, LANGS[i][1])) {
      const char *alias = lookup(ALIAS, LANGS[i][0]);
      return alias ? alias : LANGS[i][0];
    }
  return code;
}
const char *subs_lang_canon(const char *code, char *out, size_t cap) {
  char key[4];
  snprintf(out,cap,"%s",canonical(code,key));
  return out;
}
int subs_lang_match(const char *a, const char *b) {
  if (!a || !b || !*a || !*b)
    return 0;
  if (!strcasecmp(a,b))
    return 1;
  char x[4], y[4];
  return !strcmp(canonical(a,x),canonical(b,y));
}
/* The second selection pass only. Grouping and per-language caps always use the exact key. */
const char *subs_lang_fallback(const char *wanted) {
  return !strcmp(wanted,"pob") ? "por" : !strcmp(wanted,"por") ? "pob" : NULL;
}
const char *subs_lang_name(const char *code) {
  char key[4];
  return lookup(LANGS, canonical(code,key));
}
const char *subs_sub_name(const char *code) {
  const char *name = code ? lookup(SUBNAMES,code) : NULL;
  if (name)
    return name;
  char key[4];
  const char *lang = canonical(code,key);
  name = lookup(SUBNAMES,lang);
  return name ? name : lookup(LANGS,lang);
}
