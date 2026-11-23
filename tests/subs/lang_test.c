#include "core/subs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  static const char *cases[][2] = {
    {"pob","pob"}, {"pt-BR","pob"}, {"pt_BR","pob"}, {"pt-br","pob"}, {"ptbr","pob"},
    {"Portuguese (Brazil)","pob"}, {"Portuguese (BR)","pob"}, {"Português (Brasil)","pob"},
    {"Brazilian","pob"}, {"Brazilian Portuguese","pob"}, {"por","por"}, {"pt","por"},
    {"pt-PT","por"}, {"Portuguese","por"}, {"Português","por"}, {"English","eng"},
    {"SPANISH","spa"}, {"French","fre"}, {"fra","fre"}, {"en-US","eng"},
    {"Make English subtitles","Make English subtitles"}, {"Unlisted_Name","Unlisted_Name"}
  };
  char buf[64];
  for (size_t i = 0; i < sizeof cases/sizeof cases[0]; i++) {
    assert(!strcmp(subs_lang_canon(cases[i][0],buf,sizeof buf),cases[i][1]));
    assert(subs_lang_match(cases[i][0],cases[i][1]));
  }
  assert(!subs_lang_match("pt-BR","por"));
  assert(!subs_lang_match("pt-PT","pob"));
  assert(subs_lang_match("pt-PT",subs_lang_fallback("pob")));
  assert(subs_lang_match("pt_BR",subs_lang_fallback("por")));
  assert(!subs_lang_fallback("eng"));
  assert(!subs_lang_match("Make English subtitles","eng"));
  assert(!subs_lang_match("Translate to Portuguese","pob"));
  assert(!subs_lang_match(NULL,"eng"));
  assert(!subs_lang_match("",""));
  assert(!strcmp(subs_sub_name("pt-BR"),"Portuguese (BR)"));
  assert(!strcmp(subs_lang_name("pt-PT"),"Portuguese"));
  assert(!strcmp(subs_sub_name("spl"),"Spanish (LA)"));
  puts("Subtitle language tests passed");
  return 0;
}
