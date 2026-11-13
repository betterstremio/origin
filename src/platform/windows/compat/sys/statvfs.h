/* Free and total space of the drive a path is on (GetDiskFreeSpaceEx). */
#ifndef SB_COMPAT_STATVFS_H
#define SB_COMPAT_STATVFS_H
struct statvfs {
  unsigned long long f_bsize, f_frsize, f_blocks, f_bfree, f_bavail;
};
int statvfs(const char *path, struct statvfs *out);
#endif
