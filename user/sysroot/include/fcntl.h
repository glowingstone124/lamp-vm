#ifndef LAMP_LIBC_FCNTL_H
#define LAMP_LIBC_FCNTL_H

#include <lamp/abi.h>
#include <sys/types.h>

#define O_RDONLY LAMP_O_RDONLY
#define O_WRONLY LAMP_O_WRONLY
#define O_RDWR LAMP_O_RDWR
#define O_CREAT LAMP_O_CREAT
#define O_TRUNC LAMP_O_TRUNC
#define O_NONBLOCK LAMP_O_NONBLOCK
#define O_APPEND 0x00000400
#define O_EXCL 0x00000080
#define O_NOCTTY 0x00000100
#define O_CLOEXEC LAMP_O_CLOEXEC

#define F_GETFD LAMP_FCNTL_F_GETFD
#define F_SETFD LAMP_FCNTL_F_SETFD
#define F_GETFL LAMP_FCNTL_F_GETFL
#define F_SETFL LAMP_FCNTL_F_SETFL
#define F_DUPFD LAMP_FCNTL_F_DUPFD
#define F_DUPFD_CLOEXEC LAMP_FCNTL_F_DUPFD_CLOEXEC
#define FD_CLOEXEC LAMP_FD_CLOEXEC

int open(const char *path, int flags, ...);
int fcntl(int fd, int cmd, ...);

#endif
