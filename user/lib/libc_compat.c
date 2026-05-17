#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <glob.h>
#include <getopt.h>
#include <grp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <pwd.h>
#include <regex.h>
#include <setjmp.h>
#include <string.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <lamp/libsys.h>

int errno;
int h_errno;
static char *g_environ_empty[] = { 0 };
char **environ = g_environ_empty;

struct FILE {
    int fd;
    int eof;
    int err;
};

static FILE g_stdin = { 0, 0, 0 };
static FILE g_stdout = { 1, 0, 0 };
static FILE g_stderr = { 2, 0, 0 };
FILE *stdin = &g_stdin;
FILE *stdout = &g_stdout;
FILE *stderr = &g_stderr;

static void set_errno_from_libsys(void) {
    errno = libsys_errno;
}

static int ret_errno(int rc) {
    if (rc < 0) {
        set_errno_from_libsys();
        return -1;
    }
    errno = 0;
    return rc;
}

pid_t getpid(void) { return (pid_t)ret_errno(libsys_getpid()); }
pid_t getppid(void) { return (pid_t)ret_errno(libsys_getppid()); }
uid_t getuid(void) { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void) { return 0; }
gid_t getegid(void) { return 0; }
int getpagesize(void) { return 4096; }
int getgroups(int size, gid_t list[]) {
    if (size > 0 && list) {
        list[0] = 0;
    }
    return 1;
}
const char *hstrerror(int err) {
    switch (err) {
    case NETDB_SUCCESS: return "Resolver Error 0 (no error)";
    case HOST_NOT_FOUND: return "Unknown host";
    case TRY_AGAIN: return "Host name lookup failure";
    case NO_RECOVERY: return "Unknown server error";
    case NO_DATA: return "No address associated with name";
    default: return "Resolver error";
    }
}
int inet_aton(const char *cp, struct in_addr *inp) {
    unsigned int parts[4] = {0, 0, 0, 0};
    const char *p = cp;
    char *end;
    int i;
    if (!cp || !inp) return 0;
    for (i = 0; i < 4; i++) {
        unsigned long v;
        if (*p == '\0') return 0;
        v = strtoul(p, &end, 10);
        if (end == p || v > 255) return 0;
        parts[i] = (unsigned int)v;
        if (i == 3) {
            if (*end != '\0') return 0;
            break;
        }
        if (*end != '.') return 0;
        p = end + 1;
    }
    inp->s_addr = htonl((parts[0] << 24) | (parts[1] << 16) |
                        (parts[2] << 8) | parts[3]);
    return 1;
}
char *inet_ntoa(struct in_addr in) { static char buf[16]; unsigned int a = ntohl(in.s_addr); snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (a >> 24) & 0xff, (a >> 16) & 0xff, (a >> 8) & 0xff, a & 0xff); return buf; }
int setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen) { (void)fd; (void)level; (void)optname; (void)optval; (void)optlen; return 0; }
int getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen) { (void)fd; (void)level; (void)optname; (void)optval; (void)optlen; return -1; }
int getpeername(int fd, struct sockaddr *addr, socklen_t *len) { (void)fd; (void)addr; (void)len; return -1; }
struct servent *getservbyname(const char *name, const char *proto) { (void)name; (void)proto; return 0; }
struct servent *getservbyport(int port, const char *proto) { (void)port; (void)proto; return 0; }
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) { (void)node; (void)service; (void)hints; (void)res; return EAI_FAIL; }
void freeaddrinfo(struct addrinfo *res) { (void)res; }
int getnameinfo(const struct sockaddr *addr, socklen_t addrlen, char *host, socklen_t hostlen, char *serv, socklen_t servlen, int flags) { (void)addr; (void)addrlen; (void)host; (void)hostlen; (void)serv; (void)servlen; (void)flags; return EAI_FAIL; }
struct hostent *gethostbyname(const char *name) {
    static struct in_addr addr;
    static char *aliases[] = { 0 };
    static char *addr_list[] = { (char *)&addr, 0 };
    static struct hostent host = { 0, aliases, AF_INET, sizeof(addr), addr_list };
    if (!name) {
        h_errno = HOST_NOT_FOUND;
        return 0;
    }
    if (strcmp(name, "localhost") == 0) {
        addr.s_addr = htonl(INADDR_LOOPBACK);
    } else if (!inet_aton(name, &addr)) {
        h_errno = HOST_NOT_FOUND;
        return 0;
    }
    host.h_name = (char *)name;
    h_errno = NETDB_SUCCESS;
    return &host;
}
pid_t vfork(void) { return (pid_t)ret_errno(libsys_vfork()); }
int execve(const char *path, char *const argv[], char *const envp[]) {
    return ret_errno(libsys_execve(path, (const char *const *)argv, (const char *const *)envp));
}
int execv(const char *path, char *const argv[]) { return execve(path, argv, environ); }
int execvp(const char *file, char *const argv[]) {
    /* POSIX: if file contains '/', exec directly; otherwise search PATH */
    const char *path_env;
    char full_path[256];
    if (!file) { errno = EFAULT; return -1; }
    if (strchr(file, '/')) return execve(file, argv, environ);
    path_env = getenv("PATH");
    if (!path_env) path_env = "/bin";
    while (*path_env) {
        const char *end = strchr(path_env, ':');
        size_t dir_len = end ? (size_t)(end - path_env) : strlen(path_env);
        if (dir_len == 0) { path_env++; continue; }
        if (dir_len + 1 + strlen(file) + 1 > sizeof(full_path)) { path_env = end ? end + 1 : path_env + dir_len; continue; }
        memcpy(full_path, path_env, dir_len);
        full_path[dir_len] = '/';
        strcpy(full_path + dir_len + 1, file);
        execve(full_path, argv, environ);
        if (errno != ENOENT) break;
        path_env = end ? end + 1 : path_env + dir_len;
    }
    return -1;
}
void _exit(int status) {
    (void)libsys_exit(status);
    for (;;) {
    }
}
void exit(int status) { _exit(status); }
int close(int fd) { return ret_errno(libsys_close(fd)); }
ssize_t read(int fd, void *buf, size_t count) { return (ssize_t)ret_errno(libsys_read(fd, buf, (uint32_t)count)); }
ssize_t write(int fd, const void *buf, size_t count) { return (ssize_t)ret_errno(libsys_write(fd, buf, (uint32_t)count)); }
off_t lseek(int fd, off_t offset, int whence) { return (off_t)ret_errno(libsys_lseek(fd, offset, (uint32_t)whence)); }
int dup(int oldfd) { return ret_errno(libsys_dup(oldfd)); }
int dup2(int oldfd, int newfd) { return ret_errno(libsys_dup2(oldfd, newfd)); }
int pipe(int pipefd[2]) { return ret_errno(libsys_pipe(pipefd)); }
int chdir(const char *path) { return ret_errno(libsys_chdir(path)); }
int access(const char *path, int mode) { return ret_errno(libsys_access(path, (uint32_t)mode)); }
int unlink(const char *path) { return ret_errno(libsys_unlink(path)); }
int rmdir(const char *path) { return ret_errno(libsys_rmdir(path)); }
int link(const char *oldpath, const char *newpath) { return ret_errno(libsys_link(oldpath, newpath)); }
int symlink(const char *target, const char *linkpath) { return ret_errno(libsys_symlink(target, linkpath)); }
ssize_t readlink(const char *path, char *buf, size_t size) {
    return (ssize_t)ret_errno(libsys_readlink(path, buf, (uint32_t)size));
}
pid_t waitpid(pid_t pid, int *status, int options) {
    int rc = libsys_waitpid(pid, status, (uint32_t)(options & WNOHANG));
    if (rc < 0) {
        set_errno_from_libsys();
        if (pid == (pid_t)-1 && (options & WNOHANG)) {
            errno = ECHILD;
        }
        return (pid_t)-1;
    }
    errno = 0;
    return (pid_t)rc;
}
pid_t wait(int *status) { return waitpid(-1, status, 0); }
pid_t setsid(void) { return getpid(); }
pid_t getsid(pid_t pid) { (void)pid; return getpid(); }
int tcsetpgrp(int fd, pid_t pgrp) { (void)fd; (void)pgrp; return 0; }
int ttyname_r(int fd, char *buf, size_t size) { (void)fd; if (buf && size) buf[0] = '\0'; errno = ENOTTY; return -1; }
int isatty(int fd) {
    return fd >= 0 && fd <= 2;
}

