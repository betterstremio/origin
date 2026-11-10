/* fcntl(): close-on-exec means nothing here (the app starts no programs), and non-blocking applies to sockets only. */
#ifndef SB_COMPAT_FCNTL_H
#define SB_COMPAT_FCNTL_H
#include_next <fcntl.h>
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define FD_CLOEXEC 1
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x40000000
#endif
int fcntl(int fd, int cmd, ...);
#endif
