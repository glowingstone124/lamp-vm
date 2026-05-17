#ifndef LAMP_USER_LIBSYS_H
#define LAMP_USER_LIBSYS_H

#include "abi.h"

extern int32_t libsys_errno;

int32_t libsys_call6(uint32_t nr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5);
int32_t libsys_getpid(void);
int32_t libsys_getppid(void);
int32_t libsys_waitpid(int32_t pid, int32_t *status, uint32_t options);
int32_t libsys_read(int32_t fd, void *buf, uint32_t len);
int32_t libsys_write(int32_t fd, const void *buf, uint32_t len);
int32_t libsys_close(int32_t fd);
int32_t libsys_dup(int32_t oldfd);
int32_t libsys_dup2(int32_t oldfd, int32_t newfd);
int32_t libsys_fcntl(int32_t fd, uint32_t cmd, uint32_t arg);
int32_t libsys_open(const char *path, uint32_t flags);
int32_t libsys_lseek(int32_t fd, int32_t offset, uint32_t whence);
int32_t libsys_stat(const char *path, lamp_stat_t *st);
int32_t libsys_fstat(int32_t fd, lamp_stat_t *st);
int32_t libsys_getdents(int32_t fd, lamp_dirent_t *dirp, uint32_t len);
int32_t libsys_access(const char *path, uint32_t mode);
int32_t libsys_chdir(const char *path);
int32_t libsys_getcwd(char *buf, uint32_t size);
int32_t libsys_pipe(int32_t pipefd[2]);
int32_t libsys_ioctl(int32_t fd, uint32_t request, void *arg);
int32_t libsys_sigaction(uint32_t sig, const lamp_sigaction_t *act, lamp_sigaction_t *oldact);
int32_t libsys_sigprocmask(uint32_t how, const uint32_t *set, uint32_t *oldset);
int32_t libsys_kill(int32_t pid, uint32_t sig);
int32_t libsys_umask(uint32_t mask);
int32_t libsys_rename(const char *oldpath, const char *newpath);
int32_t libsys_unlink(const char *path);
int32_t libsys_mkdir(const char *path, uint32_t mode);
int32_t libsys_rmdir(const char *path);
int32_t libsys_link(const char *oldpath, const char *newpath);
int32_t libsys_symlink(const char *target, const char *linkpath);
int32_t libsys_readlink(const char *path, char *buf, uint32_t size);
int32_t libsys_execve(const char *path, const char *const argv[], const char *const envp[]);
int32_t libsys_vfork(void);
int32_t libsys_exit(int32_t code);

#endif
