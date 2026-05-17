#ifndef LAMP_LIBC_SYS_SELECT_H
#define LAMP_LIBC_SYS_SELECT_H

#include <sys/types.h>
#include <sys/time.h>

typedef struct {
    unsigned int bits;
} fd_set;

#define FD_ZERO(set) ((set)->bits = 0u)
#define FD_SET(fd, set) ((set)->bits |= (1u << (fd)))
#define FD_CLR(fd, set) ((set)->bits &= ~(1u << (fd)))
#define FD_ISSET(fd, set) (((set)->bits & (1u << (fd))) != 0)

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);

#endif
