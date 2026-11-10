/* Read-only file mappings (fonts), on CreateFileMapping / MapViewOfFile. */
#ifndef SB_COMPAT_MMAN_H
#define SB_COMPAT_MMAN_H
#include <stddef.h>
#include <sys/types.h>
#define PROT_READ 0x1
#define MAP_PRIVATE 0x2
#define MAP_SHARED 0x1
#define MAP_FAILED ((void *)-1)
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int munmap(void *addr, size_t len);
#endif
