/* poll(): WSAPoll for sockets. Anything else (applog's pipe) counts as ready, so its reader waits in read() instead,
   which ends when the pipe's last writer closes. No close() macro here: only sys/socket.h has that. */
#ifndef SB_COMPAT_POLL_H
#define SB_COMPAT_POLL_H
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
#define NOGDI
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#undef near
#undef far
int sb_poll(struct pollfd *fds, unsigned long n, int timeoutMs);
#define poll(f, n, t) sb_poll(f, n, t)
#endif
