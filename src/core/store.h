/* Persistent state: settings, the sign-in, the library cache and small caches, kept as one JSON
   document in the data folder and written back a moment after a change (never on every key). */
#ifndef SB_STORE_H
#define SB_STORE_H
#include "cJSON.h"

void store_init(const char *dataDir);
/* string / number values under the top-level object (settings and the like) */
const char *store_get(const char *key, const char *def);
void store_set(const char *key, const char *value);
/* whole JSON branches (library, addons, user): the returned node belongs to the store */
cJSON *store_node(const char *key);
void store_set_node(const char *key, cJSON *node); /* takes ownership */
void store_remove(const char *key);
unsigned store_serial(void); /* bumps on every change: a value read once stays good while it holds */
void store_flush(void); /* writes now if dirty */
void store_pump(void); /* writes a little after the last change; call per frame */

/* file caches: bytes under a name (hashed from the key), with an age in seconds */
unsigned char *cache_read(const char *key, long maxAgeSec, size_t *len);
int cache_file(const char *key, long maxAgeSec, char *path,
               size_t cap); /* 1: a copy exists (and is young enough); its path */
/* for the drawing thread: is there a copy (young enough)? From an index in memory: never touches the disk, never waits
   on I/O. 0 while the index is not ready (the scan at start, in cache_prune_start's thread) or when the key is unknown
 */
int cache_known(const char *key, long maxAgeSec);
void cache_prune_start(int capMb); /* the cache folder kept under capMb, least recently used first (a thread at start;
                                      while running the writer thread prunes again when it grows past the cap or the
                                      free space falls under the floor (SB_CACHE_FLOOR_MB)) */
int cache_room_start(
    long needBytes); /* 1: there is room now; 0: the cache is being emptied (a thread), poll cache_room_done */
int cache_room_done(void);
long cache_cap_bytes(void); /* the cap given to cache_prune_start (0 before) */
long cache_kept_bytes(
    void); /* the cache folder's size: after the prune at start, then kept up as files are written and pruned (-1 until
                the first prune ran); kept under the cap and above the free-space floor while running */
void cache_write(const char *key, const unsigned char *data, size_t len);
const char *store_data_dir(void);
/* one account's own keys set aside and another's put back, in the data folder's acct/ (from NULL: nothing set aside).
   The old data is written by a thread; the new one is read here (a deliberate switch, a few ms) */
void store_scope_swap(const char *from, const char *to, const char *const *keys, int n);
void store_scope_forget(const char *prefix); /* removes every acct/ file starting with prefix */
#endif
