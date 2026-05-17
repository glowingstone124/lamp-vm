#ifndef LAMP_LIBC_SYS_TIME_H
#define LAMP_LIBC_SYS_TIME_H

#include <sys/types.h>

struct timeval {
    time_t tv_sec;
    int tv_usec;
};

int gettimeofday(struct timeval *tv, void *tz);
int settimeofday(const struct timeval *tv, const void *tz);
int utimes(const char *path, const struct timeval times[2]);

#endif
