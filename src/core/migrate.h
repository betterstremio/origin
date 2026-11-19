/* The first run of 3.0.0: the app id changed (tv.stremioorigin.app -> io.github.betterstremio.origin), so to the OS
   this is another app with an empty data folder. migrate_run() imports the old app's settings and sign-ins once, then
   asks for the old app to be removed. A normal start costs one stat. */
#ifndef SB_MIGRATE_H
#define SB_MIGRATE_H
/* before store_init(newDataDir). SB_MIGRATE_FROM=<folder>: import from that folder (a test: no "is it ours" check, the
   old app is never removed) */
void migrate_run(const char *newDataDir);
#endif
