/* The POSIX calls the shared code uses that mingw-w64 lacks or does differently. tools/targets/windows.sh puts this
   folder first on the include path and includes this file first in every app file. The headers beside it (poll.h,
   sys/socket.h, dlfcn.h...) stand in for the POSIX ones; the code behind them is platform/windows/compat_win.c.
   No windows.h here: it defines RGB, near, far, small, min, max..., which the app's own names collide with. */
#ifndef SB_COMPAT_H
#define SB_COMPAT_H
#ifndef _POSIX_THREAD_SAFE_FUNCTIONS
#define _POSIX_THREAD_SAFE_FUNCTIONS 200112L   /* localtime_r, gmtime_r */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <direct.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <pthread.h>

/* mkdir takes no mode */
int sb_mkdir(const char *path);
#define mkdir(p, m) sb_mkdir(p)
/* rename refuses an existing target, and every settings and cache file is saved as "write a new one, rename it over the
   old": without this, nothing would be saved after the first time */
int sb_rename(const char *from, const char *to);
#define rename(a, b) sb_rename(a, b)
/* a pipe with room for bursts, never translating line ends */
#define pipe(p) _pipe((p), 1 << 20, _O_BINARY)
/* no symbolic links to tell apart, no users */
#define lstat stat
#ifndef S_ISLNK
#define S_ISLNK(m) 0
#endif
static inline unsigned sb_uid(void) { return 0; }
#define geteuid sb_uid
#define getuid sb_uid
char *strcasestr(const char *hay, const char *needle);
char *strndup(const char *s, size_t n);   /* (libass calls it too) */
#define timegm _mkgmtime
#define _SC_PAGESIZE 30
#define _SC_NPROCESSORS_ONLN 84
long sb_sysconf(int name);
#define sysconf sb_sysconf
/* utimensat as the cache uses it: the access time set to now, the modification time kept (the cache's least recently
   used files go first; Windows often leaves access times alone by itself) */
#define AT_FDCWD -100
#define UTIME_NOW ((1l << 30) - 1l)
#define UTIME_OMIT ((1l << 30) - 2l)
int utimensat(int dirfd, const char *path, const struct timespec ts[2], int flags);
#endif