int open(const char *path, int flags, ...) {
    return ret_errno(libsys_open(path, (uint32_t)flags));
}

int fcntl(int fd, int cmd, ...) {
    va_list ap;
    uint32_t arg = 0;
    int rc;
    va_start(ap, cmd);
    arg = va_arg(ap, uint32_t);
    va_end(ap);
    rc = libsys_fcntl(fd, (uint32_t)cmd, arg);
    if (rc < 0) {
        set_errno_from_libsys();
        if ((cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) && arg >= 32u) {
            errno = EBADF;
        }
        return -1;
    }
    errno = 0;
    return rc;
}

int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    void *arg;
    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);
    return ret_errno(libsys_ioctl(fd, (uint32_t)request, arg));
}

char *getcwd(char *buf, size_t size) {
    int rc = libsys_getcwd(buf, (uint32_t)size);
    if (rc < 0) {
        set_errno_from_libsys();
        return 0;
    }
    errno = 0;
    return buf;
}

static void stat_copy(struct stat *dst, const lamp_stat_t *src) {
    dst->st_dev = src->st_dev;
    dst->st_ino = src->st_ino;
    dst->st_mode = src->st_mode;
    dst->st_nlink = src->st_nlink;
    dst->st_uid = src->st_uid;
    dst->st_gid = src->st_gid;
    dst->st_rdev = src->st_rdev;
    dst->st_size = (off_t)src->st_size;
    dst->st_blksize = src->st_blksize;
    dst->st_blocks = src->st_blocks;
    dst->st_atime = 0;
    dst->st_mtime = 0;
    dst->st_ctime = 0;
}

int stat(const char *path, struct stat *st) {
    lamp_stat_t lst;
    int rc = libsys_stat(path, &lst);
    if (rc < 0) {
        set_errno_from_libsys();
        return -1;
    }
    stat_copy(st, &lst);
    errno = 0;
    return 0;
}
int lstat(const char *path, struct stat *st) { return stat(path, st); }
int fstat(int fd, struct stat *st) {
    lamp_stat_t lst;
    int rc = libsys_fstat(fd, &lst);
    if (rc < 0) {
        set_errno_from_libsys();
        return -1;
    }
    stat_copy(st, &lst);
    errno = 0;
    return 0;
}
int mkdir(const char *path, mode_t mode) { return ret_errno(libsys_mkdir(path, mode)); }
mode_t umask(mode_t mask) { return (mode_t)libsys_umask(mask); }

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    return ret_errno(libsys_call6(LAMP_SYS_POLL, (uint32_t)(uintptr_t)fds, (uint32_t)nfds, (uint32_t)timeout, 0, 0, 0));
}

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
    int timeout_ms = -1;
    if (timeout) {
        timeout_ms = (int)(timeout->tv_sec * 1000u + (uint32_t)timeout->tv_usec / 1000u);
    }
    return ret_errno(libsys_call6(LAMP_SYS_SELECT,
                                  (uint32_t)nfds,
                                  (uint32_t)(uintptr_t)readfds,
                                  (uint32_t)(uintptr_t)writefds,
                                  (uint32_t)(uintptr_t)exceptfds,
                                  (uint32_t)timeout_ms,
                                  0));
}

