/* The Anime tab's taste: the anime in the library (what was watched, how far, how long ago) turned into AniList ids and
   from those into genres, tags and a studio (anilist_resolve + anilist_media_info, cached, paced: nothing here blocks
   or asks more than the AniList module allows). The weights are small tables built once per change of the library or of
   an answer, never per frame. Everything is empty (and nothing is asked) until anime_taste_begin() is called. */
#ifndef SB_ANIME_TASTE_H
#define SB_ANIME_TASTE_H
#include "core/stremio.h"

void anime_taste_begin(void); /* start or continue (cheap when the library has not changed since the last call) */
void anime_taste_close(void); /* ignore pending results when leaving the Anime tab disabled */
int anime_taste_seeds(void); /* anime seeds whose genres are known */
/* the genres the user leans to, heaviest first; returns how many */
int anime_taste_genres(char g[][24], int max);
/* the most recently watched anime with an AniList id, newest first: its id and library name; returns how many */
int anime_taste_recent(int *ids, char names[][96], int max);
const char *anime_taste_studio(void); /* the studio of the most watched anime, "" when none */
int anime_taste_has(int alid); /* the library has it (watched or saved): not a pick */
/* a candidate's fit: genre overlap with the taste plus its popularity and score (higher is better; 0 for no fit) */
int anime_taste_score(int alid);
#endif
