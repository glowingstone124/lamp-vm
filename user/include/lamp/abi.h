#ifndef LAMP_USER_ABI_H
#define LAMP_USER_ABI_H

#include "types.h"

enum {
    LAMP_SYS_GETPID = 0u,
    LAMP_SYS_YIELD = 1u,
    LAMP_SYS_SLEEP_TICKS = 2u,
    LAMP_SYS_EXIT = 3u,
    LAMP_SYS_WAITPID = 4u,
    LAMP_SYS_NANOSLEEP = 5u,
    LAMP_SYS_READ = 6u,
    LAMP_SYS_WRITE = 7u,
    LAMP_SYS_POLL = 8u,
    LAMP_SYS_SELECT = 9u,
    LAMP_SYS_TTY_GETMODE = 10u,
    LAMP_SYS_TTY_SETMODE = 11u,
    LAMP_SYS_CLOCK_GETTIME = 12u,
    LAMP_SYS_GETTIMEOFDAY = 13u,
    LAMP_SYS_CLOCK_GETRES = 14u,
    LAMP_SYS_CLOCK_SETTIME = 15u,
    LAMP_SYS_CLOSE = 16u,
    LAMP_SYS_DUP = 17u,
    LAMP_SYS_DUP2 = 18u,
    LAMP_SYS_FCNTL = 19u,
    LAMP_SYS_OPEN = 20u,
    LAMP_SYS_SOCKET = 21u,
    LAMP_SYS_CONNECT = 22u,
    LAMP_SYS_BIND = 23u,
    LAMP_SYS_LISTEN = 24u,
    LAMP_SYS_ACCEPT = 25u,
    LAMP_SYS_SEND = 26u,
    LAMP_SYS_RECV = 27u,
    LAMP_SYS_EXECVE = 28u,
    LAMP_SYS_VFORK = 29u,
    LAMP_SYS_LSEEK = 30u,
    LAMP_SYS_GETPPID = 31u,
    LAMP_SYS_STAT = 32u,
    LAMP_SYS_FSTAT = 33u,
    LAMP_SYS_GETDENTS = 34u,
    LAMP_SYS_ACCESS = 35u,
    LAMP_SYS_CHDIR = 36u,
    LAMP_SYS_GETCWD = 37u,
    LAMP_SYS_PIPE = 38u,
    LAMP_SYS_IOCTL = 39u,
    LAMP_SYS_SIGACTION = 40u,
    LAMP_SYS_SIGPROCMASK = 41u,
    LAMP_SYS_KILL = 42u
};

enum {
    LAMP_ERRNO_OK = 0u,
    LAMP_ERRNO_EPERM = 1u,
    LAMP_ERRNO_ENOENT = 2u,
    LAMP_ERRNO_ESRCH = 3u,
    LAMP_ERRNO_EINTR = 4u,
    LAMP_ERRNO_EIO = 5u,
    LAMP_ERRNO_EBADF = 9u,
    LAMP_ERRNO_ECHILD = 10u,
    LAMP_ERRNO_EAGAIN = 11u,
    LAMP_ERRNO_EACCES = 13u,
    LAMP_ERRNO_ENOMEM = 12u,
    LAMP_ERRNO_EFAULT = 14u,
    LAMP_ERRNO_EBUSY = 16u,
    LAMP_ERRNO_ENOTDIR = 20u,
    LAMP_ERRNO_EISDIR = 21u,
    LAMP_ERRNO_EINVAL = 22u,
    LAMP_ERRNO_EMFILE = 24u,
    LAMP_ERRNO_ENOTTY = 25u,
    LAMP_ERRNO_ENOSPC = 28u,
    LAMP_ERRNO_ESPIPE = 29u,
    LAMP_ERRNO_EROFS = 30u,
    LAMP_ERRNO_EPIPE = 32u,
    LAMP_ERRNO_ERANGE = 34u,
    LAMP_ERRNO_ENAMETOOLONG = 36u,
    LAMP_ERRNO_ENOSYS = 38u,
    LAMP_ERRNO_EOVERFLOW = 75u,
    LAMP_ERRNO_ENOTSOCK = 88u,
    LAMP_ERRNO_EOPNOTSUPP = 95u,
    LAMP_ERRNO_EAFNOSUPPORT = 97u,
    LAMP_ERRNO_ENOTCONN = 107u
};