clock_t times(struct tms *buf) { if (buf) memset(buf, 0, sizeof(*buf)); return 0; }
int settimeofday(const struct timeval *tv, const void *tz) { (void)tv; (void)tz; return 0; }
int gettimeofday(struct timeval *tv, void *tz) {
    return ret_errno(libsys_call6(LAMP_SYS_GETTIMEOFDAY, (uint32_t)(uintptr_t)tv, (uint32_t)(uintptr_t)tz, 0, 0, 0, 0));
}
int clock_gettime(int clock_id, struct timespec *ts) {
    return ret_errno(libsys_call6(LAMP_SYS_CLOCK_GETTIME, (uint32_t)clock_id, (uint32_t)(uintptr_t)ts, 0, 0, 0, 0));
}
int clock_getres(int clock_id, struct timespec *ts) {
    return ret_errno(libsys_call6(LAMP_SYS_CLOCK_GETRES, (uint32_t)clock_id, (uint32_t)(uintptr_t)ts, 0, 0, 0, 0));
}
int nanosleep(const struct timespec *req, struct timespec *rem) {
    return ret_errno(libsys_call6(LAMP_SYS_NANOSLEEP, (uint32_t)(uintptr_t)req, (uint32_t)(uintptr_t)rem, 0, 0, 0, 0));
}
time_t time(time_t *tloc) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) < 0) {
        return (time_t)-1;
    }
    if (tloc) *tloc = ts.tv_sec;
    return ts.tv_sec;
}
struct tm *localtime_r(const time_t *timep, struct tm *result) {
    (void)timep;
    if (!result) return 0;
    memset(result, 0, sizeof(*result));
    result->tm_mday = 1;
    result->tm_year = 70;
    return result;
}
struct tm *localtime(const time_t *timep) {
    static struct tm tm;
    return localtime_r(timep, &tm);
}
char *ctime(const time_t *timep) {
    (void)timep;
    return "Thu Jan  1 00:00:00 1970\n";
}
time_t mktime(struct tm *tm) { if (tm) { tm->tm_wday = 4; return 0; } return -1; }
void tzset(void) {}
size_t strftime(char *s, size_t max, const char *format, const struct tm *tm) {
    (void)tm;
    if (!s || max == 0) return 0;
    if (format && strcmp(format, "%Y-%m-%d %H:%M:%S %z") == 0) {
        return (size_t)snprintf(s, max, "1970-01-01 00:00:00 +0000");
    }
    return (size_t)snprintf(s, max, "1970-01-01");
}

int tcgetattr(int fd, struct termios *t) {
    (void)fd;
    (void)t;
    errno = ENOTTY;
    return -1;
}
int tcsetattr(int fd, int optional_actions, const struct termios *t) {
    (void)fd;
    (void)optional_actions;
    (void)t;
    errno = ENOTTY;
    return -1;
}
int tcflush(int fd, int queue_selector) {
    (void)fd;
    (void)queue_selector;
    return 0;
}
unsigned int alarm(unsigned int seconds) {
    (void)seconds;
    return 0;
}
unsigned int sleep(unsigned int seconds) {
    struct timespec req;
    req.tv_sec = (time_t)seconds;
    req.tv_nsec = 0;
    return nanosleep(&req, 0) < 0 ? seconds : 0;
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact) {
    lamp_sigaction_t la;
    lamp_sigaction_t old;
    lamp_sigaction_t *lap = 0;
    lamp_sigaction_t *oldp = oldact ? &old : 0;
    if (act) {
        la.handler = (uint32_t)(uintptr_t)act->sa_handler;
        la.flags = (uint32_t)act->sa_flags;
        la.mask = act->sa_mask;
        la.restorer = (uint32_t)(uintptr_t)act->sa_restorer;
        lap = &la;
    }
    if (libsys_sigaction((uint32_t)sig, lap, oldp) < 0) {
        set_errno_from_libsys();
        return -1;
    }
    if (oldact) {
        oldact->sa_handler = (sighandler_t)(uintptr_t)old.handler;
        oldact->sa_flags = (int)old.flags;
        oldact->sa_mask = old.mask;
        oldact->sa_restorer = (void (*)(void))(uintptr_t)old.restorer;
    }
    return 0;
}
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    return ret_errno(libsys_sigprocmask((uint32_t)how, set, oldset));
}
int kill(pid_t pid, int sig) { return ret_errno(libsys_kill(pid, (uint32_t)sig)); }
int sigemptyset(sigset_t *set) { *set = 0; return 0; }
int sigfillset(sigset_t *set) { *set = 0xffffffffu; return 0; }
int sigaddset(sigset_t *set, int sig) { if (sig > 0 && sig <= 32) *set |= 1u << (sig - 1); return 0; }
int sigdelset(sigset_t *set, int sig) { if (sig > 0 && sig <= 32) *set &= ~(1u << (sig - 1)); return 0; }
int sigismember(const sigset_t *set, int sig) { if (sig > 0 && sig <= 32 && set) return !!(*set & (1u << (sig - 1))); return 0; }
int sigisemptyset(const sigset_t *set) { return set ? *set == 0 : 1; }
sighandler_t signal(int sig, sighandler_t handler) {
    struct sigaction act, old;
    act.sa_handler = handler;
    act.sa_mask = 0;
    act.sa_flags = SA_RESTART;
    act.sa_restorer = 0;
    if (sigaction(sig, &act, &old) < 0) return SIG_ERR;
    return old.sa_handler;
}
int sigsuspend(const sigset_t *mask) { (void)mask; errno = ENOSYS; return -1; }
int raise(int sig) { return kill(getpid(), sig); }

