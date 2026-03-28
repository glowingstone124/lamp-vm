#ifndef LAMP_USER_LIBSYS_H
#define LAMP_USER_LIBSYS_H

#include "abi.h"

extern int32_t libsys_errno;

int32_t libsys_call6(uint32_t nr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5);
int32_t libsys_getpid(void);
int32_t libsys_waitpid(int32_t pid, int32_t *status, uint32_t options);
int32_t libsys_write(int32_t fd, const void *buf, uint32_t len);
int32_t libsys_execve(const char *path, const char *const argv[], const char *const envp[]);
int32_t libsys_vfork(void);
int32_t libsys_exit(int32_t code);

#endif
