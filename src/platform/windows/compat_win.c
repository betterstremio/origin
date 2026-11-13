/* The code behind platform/windows/compat/: the POSIX calls the shared code uses, on Win32. Paths arrive as UTF-8. */
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include <ifaddrs.h>
#include <dlfcn.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/statvfs.h>

static wchar_t *wide(const char *s) { /* malloc'd; NULL on failure */
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  if (n <= 0)
    return NULL;
  wchar_t *w = malloc((size_t)n * sizeof *w);
  if (w)
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  return w;
}
static void set_errno_from_win(void) {
  DWORD e = GetLastError();
  errno = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ENOENT
          : e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS  ? EEXIST
                                                                 : EACCES;
}

int sb_mkdir(const char *path) {
  wchar_t *w = wide(path);
  if (!w) {
    errno = EINVAL;
    return -1;
  }
  int r = _wmkdir(w);
  free(w);
  return r;
}
int sb_rename(const char *from, const char *to) {
  wchar_t *a = wide(from), *b = wide(to);
  int ok = a && b && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
  if (!ok)
    set_errno_from_win();
  free(a);
  free(b);
  return ok ? 0 : -1;
}
char *strcasestr(const char *h, const char *n) {
  size_t l = strlen(n);
  if (!l)
    return (char *)h;
  for (; *h; h++)
    if (!_strnicmp(h, n, l))
      return (char *)h;
  return NULL;
}

char *strndup(const char *s, size_t n) {
  size_t l = 0;
  while (l < n && s[l])
    l++;
  char *d = malloc(l + 1);
  if (d) {
    memcpy(d, s, l);
    d[l] = 0;
  }
  return d;
}
long sb_sysconf(int name) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return name == _SC_NPROCESSORS_ONLN ? (long)si.dwNumberOfProcessors : name == _SC_PAGESIZE ? (long)si.dwPageSize : -1;
}
int utimensat(int dirfd, const char *path, const struct timespec ts[2], int flags) {
  (void)dirfd;
  (void)flags;
  wchar_t *w = wide(path);
  if (!w) {
    errno = EINVAL;
    return -1;
  }
  HANDLE h = CreateFileW(w, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  free(w);
  if (h == INVALID_HANDLE_VALUE) {
    set_errno_from_win();
    return -1;
  }
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  int ok = SetFileTime(h, NULL, ts && ts[0].tv_nsec == UTIME_OMIT ? NULL : &now,
                       ts && ts[1].tv_nsec != UTIME_OMIT ? &now : NULL);
  CloseHandle(h);
  return ok ? 0 : -1;
}

/* ---- sockets ---- */
static int is_sock(int fd) {
  int t = 0, l = sizeof t;
  return fd >= 0 && getsockopt((SOCKET)(intptr_t)fd, SOL_SOCKET, SO_TYPE, (char *)&t, &l) == 0;
}
int sb_close(int fd) {
  if (is_sock(fd))
    return closesocket((SOCKET)(intptr_t)fd) ? -1 : 0;
  return _close(fd);
}
int sb_setsockopt(int fd, int level, int name, const void *val, int len) {
  if (level == SOL_SOCKET && name == SO_REUSEADDR)
    return 0;
  if (level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO) && len == (int)sizeof(struct timeval)) {
    const struct timeval *tv = val;
    DWORD ms = (DWORD)(tv->tv_sec * 1000 + tv->tv_usec / 1000);
    return setsockopt((SOCKET)(intptr_t)fd, level, name, (const char *)&ms, sizeof ms);
  }
  return setsockopt((SOCKET)(intptr_t)fd, level, name, (const char *)val, len);
}
int sb_poll(struct pollfd *f, unsigned long n, int timeoutMs) {
  int socks = 1;
  for (unsigned long i = 0; i < n; i++)
    if (!is_sock((int)f[i].fd))
      socks = 0;
  if (socks)
    return WSAPoll(f, n, timeoutMs);
  int r = 0;
  for (unsigned long i = 0; i < n; i++) {
    f[i].revents = f[i].events & POLLIN;
    if (f[i].revents)
      r++;
  } /* a pipe: read() waits */
  return r;
}
int fcntl(int fd, int cmd, ...) {
  if (cmd != F_SETFL)
    return 0;
  va_list ap;
  va_start(ap, cmd);
  long a = va_arg(ap, long);
  va_end(ap);
  if (is_sock(fd)) {
    u_long nb = (a & O_NONBLOCK) != 0;
    return ioctlsocket((SOCKET)(intptr_t)fd, FIONBIO, &nb) ? -1 : 0;
  }
  return 0;
}
/* IPv4 addresses that are up; adapters with a gateway first (the real network before WSL's, Hyper-V's or a VPN's) */
int getifaddrs(struct ifaddrs **out) {
  *out = NULL;
  ULONG sz = 16384;
  IP_ADAPTER_ADDRESSES *aa = NULL;
  ULONG r = ERROR_BUFFER_OVERFLOW;
  for (int k = 0; k < 3 && r == ERROR_BUFFER_OVERFLOW; k++) {
    free(aa);
    aa = malloc(sz);
    r = GetAdaptersAddresses(
        AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS,
        NULL, aa, &sz);
  }
  if (r != NO_ERROR) {
    free(aa);
    errno = EIO;
    return -1;
  }
  struct ifaddrs *head = NULL, **tail = &head;
  for (int pass = 0; pass < 2; pass++)
    for (IP_ADAPTER_ADDRESSES *a = aa; a; a = a->Next) {
      if (a->OperStatus != IfOperStatusUp || (pass == 0) != (a->FirstGatewayAddress != NULL))
        continue;
      for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
        if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET)
          continue;
        size_t nl = strlen(a->AdapterName) + 1;
        struct ifaddrs *e = calloc(1, sizeof *e + 2 * sizeof(struct sockaddr_in) + nl);
        if (!e)
          continue;
        struct sockaddr_in *ad = (struct sockaddr_in *)(e + 1), *mk = ad + 1;
        char *nm = (char *)(mk + 1);
        memcpy(ad, u->Address.lpSockaddr, sizeof *ad);
        mk->sin_family = AF_INET;
        ConvertLengthToIpv4Mask(u->OnLinkPrefixLength, (PULONG)&mk->sin_addr.s_addr);
        memcpy(nm, a->AdapterName, nl);
        e->ifa_name = nm;
        e->ifa_addr = (struct sockaddr *)ad;
        e->ifa_netmask = (struct sockaddr *)mk;
        e->ifa_flags = IFF_UP | (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK ? IFF_LOOPBACK : 0);
        *tail = e;
        tail = &e->ifa_next;
      }
    }
  free(aa);
  *out = head;
  return 0;
}
void freeifaddrs(struct ifaddrs *i) {
  while (i) {
    struct ifaddrs *n = i->ifa_next;
    free(i);
    i = n;
  }
}