int socket(int domain, int type, int protocol) {
    return ret_errno(libsys_call6(LAMP_SYS_SOCKET, (uint32_t)domain, (uint32_t)type, (uint32_t)protocol, 0, 0, 0));
}
int connect(int fd, const struct sockaddr *addr, socklen_t len) {
    return ret_errno(libsys_call6(LAMP_SYS_CONNECT, (uint32_t)fd, (uint32_t)(uintptr_t)addr, len, 0, 0, 0));
}
int bind(int fd, const struct sockaddr *addr, socklen_t len) {
    return ret_errno(libsys_call6(LAMP_SYS_BIND, (uint32_t)fd, (uint32_t)(uintptr_t)addr, len, 0, 0, 0));
}
int listen(int fd, int backlog) { return ret_errno(libsys_call6(LAMP_SYS_LISTEN, (uint32_t)fd, (uint32_t)backlog, 0, 0, 0, 0)); }
int accept(int fd, struct sockaddr *addr, socklen_t *len) {
    return ret_errno(libsys_call6(LAMP_SYS_ACCEPT, (uint32_t)fd, (uint32_t)(uintptr_t)addr, (uint32_t)(uintptr_t)len, 0, 0, 0));
}
int getsockname(int fd, struct sockaddr *addr, socklen_t *len) {
    (void)fd;
    (void)addr;
    (void)len;
    errno = ENOSYS;
    return -1;
}
ssize_t sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen) { (void)dest_addr; (void)addrlen; return send(fd, buf, len, flags); }
ssize_t send(int fd, const void *buf, size_t len, int flags) {
    return (ssize_t)ret_errno(libsys_call6(LAMP_SYS_SEND, (uint32_t)fd, (uint32_t)(uintptr_t)buf, (uint32_t)len, (uint32_t)flags, 0, 0));
}
ssize_t recv(int fd, void *buf, size_t len, int flags) {
    return (ssize_t)ret_errno(libsys_call6(LAMP_SYS_RECV, (uint32_t)fd, (uint32_t)(uintptr_t)buf, (uint32_t)len, (uint32_t)flags, 0, 0));
}

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}
void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        while (n) { n--; d[n] = s[n]; }
    }
    return dst;
}
void *memset(void *dst, int c, size_t n) {
    unsigned char *d = dst;
    for (size_t i = 0; i < n; i++) d[i] = (unsigned char)c;
    return dst;
}
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s && s[n]) n++; return n; }
size_t strnlen(const char *s, size_t maxlen) { size_t n = 0; while (s && n < maxlen && s[n]) n++; return n; }
char *strcpy(char *dst, const char *src) { char *d = dst; while ((*d++ = *src++) != 0) {} return dst; }
char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}
char *strcat(char *dst, const char *src) { strcpy(dst + strlen(dst), src); return dst; }
char *stpcpy(char *dst, const char *src) { while ((*dst = *src) != 0) { dst++; src++; } return dst; }
void *memchr(const void *s, int c, size_t n) { const unsigned char *p = s; for (size_t i = 0; i < n; i++) if (p[i] == (unsigned char)c) return (void *)(p + i); return 0; }
void *mempcpy(void *dst, const void *src, size_t n) { return (char *)memcpy(dst, src, n) + n; }
unsigned int if_nametoindex(const char *name) { (void)name; return 0; }
char *if_indextoname(unsigned int ifindex, char *name) { (void)ifindex; if (name) name[0] = '\0'; return name; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == 0 || b[i] == 0) return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}
char *strchr(const char *s, int c) { while (*s) { if (*s == (char)c) return (char *)s; s++; } return c == 0 ? (char *)s : 0; }
char *strchrnul(const char *s, int c) { char *p = strchr(s, c); return p ? p : (char *)(s + strlen(s)); }
char *strrchr(const char *s, int c) {
    const char *last = 0;
    char ch = (char)c;
    for (;;) {
        if (*s == ch) last = s;
        if (*s == '\0') break;
        s++;
    }
    return (char *)last;
}
char *strstr(const char *h, const char *n) {
    size_t nl = strlen(n);
    if (nl == 0) return (char *)h;
    while (*h) { if (strncmp(h, n, nl) == 0) return (char *)h; h++; }
    return 0;
}
size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    while (s[n] && strchr(accept, s[n])) n++;
    return n;
}
size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n])) n++;
    return n;
}
char *strdup(const char *s) { size_t n = strlen(s) + 1; char *p = malloc(n); if (p) memcpy(p, s, n); return p; }
char *strndup(const char *s, size_t n) { size_t len = strnlen(s, n); char *p = malloc(len + 1); if (p) { memcpy(p, s, len); p[len] = '\0'; } return p; }
char *strerror(int errnum) {
    switch (errnum) {
        case ENOENT: return "No such file";
        case EINVAL: return "Invalid argument";
        case EBADF: return "Bad file descriptor";
        case ENOSYS: return "Function not implemented";
        default: return "Error";
    }
}
int strcoll(const char *a, const char *b) { return strcmp(a, b); }
int strverscmp(const char *a, const char *b) { return strcmp(a, b); }
int strcasecmp(const char *a, const char *b) { while (*a && *b) { int d = tolower(*a) - tolower(*b); if (d) return d; a++; b++; } return tolower(*a) - tolower(*b); }
int strncasecmp(const char *a, const char *b, size_t n) { size_t i = 0; while (i < n && *a && *b) { int d = tolower(*a) - tolower(*b); if (d) return d; a++; b++; i++; } return i == n ? 0 : tolower(*a) - tolower(*b); }

int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
int isprint(int c) { return c >= 0x20 && c < 0x7f; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int tolower(int c) { return isupper(c) ? c + ('a' - 'A') : c; }
int toupper(int c) { return islower(c) ? c - ('a' - 'A') : c; }

static unsigned char g_heap[256 * 1024];
static size_t g_heap_used;

static size_t align4(size_t n) { return (n + 3u) & ~3u; }
void *malloc(size_t size) {
    size = align4(size ? size : 1u);
    size_t total = align4(size + sizeof(size_t));
    if (g_heap_used + total > sizeof(g_heap)) {
        errno = ENOMEM;
        return 0;
    }
    unsigned char *raw = &g_heap[g_heap_used];
    *(size_t *)raw = size;
    g_heap_used += total;
    return raw + sizeof(size_t);
}
void free(void *ptr) { (void)ptr; }
void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}
void *realloc(void *ptr, size_t size) {
    void *p = malloc(size);
    if (p && ptr) {
        size_t old_size = *(size_t *)((unsigned char *)ptr - sizeof(size_t));
        size_t copy = old_size < size ? old_size : size;
        memcpy(p, ptr, copy);
    }
    return p;
}

