#ifndef LAMP_LIBC_SYS_STATFS_H
#define LAMP_LIBC_SYS_STATFS_H

struct statfs { unsigned long f_type; unsigned long f_bsize; unsigned long f_blocks; unsigned long f_bfree; };
int statfs(const char *path, struct statfs *buf);

#endif
