#ifndef LAMP_LIBC_SCHED_H
#define LAMP_LIBC_SCHED_H

#include <stddef.h>
#include <sys/types.h>

int sched_getaffinity(pid_t pid, size_t cpusetsize, void *mask);

#endif