long strtol(const char *nptr, char **endptr, int base) {
    int neg = 0;
    unsigned long v;
    while (isspace((unsigned char)*nptr)) nptr++;
    if (*nptr == '-') { neg = 1; nptr++; } else if (*nptr == '+') nptr++;
    v = strtoul(nptr, endptr, base);
    return neg ? -(long)v : (long)v;
}
unsigned long strtoul(const char *nptr, char **endptr, int base) {
    unsigned long v = 0;
    if (base == 0) base = 10;
    while (isspace((unsigned char)*nptr)) nptr++;
    while (*nptr) {
        int d;
        if (*nptr >= '0' && *nptr <= '9') d = *nptr - '0';
        else if (*nptr >= 'a' && *nptr <= 'z') d = *nptr - 'a' + 10;
        else if (*nptr >= 'A' && *nptr <= 'Z') d = *nptr - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * (unsigned)base + (unsigned)d;
        nptr++;
    }
    if (endptr) *endptr = (char *)nptr;
    return v;
}
long long strtoll(const char *nptr, char **endptr, int base) { return (long long)strtol(nptr, endptr, base); }
unsigned long long strtoull(const char *nptr, char **endptr, int base) { return (unsigned long long)strtoul(nptr, endptr, base); }
double strtod(const char *nptr, char **endptr) {
    const char *p = nptr;
    while (p && isspace((unsigned char)*p)) p++;
    if (p && (*p == '-' || *p == '+')) p++;
    while (p && isdigit((unsigned char)*p)) p++;
    if (p && *p == '.') {
        p++;
        while (isdigit((unsigned char)*p)) p++;
    }
    if (endptr) *endptr = (char *)(p ? p : nptr);
    return 0.0;
}
static unsigned long g_rand_seed = 1;
int rand(void) { g_rand_seed = g_rand_seed * 1103515245u + 12345u; return (int)((g_rand_seed / 65536u) % (unsigned)(RAND_MAX + 1)); }
void srand(unsigned int seed) { g_rand_seed = seed; }
int atoi(const char *s) { return (int)strtol(s, 0, 10); }
char *getenv(const char *name) { (void)name; return 0; }
int setenv(const char *name, const char *value, int overwrite) { (void)name; (void)value; (void)overwrite; return 0; }
int putenv(char *string) { (void)string; return 0; }
int unsetenv(const char *name) { (void)name; return 0; }
int clearenv(void) { return 0; }

void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
    unsigned char *b = base;
    unsigned char tmp[64];
    if (size > sizeof(tmp)) return;
    for (size_t i = 0; i < nmemb; i++) {
        for (size_t j = i + 1; j < nmemb; j++) {
            if (compar(&b[i * size], &b[j * size]) > 0) {
                memcpy(tmp, &b[i * size], size);
                memcpy(&b[i * size], &b[j * size], size);
                memcpy(&b[j * size], tmp, size);
            }
        }
    }
}

