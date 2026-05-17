#ifndef LAMP_LIBC_TIME_H
#define LAMP_LIBC_TIME_H

#include <sys/types.h>

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1

int clock_gettime(int clock_id, struct timespec *ts);
int clock_getres(int clock_id, struct timespec *ts);
int nanosleep(const struct timespec *req, struct timespec *rem);
time_t time(time_t *tloc);
struct tm *localtime_r(const time_t *timep, struct tm *result);
struct tm *localtime(const time_t *timep);
char *ctime(const time_t *timep);
size_t strftime(char *s, size_t max, const char *format, const struct tm *tm);
time_t mktime(struct tm *tm);
void tzset(void);

#endif
