/* AniList id -> IMDb id, for the Anime tab: an anime card that has an IMDb id is drawn, previewed and opened as a Shows
   card (metahub art and logo, the IMDb trailer clip, one title page with every season). Pairs come from ARM
   (arm.haglund.dev, Fribb's anime-lists), asked in bulk off the drawing path, and are kept on the disk for good
   (almap.txt in the data folder; a miss for 7 days). Main thread only; nothing here blocks. */
#ifndef SB_ANIME_MAP_H
#define SB_ANIME_MAP_H

/* "tt..." when known to have one, else NULL (unknown, or known to have none). The first call starts reading the file
   on a worker; until it is in, every id is unknown (and wants wait for it). */
const char *anime_map_imdb(int alid);
int anime_map_known(int alid); /* 1: answered (found, or a miss younger than 7 days) */
int anime_map_kitsu(int alid); /* the Kitsu id ARM gave, 0 when none */
int anime_map_season(int alid); /* the TVDB season of this AniList entry, 0 when unknown */
/* the AniList entry of an IMDb id: the one with the lowest season (season 1 first), 0 when none is known */
int anime_map_alid(const char *imdb);
int anime_map_ready(void); /* 1 once the file has been read (starts reading it) */
/* every AniList entry known to map to this IMDb id (all seasons of a show), with its Kitsu id (0 when none): fills up
   to max of each, returns how many */
int anime_map_aliases(const char *imdb, int *alids, int *kitsus, int max);
/* ask for the ids not known yet (in flight ones are not asked twice); answers come on later frames */
void anime_map_want(const int *alids, int n);
unsigned anime_map_version(void); /* bumps when an answer has added pairs */
void anime_map_tick(void); /* once a frame: sends what is waiting (coalesced, at most 100 a request) */
#endif