static int file_fd(FILE *stream) { return stream ? stream->fd : 1; }
int fflush(FILE *stream) { (void)stream; return 0; }
int fileno(FILE *stream) { return file_fd(stream); }
int fileno_unlocked(FILE *stream) { return fileno(stream); }
FILE *fdopen(int fd, const char *mode) { (void)mode; FILE *f = malloc(sizeof(FILE)); if (f) { f->fd = fd; f->eof = 0; f->err = 0; } return f; }
FILE *fopen(const char *path, const char *mode) {
    int flags = O_RDONLY;
    if (mode && mode[0] == 'w') flags = O_WRONLY | O_CREAT | O_TRUNC;
    int fd = open(path, flags, 0666);
    return fd < 0 ? 0 : fdopen(fd, mode);
}
FILE *freopen(const char *path, const char *mode, FILE *stream) {
    int flags = O_RDONLY;
    int fd;
    if (!stream) {
        return fopen(path, mode);
    }
    if (mode && mode[0] == 'w') flags = O_WRONLY | O_CREAT | O_TRUNC;
    fd = open(path, flags, 0666);
    if (fd < 0) {
        return 0;
    }
    close(stream->fd);
    stream->fd = fd;
    stream->eof = 0;
    stream->err = 0;
    return stream;
}
int fclose(FILE *stream) { int fd = file_fd(stream); if (stream != stdin && stream != stdout && stream != stderr) free(stream); return close(fd); }
int fseeko(FILE *stream, off_t offset, int whence) { return lseek(file_fd(stream), offset, whence) < 0 ? -1 : 0; }
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    ssize_t n = read(file_fd(stream), ptr, size * nmemb);
    if (n <= 0) { if (stream) stream->eof = 1; return 0; }
    return (size_t)n / size;
}
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    ssize_t n = write(file_fd(stream), ptr, size * nmemb);
    return n <= 0 ? 0 : (size_t)n / size;
}
char *fgets(char *s, int size, FILE *stream) {
    if (size <= 0) return 0;
    int i = 0;
    while (i + 1 < size) {
        char c;
        if (read(file_fd(stream), &c, 1) != 1) break;
        s[i++] = c;
        if (c == '\n') break;
    }
    if (i == 0) return 0;
    s[i] = '\0';
    return s;
}
ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
    size_t cap;
    size_t len = 0;
    int c;
    char *buf;
    if (!lineptr || !n) { errno = EINVAL; return -1; }
    cap = *n ? *n : 128;
    buf = *lineptr ? *lineptr : malloc(cap);
    if (!buf) { errno = ENOMEM; return -1; }
    while ((c = fgetc(stream)) != EOF) {
        if (len + 1 >= cap) {
            char *newbuf;
            cap *= 2;
            newbuf = realloc(buf, cap);
            if (!newbuf) { errno = ENOMEM; return -1; }
            buf = newbuf;
        }
        buf[len++] = (char)c;
        if (c == '\n') break;
    }
    if (len == 0 && c == EOF) {
        if (!*lineptr) free(buf);
        return -1;
    }
    buf[len] = '\0';
    *lineptr = buf;
    *n = cap;
    return (ssize_t)len;
}
int feof(FILE *stream) { return stream ? stream->eof : 1; }
int feof_unlocked(FILE *stream) { return feof(stream); }
int ferror(FILE *stream) { return stream ? stream->err : 1; }
int ferror_unlocked(FILE *stream) { return ferror(stream); }
void clearerr(FILE *stream) { if (stream) { stream->eof = 0; stream->err = 0; } }
int fputs(const char *s, FILE *stream) { return write(file_fd(stream), s, strlen(s)) < 0 ? EOF : 0; }
int fputs_unlocked(const char *s, FILE *stream) { return fputs(s, stream); }
int puts(const char *s) { if (fputs(s, stdout) == EOF) return EOF; return fputs("\n", stdout); }
int fputc(int c, FILE *stream) { unsigned char ch = (unsigned char)c; return write(file_fd(stream), &ch, 1) == 1 ? c : EOF; }
int putc(int c, FILE *stream) { return fputc(c, stream); }
int putc_unlocked(int c, FILE *stream) { return fputc(c, stream); }
int putchar(int c) { return fputc(c, stdout); }
int putchar_unlocked(int c) { return putchar(c); }
int fgetc(FILE *stream) { unsigned char ch; return read(file_fd(stream), &ch, 1) == 1 ? ch : EOF; }
int getc(FILE *stream) { return fgetc(stream); }
int getc_unlocked(FILE *stream) { return fgetc(stream); }
int getchar(void) { return fgetc(stdin); }
int getchar_unlocked(void) { return getchar(); }
char *fgets_unlocked(char *s, int size, FILE *stream) { return fgets(s, size, stream); }
void perror(const char *s) { if (s) { fputs(s, stderr); fputs(": ", stderr); } fputs(strerror(errno), stderr); fputs("\n", stderr); }
void openlog(const char *ident, int option, int facility) { (void)ident; (void)option; (void)facility; }
void vsyslog(int priority, const char *format, va_list ap) { (void)priority; vfprintf(stderr, format, ap); fputc('\n', stderr); }
void syslog(int priority, const char *format, ...) { va_list ap; va_start(ap, format); vsyslog(priority, format, ap); va_end(ap); }
void closelog(void) {}
unsigned long long monotonic_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) return 0;
    return (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)(ts.tv_nsec / 1000);
}
unsigned long long monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) return 0;
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long)(ts.tv_nsec / 1000000L);
}
char *strftime_HHMMSS(char *buf, unsigned len, const struct tm *ptm) {
    if (!buf || len == 0) return buf;
    snprintf(buf, len, "%02d:%02d:%02d", ptm ? ptm->tm_hour : 0, ptm ? ptm->tm_min : 0, ptm ? ptm->tm_sec : 0);
    return buf;
}
size_t wcslen(const int *s) { size_t n = 0; if (!s) return 0; while (s[n]) n++; return n; }
off_t bb_copyfd_eof(int fd1, int fd2) {
    char buf[512];
    off_t total = 0;
    for (;;) {
        ssize_t n = read(fd1, buf, sizeof(buf));
        if (n <= 0) return n < 0 ? -1 : total;
        if (write(fd2, buf, (size_t)n) != n) return -1;
        total += n;
    }
}

static void out_char(char **buf, size_t *left, int fd, int *count, char c) {
    if (buf) {
        if (*left > 1) { **buf = c; (*buf)++; (*left)--; }
    } else {
        (void)write(fd, &c, 1);
    }
    (*count)++;
}
static void out_str(char **buf, size_t *left, int fd, int *count, const char *s) {
    if (!s) s = "(null)";
    while (*s) out_char(buf, left, fd, count, *s++);
}
static void out_uint(char **buf, size_t *left, int fd, int *count, unsigned long v, unsigned base, int neg) {
    char tmp[32];
    int n = 0;
    if (neg) out_char(buf, left, fd, count, '-');
    do { unsigned d = v % base; tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= base; } while (v);
    while (n) out_char(buf, left, fd, count, tmp[--n]);
}
int vasprintf(char **strp, const char *fmt, va_list ap) {
    int n = vsnprintf(0, 0, fmt, ap);
    if (n < 0) return -1;
    *strp = malloc((size_t)(n + 1));
    if (!*strp) return -1;
    return vsnprintf(*strp, (size_t)(n + 1), fmt, ap);
}
int vsnprintf(char *str, size_t size, const char *fmt, va_list ap) {
    char *p = str;
    size_t left = size;
    int count = 0;
    while (*fmt) {
        if (*fmt != '%') { out_char(str ? &p : 0, &left, -1, &count, *fmt++); continue; }
        fmt++;
        while (*fmt == 'l' || *fmt == 'z') fmt++;
        if (*fmt == 's') out_str(str ? &p : 0, &left, -1, &count, va_arg(ap, const char *));
        else if (*fmt == 'c') out_char(str ? &p : 0, &left, -1, &count, (char)va_arg(ap, int));
        else if (*fmt == 'd' || *fmt == 'i') { long v = va_arg(ap, int); out_uint(str ? &p : 0, &left, -1, &count, v < 0 ? (unsigned long)-v : (unsigned long)v, 10, v < 0); }
        else if (*fmt == 'u') out_uint(str ? &p : 0, &left, -1, &count, va_arg(ap, unsigned), 10, 0);
        else if (*fmt == 'x' || *fmt == 'X') out_uint(str ? &p : 0, &left, -1, &count, va_arg(ap, unsigned), 16, 0);
        else if (*fmt == '%') out_char(str ? &p : 0, &left, -1, &count, '%');
        else out_char(str ? &p : 0, &left, -1, &count, *fmt);
        if (*fmt) fmt++;
    }
    if (str && size != 0) *p = '\0';
    return count;
}
int snprintf(char *str, size_t size, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int n = vsnprintf(str, size, fmt, ap); va_end(ap); return n; }
int sprintf(char *str, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int n = vsnprintf(str, 0xffffffffu, fmt, ap); va_end(ap); return n; }
int sscanf(const char *str, const char *fmt, ...) { (void)str; (void)fmt; return 0; }
int dprintf(int fd, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    (void)write(fd, buf, strlen(buf));
    return n;
}
int vfprintf(FILE *stream, const char *fmt, va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    (void)write(file_fd(stream), buf, strlen(buf));
    return n;
}
int fprintf(FILE *stream, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int n = vfprintf(stream, fmt, ap); va_end(ap); return n; }
int printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); int n = vfprintf(stdout, fmt, ap); va_end(ap); return n; }

