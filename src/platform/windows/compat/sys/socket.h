/* Sockets on Winsock. Only the files that include this header see the macros below (httpd.c, hproxy.c): there,
   close() is for sockets, and setsockopt() takes POSIX meanings. */
#ifndef SB_COMPAT_SYS_SOCKET_H
#define SB_COMPAT_SYS_SOCKET_H
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
#define NOGDI /* (wingdi.h's RGB would replace the app's) */
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <unistd.h>
#undef near
#undef far
#ifndef SHUT_RD
#define SHUT_RD SD_RECEIVE
#define SHUT_WR SD_SEND
#define SHUT_RDWR SD_BOTH
#endif
/* a socket goes to closesocket(), anything else to the C library */
int sb_close(int fd);
#define close(fd) sb_close(fd)
/* SO_RCVTIMEO / SO_SNDTIMEO given as a struct timeval become Windows' milliseconds (a timeval read as milliseconds
   would be 5 ms instead of 5 s); SO_REUSEADDR is left out (on Windows it lets another program take the port) */
int sb_setsockopt(int fd, int level, int name, const void *val, int len);
#define setsockopt(s, l, n, v, len) sb_setsockopt((int)(s), l, n, v, (int)(len))
#endif