/* ---- libraries ---- */
static char dlErr[300];
void *dlopen(const char *path, int flags) {
  wchar_t *w = wide(path);
  if (!w) {
    snprintf(dlErr, sizeof dlErr, "bad path");
    return NULL;
  }
  HMODULE h = (flags & RTLD_NOLOAD) ? GetModuleHandleW(w) : LoadLibraryExW(w, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!h)
    snprintf(dlErr, sizeof dlErr, "%s: error %lu", path, (unsigned long)GetLastError());
  free(w);
  return h;
}
void *dlsym(void *lib, const char *name) {
  void *p = (void *)GetProcAddress(lib ? (HMODULE)lib : GetModuleHandleW(NULL), name);
  if (!p)
    snprintf(dlErr, sizeof dlErr, "%s: not found", name);
  return p;
}
int dlclose(void *lib) {
  return FreeLibrary((HMODULE)lib) ? 0 : -1;
}
const char *dlerror(void) {
  return dlErr[0] ? dlErr : NULL;
}

/* ---- files ---- */
int statvfs(const char *path, struct statvfs *o) {
  wchar_t *w = wide(path);
  ULARGE_INTEGER av, tot, fr;
  int ok = w && GetDiskFreeSpaceExW(w, &av, &tot, &fr);
  free(w);
  if (!ok) {
    set_errno_from_win();
    return -1;
  }
  o->f_bsize = o->f_frsize = 4096;
  o->f_blocks = tot.QuadPart / 4096;
  o->f_bfree = fr.QuadPart / 4096;
  o->f_bavail = av.QuadPart / 4096;
  return 0;
}
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
  (void)addr;
  (void)prot;
  (void)flags;
  HANDLE f = (HANDLE)_get_osfhandle(fd);
  if (f == INVALID_HANDLE_VALUE)
    return MAP_FAILED;
  HANDLE m = CreateFileMappingW(f, NULL, PAGE_READONLY, 0, 0, NULL);
  if (!m)
    return MAP_FAILED;
  void *v = MapViewOfFile(m, FILE_MAP_READ, (DWORD)((uint64_t)off >> 32), (DWORD)(uint64_t)off, len);
  CloseHandle(m); /* (the view keeps the mapping) */
  return v ? v : MAP_FAILED;
}
int munmap(void *addr, size_t len) {
  (void)len;
  return UnmapViewOfFile(addr) ? 0 : -1;
}

/* ---- signals ---- */
static struct sigaction acts[32];
void win_crash_hook(int sig); /* (plat_win.c: the crash report, from where the fault happened) */
static void trampoline(int sig) {
  if (sig == SIGSEGV || sig == SIGILL || sig == SIGFPE || sig == SIGABRT)
    win_crash_hook(sig);
  struct sigaction a = acts[sig & 31];
  if ((a.sa_flags & SA_SIGINFO) && a.sa_sigaction)
    a.sa_sigaction(sig, NULL, NULL);
  else if (a.sa_handler && a.sa_handler != SIG_IGN && a.sa_handler != SIG_DFL)
    a.sa_handler(sig);
}
int sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
  if (sig <= 0 || sig >= 32) {
    errno = EINVAL;
    return -1;
  }
  if (old)
    *old = acts[sig];
  if (act) {
    acts[sig] = *act;
    int ours = (act->sa_flags & SA_SIGINFO) ? act->sa_sigaction != NULL
                                            : act->sa_handler != SIG_DFL && act->sa_handler != SIG_IGN;
    if (signal(sig, ours ? trampoline : act->sa_handler) == SIG_ERR)
      return 0;
  } /* (SIGBUS, SIGALRM: no such signal here) */
  return 0;
}