typedef struct {
    int fd;
    struct dirent ent;
} lamp_DIR;
struct DIR { lamp_DIR d; };
DIR *opendir(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    DIR *d = malloc(sizeof(DIR));
    if (!d) { close(fd); return 0; }
    d->d.fd = fd;
    return d;
}
struct dirent *readdir(DIR *dirp) {
    lamp_dirent_t lent;
    int rc;
    if (!dirp) return 0;
    rc = libsys_getdents(dirp->d.fd, &lent, sizeof(lent));
    if (rc <= 0) return 0;
    dirp->d.ent.d_ino = lent.d_ino;
    dirp->d.ent.d_off = (long)lent.d_off;
    dirp->d.ent.d_reclen = (unsigned short)lent.d_reclen;
    dirp->d.ent.d_type = (unsigned char)lent.d_type;
    strncpy(dirp->d.ent.d_name, lent.d_name, sizeof(dirp->d.ent.d_name));
    return &dirp->d.ent;
}
int closedir(DIR *dirp) { int rc = dirp ? close(dirp->d.fd) : -1; free(dirp); return rc; }

char *dirname(char *path) {
    char *slash = strrchr(path, '/');
    if (!slash) return ".";
    if (slash == path) { path[1] = '\0'; return path; }
    *slash = '\0';
    return path;
}
char *basename(char *path) {
    char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

struct passwd *getpwuid(uid_t uid) { (void)uid; return 0; }
struct passwd *getpwnam(const char *name) { (void)name; return 0; }
struct passwd *bb_internal_getpwuid(uid_t uid) { return getpwuid(uid); }
struct passwd *bb_internal_getpwnam(const char *name) { return getpwnam(name); }
struct passwd *bb_internal_getpwent(void) { return 0; }
void bb_internal_setpwent(void) {}
void bb_internal_endpwent(void) {}
void endpwent(void) {}
struct group *getgrgid(gid_t gid) { (void)gid; return 0; }
struct group *getgrnam(const char *name) { (void)name; return 0; }
struct group *bb_internal_getgrgid(gid_t gid) { return getgrgid(gid); }
struct group *bb_internal_getgrnam(const char *name) { return getgrnam(name); }
int initgroups(const char *user, gid_t group) { (void)user; (void)group; return 0; }
void endgrent(void) {}

int uname(struct utsname *buf) {
    if (!buf) { errno = EFAULT; return -1; }
    strcpy(buf->sysname, "Lamp");
    strcpy(buf->nodename, "lamp");
    strcpy(buf->release, "0");
    strcpy(buf->version, "0");
    strcpy(buf->machine, "lamp");
    return 0;
}
int sysinfo(struct sysinfo *info) { if (info) memset(info, 0, sizeof(*info)); return 0; }
int statfs(const char *path, struct statfs *buf) { (void)path; if (buf) memset(buf, 0, sizeof(*buf)); return 0; }
int getrlimit(int resource, struct rlimit *rlim) { (void)resource; if (rlim) { rlim->rlim_cur = 32; rlim->rlim_max = 32; } return 0; }
int setrlimit(int resource, const struct rlimit *rlim) { (void)resource; (void)rlim; return 0; }
void *mmap(void *addr, unsigned long len, int prot, int flags, int fd, long off) { (void)addr; (void)prot; (void)flags; (void)fd; (void)off; return malloc(len); }
int munmap(void *addr, unsigned long len) { (void)len; free(addr); return 0; }
int sched_getaffinity(pid_t pid, size_t cpusetsize, void *mask) {
    (void)pid;
    if (cpusetsize >= sizeof(unsigned long) && mask) {
        memset(mask, 0, cpusetsize);
        *(unsigned long *)mask = 1;
        return 0;
    }
    errno = EINVAL;
    return -1;
}
long sysconf(int name) { if (name == _SC_CLK_TCK) return 100; return -1; }
int rename(const char *oldpath, const char *newpath) { return ret_errno(libsys_rename(oldpath, newpath)); }
int setuid(uid_t uid) { (void)uid; return 0; }
int setgid(gid_t gid) { (void)gid; return 0; }
int seteuid(uid_t uid) { (void)uid; return 0; }
int setegid(gid_t gid) { (void)gid; return 0; }
int fchdir(int fd) { (void)fd; return 0; }
int chroot(const char *path) { (void)path; errno = ENOSYS; return -1; }
int mkstemp(char *template) { (void)template; errno = ENOSYS; return -1; }
char *realpath(const char *path, char *resolved_path) {
    if (!path || !resolved_path) { errno = EINVAL; return 0; }
    strcpy(resolved_path, path);
    return resolved_path;
}
int chown(const char *path, uid_t owner, gid_t group) { (void)path; (void)owner; (void)group; return 0; }
int lchown(const char *path, uid_t owner, gid_t group) { return chown(path, owner, group); }
int chmod(const char *path, mode_t mode) { (void)path; (void)mode; return 0; }
int mknod(const char *path, mode_t mode, dev_t dev) { (void)path; (void)mode; (void)dev; errno = ENOSYS; return -1; }
int utimes(const char *path, const struct timeval times[2]) { (void)path; (void)times; return 0; }
int fnmatch(const char *pattern, const char *string, int flags) { (void)flags; return strcmp(pattern, string) == 0 ? 0 : FNM_NOMATCH; }
int glob(const char *pattern, int flags, int (*errfunc)(const char *epath, int eerrno), glob_t *pglob) { (void)pattern; (void)flags; (void)errfunc; if (pglob) { pglob->gl_pathc = 0; pglob->gl_pathv = 0; pglob->gl_offs = 0; } return GLOB_NOMATCH; }
void globfree(glob_t *pglob) { (void)pglob; }
int regcomp(regex_t *preg, const char *regex, int cflags) { (void)preg; (void)regex; (void)cflags; return 0; }
int regexec(const regex_t *preg, const char *string, unsigned long nmatch, regmatch_t pmatch[], int eflags) { (void)preg; (void)string; (void)nmatch; (void)pmatch; (void)eflags; return REG_NOMATCH; }
void regfree(regex_t *preg) { (void)preg; }
char *setlocale(int category, const char *locale) { (void)category; (void)locale; return "C"; }
unsigned short htons(unsigned short x) { return (unsigned short)((x << 8) | (x >> 8)); }
unsigned short ntohs(unsigned short x) { return htons(x); }
unsigned int htonl(unsigned int x) { return __builtin_bswap32(x); }
unsigned int ntohl(unsigned int x) { return htonl(x); }

__asm__(
    ".text\n"
    ".globl setjmp\n"
    "setjmp:\n"
    "  store32 r30, r0, 0\n"
    "  store32 r31, r0, 4\n"
    "  movi r1, 0xF0\n"
    "  in r2, r1\n"
    "  store32 r2, r0, 8\n"
    "  movi r1, 0xF4\n"
    "  in r3, r1\n"
    "  add r4, r2, r2\n"
    "  add r4, r4, r4\n"
    "  add r4, r4, r4\n"
    "  add r3, r3, r4\n"
    "  load32 r4, r3, 0\n"
    "  store32 r4, r0, 12\n"
    "  movi r0, 0\n"
    "  ret\n"
    ".globl longjmp\n"
    "longjmp:\n"
    "  load32 r30, r0, 0\n"
    "  load32 r31, r0, 4\n"
    "  load32 r2, r0, 8\n"
    "  load32 r3, r0, 12\n"
    "  movi r4, 0xF4\n"
    "  in r5, r4\n"
    "  add r6, r2, r2\n"
    "  add r6, r6, r6\n"
    "  add r6, r6, r6\n"
    "  add r5, r5, r6\n"
    "  store32 r3, r5, 0\n"
    "  movi r6, 0\n"
    "  store32 r6, r5, 4\n"
    "  mov r0, r1\n"
    "  movi r4, 0xF0\n"
    "  out r2, r4\n"
    "  ret\n"
);

char *optarg;
int optind = 1;
int opterr = 1;
int optopt;
static char *g_getopt_pos;

int getopt(int argc, char *const argv[], const char *optstring) {
    int c;
    if (optind == 0) {
        optind = 1;
        g_getopt_pos = 0;
    }
    optarg = 0;
    if (!g_getopt_pos || *g_getopt_pos == '\0') {
        if (optind >= argc || !argv[optind] || argv[optind][0] != '-' || argv[optind][1] == '\0') {
            return -1;
        }
        if (argv[optind][1] == '-' && argv[optind][2] == '\0') {
            optind++;
            return -1;
        }
        g_getopt_pos = argv[optind] + 1;
    }

    c = (unsigned char)*g_getopt_pos++;
    const char *spec = strchr(optstring, c);
    optopt = c;
    if (!spec || c == ':') {
        if (*g_getopt_pos == '\0') optind++;
        return '?';
    }
    if (spec[1] == ':') {
        if (*g_getopt_pos != '\0') {
            optarg = g_getopt_pos;
            optind++;
            g_getopt_pos = 0;
        } else if (optind + 1 < argc) {
            optarg = argv[++optind];
            optind++;
            g_getopt_pos = 0;
        } else {
            optind++;
            return optstring[0] == ':' ? ':' : '?';
        }
    } else if (*g_getopt_pos == '\0') {
        optind++;
        g_getopt_pos = 0;
    }
    return c;
}
int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex) {
    (void)longopts;
    if (longindex) *longindex = -1;
    return getopt(argc, argv, optstring);
}
int getopt_long_only(int argc, char *const argv[], const char *optstring,
                     const struct option *longopts, int *longindex) {
    return getopt_long(argc, argv, optstring, longopts, longindex);
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
    size_t lo = 0, hi = nmemb;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = compar(key, (const char *)base + mid * size);
        if (cmp < 0) hi = mid;
        else if (cmp > 0) lo = mid + 1;
        else return (void *)((const char *)base + mid * size);
    }
    return 0;
}
int setresuid(uid_t ruid, uid_t euid, uid_t suid) { (void)ruid; (void)euid; (void)suid; return 0; }
int setresgid(gid_t rgid, gid_t egid, gid_t sgid) { (void)rgid; (void)egid; (void)sgid; return 0; }
char *strsignal(int sig) { (void)sig; return "Unknown signal"; }
pid_t getpgrp(void) { return getpid(); }
int setpgid(pid_t pid, pid_t pgid) { (void)pid; (void)pgid; return 0; }
int setpgrp(void) { return 0; }
pid_t tcgetpgrp(int fd) { (void)fd; return getpid(); }
