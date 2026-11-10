/* sigaction on top of the C library's signal(): the C library raises SIGSEGV, SIGILL, SIGFPE and SIGABRT for its own
   threads; the handler gets no siginfo or context (NULL). */
#ifndef SB_COMPAT_SIGNAL_H
#define SB_COMPAT_SIGNAL_H
#include_next <signal.h>
#include <sys/types.h>
#ifndef _POSIX
typedef _sigset_t sigset_t;
#endif
#ifndef SIGBUS
#define SIGBUS 10
#endif
#ifndef SIGALRM
#define SIGALRM 14
#endif
#ifndef SIGPROF
#define SIGPROF 27
#endif
#define SA_SIGINFO 0x4
#define SA_RESTART 0x10000000
typedef struct { int si_signo; void *si_addr; } siginfo_t;
struct sigaction {
  void (*sa_handler)(int);
  void (*sa_sigaction)(int, siginfo_t *, void *);
  sigset_t sa_mask;
  int sa_flags;
};
int sigaction(int sig, const struct sigaction *act, struct sigaction *old);
static inline int sigemptyset(sigset_t *s) { *s = 0; return 0; }
#endif
