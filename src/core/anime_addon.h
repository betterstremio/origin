/* The Anime Kitsu addon, kept on THIS device while the Anime tab is on. The Stremio account is never written to: the
   addon is added to the in-memory list sb_addons() hands out (after the account's own, and not at all when the account
   already has it), and taken out again when the tab goes off. Its manifest is read from the disk (7 days) or fetched
   once; with the tab off nothing here asks for anything or holds anything. Meta for "anilist:" and "kitsu:" ids is then
   served by it through the normal addon routing (addons.c prefix_ok). */
#ifndef SB_ANIME_ADDON_H
#define SB_ANIME_ADDON_H
#include "cJSON.h"

#define ANIME_KITSU_URL "https://anime-kitsu.strem.fun/manifest.json"
void anime_addon_set(int on); /* the tab's switch (and the stored value at start) */
int anime_addon_ready(void); /* the manifest is in and the addon is in the list (or the account has it) */
/* sb_addons() through this: the account's list as it is, or a copy with the Kitsu addon after it. raw may be NULL */
cJSON *anime_addons_merge(cJSON *raw);
#endif
