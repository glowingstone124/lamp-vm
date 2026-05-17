#ifndef LAMP_LIBC_SYS_RESOURCE_H
#define LAMP_LIBC_SYS_RESOURCE_H

typedef unsigned long rlim_t;
struct rlimit { rlim_t rlim_cur; rlim_t rlim_max; };
#define RLIMIT_CPU    0
#define RLIMIT_FSIZE  1
#define RLIMIT_DATA   2
#define RLIMIT_STACK  3
#define RLIMIT_CORE   4
#define RLIMIT_RSS    5
#define RLIMIT_NPROC  6
#define RLIMIT_NOFILE 7
#define RLIMIT_MEMLOCK 8
#define RLIMIT_AS     9
#define RLIMIT_LOCKS  10
#define RLIMIT_MSGQUEUE 12
#define RLIMIT_RTPRIO 14
#define RLIMIT_RTTIME 15
#define RLIM_INFINITY ((rlim_t)-1)
int getrlimit(int resource, struct rlimit *rlim);
int setrlimit(int resource, const struct rlimit *rlim);

#endif