enum {
    LAMP_SEEK_SET = 0u,
    LAMP_SEEK_CUR = 1u,
    LAMP_SEEK_END = 2u
};

enum {
    LAMP_S_IFMT = 0xF000u,
    LAMP_S_IFSOCK = 0xC000u,
    LAMP_S_IFREG = 0x8000u,
    LAMP_S_IFDIR = 0x4000u,
    LAMP_S_IFCHR = 0x2000u,
    LAMP_S_IFIFO = 0x1000u
};

enum {
    LAMP_DT_UNKNOWN = 0u,
    LAMP_DT_REG = 8u,
    LAMP_DT_DIR = 4u,
    LAMP_DT_CHR = 2u,
    LAMP_DT_SOCK = 12u
};

enum {
    LAMP_F_OK = 0u,
    LAMP_X_OK = 1u,
    LAMP_W_OK = 2u,
    LAMP_R_OK = 4u
};

typedef struct lamp_stat {
    uint32_t st_dev;
    uint32_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t st_rdev;
    uint32_t st_size;
    uint32_t st_blksize;
    uint32_t st_blocks;
} lamp_stat_t;

typedef struct lamp_dirent {
    uint32_t d_ino;
    uint32_t d_off;
    uint32_t d_reclen;
    uint32_t d_type;
    char d_name[256];
} lamp_dirent_t;

enum {
    LAMP_IOCTL_TCGETS = 0x00005401u,
    LAMP_IOCTL_TCSETS = 0x00005402u,
    LAMP_IOCTL_TCSETSW = 0x00005403u,
    LAMP_IOCTL_TCSETSF = 0x00005404u,
    LAMP_IOCTL_TIOCGWINSZ = 0x00005413u
};

enum {
    LAMP_TERMIOS_ISIG = 0x00000001u,
    LAMP_TERMIOS_ICANON = 0x00000002u,
    LAMP_TERMIOS_ECHO = 0x00000008u,
    LAMP_TERMIOS_NCCS = 32u
};

typedef struct lamp_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_cc[LAMP_TERMIOS_NCCS];
} lamp_termios_t;

typedef struct lamp_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} lamp_winsize_t;

enum {
    LAMP_SIG_DFL = 0u,
    LAMP_SIG_IGN = 1u,
    LAMP_SIGHUP = 1u,
    LAMP_SIGINT = 2u,
    LAMP_SIGQUIT = 3u,
    LAMP_SIGKILL = 9u,
    LAMP_SIGTERM = 15u,
    LAMP_SIGCHLD = 17u,
    LAMP_SIGSTOP = 19u,
    LAMP_SIG_BLOCK = 0u,
    LAMP_SIG_UNBLOCK = 1u,
    LAMP_SIG_SETMASK = 2u
};

typedef struct lamp_sigaction {
    uint32_t handler;
    uint32_t flags;
    uint32_t mask;
    uint32_t restorer;
} lamp_sigaction_t;

enum {
    LAMP_IRQ_SYSCALL = 0x80u
};

enum {
    LAMP_SYSCALL_ABI_ADDR = 0x002FE000u,
    LAMP_SYSCALL_ABI_MAGIC = 0x30435953u,
    LAMP_SYSCALL_ABI_VERSION = 1u,
    LAMP_SYSCALL_ABI_OFF_MAGIC = 0x00u,
    LAMP_SYSCALL_ABI_OFF_VERSION = 0x04u,
    LAMP_SYSCALL_ABI_OFF_RET = 0x24u,
    LAMP_SYSCALL_ABI_OFF_ERRNO = 0x28u,
    LAMP_SYSCALL_ABI_WORDS = 12u
};

#endif
