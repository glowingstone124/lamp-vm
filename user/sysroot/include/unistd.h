#ifndef LAMP_LIBC_UNISTD_H
#define LAMP_LIBC_UNISTD_H

#include <stddef.h>
#include <sys/types.h>

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

extern char **environ;
extern char *optarg;
extern int optind;
extern int opterr;
extern int optopt;

pid_t getpid(void);
pid_t getppid(void);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int getgroups(int size, gid_t list[]);
pid_t vfork(void);
int execve(const char *path, char *const argv[], char *const envp[]);
int execv(const char *path, char *const argv[]);
int execvp(const char *file, char *const argv[]);
void _exit(int status) __attribute__((noreturn));
int close(int fd);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
off_t lseek(int fd, off_t offset, int whence);
int dup(int oldfd);
int dup2(int oldfd, int newfd);
int pipe(int pipefd[2]);
int chdir(const char *path);
char *getcwd(char *buf, size_t size);
int access(const char *path, int mode);
unsigned int alarm(unsigned int seconds);
unsigned int sleep(unsigned int seconds);
int getpagesize(void);
int unlink(const char *path);
int rmdir(const char *path);
int link(const char *oldpath, const char *newpath);
int symlink(const char *target, const char *linkpath);
ssize_t readlink(const char *path, char *buf, size_t size);
int isatty(int fd);
int getopt(int argc, char *const argv[], const char *optstring);
int ttyname_r(int fd, char *buf, size_t size);
pid_t setsid(void);
pid_t getsid(pid_t pid);
int tcsetpgrp(int fd, pid_t pgrp);
#define _SC_CLK_TCK 2
long sysconf(int name);
int chown(const char *path, uid_t owner, gid_t group);
int lchown(const char *path, uid_t owner, gid_t group);
int rename(const char *oldpath, const char *newpath);
int setuid(uid_t uid);
int setgid(gid_t gid);
int seteuid(uid_t uid);
int setegid(gid_t gid);
int fchdir(int fd);
int chroot(const char *path);

#endif
int execl(const char *path, const char *arg, ...);
int setresuid(uid_t ruid, uid_t euid, uid_t suid);
int setresgid(gid_t rgid, gid_t egid, gid_t sgid);
pid_t getpgrp(void);
int setpgid(pid_t pid, pid_t pgid);
int setpgrp(void);
pid_t tcgetpgrp(int fd);
