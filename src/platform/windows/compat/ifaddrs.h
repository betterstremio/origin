/* The machine's addresses, from GetAdaptersAddresses (compat_win.c): IPv4 only, which is all the app asks for. */
#ifndef SB_COMPAT_IFADDRS_H
#define SB_COMPAT_IFADDRS_H
#include <net/if.h>
struct ifaddrs {
  struct ifaddrs *ifa_next;
  char *ifa_name;
  unsigned ifa_flags;
  struct sockaddr *ifa_addr, *ifa_netmask;
};
int getifaddrs(struct ifaddrs **out);
void freeifaddrs(struct ifaddrs *ifs);
#endif
