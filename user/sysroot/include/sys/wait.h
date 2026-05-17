#ifndef LAMP_LIBC_SYS_WAIT_H
#define LAMP_LIBC_SYS_WAIT_H

#include <sys/types.h>

#define WNOHANG 1
#define WUNTRACED 2
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFEXITED(s) (1)
#define WIFSIGNALED(s) (0)
#define WIFSTOPPED(s) (0)
#define WSTOPSIG(s) (0)
#define WTERMSIG(s) (0)

pid_t waitpid(pid_t pid, int *status, int options);
pid_t wait(int *status);

#endif
