#ifndef LAMP_KERNEL_SYSCALL_H
#define LAMP_KERNEL_SYSCALL_H

#include "types.h"

enum {
    SYS_GETPID = 0u,
    SYS_YIELD = 1u,
    SYS_SLEEP_TICKS = 2u,
    SYS_EXIT = 3u,
    SYS_WAITPID = 4u,
    SYS_NANOSLEEP = 5u,
    SYS_READ = 6u,
    SYS_WRITE = 7u,
    SYS_POLL = 8u,
    SYS_SELECT = 9u,
    SYS_TTY_GETMODE = 10u,
    SYS_TTY_SETMODE = 11u,
    SYS_CLOCK_GETTIME = 12u,
    SYS_GETTIMEOFDAY = 13u,
    SYS_CLOCK_GETRES = 14u,
    SYS_CLOCK_SETTIME = 15u,
    SYS_CLOSE = 16u,
    SYS_DUP = 17u,
    SYS_DUP2 = 18u,
    SYS_FCNTL = 19u,
    SYS_OPEN = 20u,
    SYS_SOCKET = 21u,
    SYS_CONNECT = 22u,
    SYS_BIND = 23u,
    SYS_LISTEN = 24u,
    SYS_ACCEPT = 25u,
    SYS_SEND = 26u,
    SYS_RECV = 27u,
    SYS_EXECVE = 28u,
    SYS_VFORK = 29u,
    SYS_LSEEK = 30u,
    SYS_GETPPID = 31u,
    SYS_STAT = 32u,
    SYS_FSTAT = 33u,
    SYS_GETDENTS = 34u,
    SYS_ACCESS = 35u,
    SYS_CHDIR = 36u,
    SYS_GETCWD = 37u,
    SYS_PIPE = 38u,
    SYS_IOCTL = 39u,
    SYS_SIGACTION = 40u,
    SYS_SIGPROCMASK = 41u,
    SYS_KILL = 42u,
    SYS_UMASK = 43u,
    SYS_RENAME = 44u,
    SYS_UNLINK = 45u,
    SYS_MKDIR = 46u,
    SYS_RMDIR = 47u,
    SYS_LINK = 48u,
    SYS_SYMLINK = 49u,
    SYS_READLINK = 50u
};

enum {
    SYS_WAITPID_WNOHANG = 1u,
    SYS_WAITPID_WUNTRACED = 2u,
    SYS_IO_NONBLOCK = 1u /* deprecated: use fcntl(F_SETFL, O_NONBLOCK) */
};

enum {
    SYS_POLLIN = 0x0001u,
    SYS_POLLOUT = 0x0004u,
    SYS_POLLERR = 0x0008u,
    SYS_POLLNVAL = 0x0020u
};

enum {
    SYS_CLOCK_REALTIME = 0u,
    SYS_CLOCK_MONOTONIC = 1u,
    SYS_CLOCK_BOOTTIME = 7u
};

enum {
    SYS_FCNTL_F_DUPFD = 0u,
    SYS_FCNTL_F_GETFD = 1u,
    SYS_FCNTL_F_SETFD = 2u,
    SYS_FCNTL_F_GETFL = 3u,
    SYS_FCNTL_F_SETFL = 4u,
    SYS_FCNTL_F_DUPFD_CLOEXEC = 5u
};

enum {
    SYS_O_ACCMODE = 0x00000003u,
    SYS_O_RDONLY = 0x00000000u,
    SYS_O_WRONLY = 0x00000001u,
    SYS_O_RDWR = 0x00000002u,
    SYS_O_CREAT = 0x00000040u,
    SYS_O_TRUNC = 0x00000200u,
    SYS_O_NONBLOCK = 0x00000800u,
    SYS_O_CLOEXEC = 0x00080000u
};

enum {
    SYS_FD_CLOEXEC = 0x00000001u
};

enum {
    SYS_SEEK_SET = 0u,
    SYS_SEEK_CUR = 1u,
    SYS_SEEK_END = 2u
};

enum {
    SYS_S_IFMT = 0xF000u,
    SYS_S_IFSOCK = 0xC000u,
    SYS_S_IFLNK = 0xA000u,
    SYS_S_IFREG = 0x8000u,
    SYS_S_IFDIR = 0x4000u,
    SYS_S_IFCHR = 0x2000u,
    SYS_S_IFIFO = 0x1000u
};

enum {
    SYS_DT_UNKNOWN = 0u,
    SYS_DT_REG = 8u,
    SYS_DT_DIR = 4u,
    SYS_DT_CHR = 2u,
    SYS_DT_LNK = 10u,
    SYS_DT_SOCK = 12u
};

enum {
    SYS_F_OK = 0u,
    SYS_X_OK = 1u,
    SYS_W_OK = 2u,
    SYS_R_OK = 4u
};

enum {
    SYS_IOCTL_TCGETS = 0x00005401u,
    SYS_IOCTL_TCSETS = 0x00005402u,
    SYS_IOCTL_TCSETSW = 0x00005403u,
    SYS_IOCTL_TCSETSF = 0x00005404u,
    SYS_IOCTL_TIOCGWINSZ = 0x00005413u
};

enum {
    SYS_TERMIOS_ISIG = 0x00000001u,
    SYS_TERMIOS_ICANON = 0x00000002u,
    SYS_TERMIOS_ECHO = 0x00000008u,
    SYS_TERMIOS_NCCS = 32u
};

typedef struct syscall_termios32 {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_cc[SYS_TERMIOS_NCCS];
} syscall_termios32_t;

typedef struct syscall_winsize32 {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} syscall_winsize32_t;

enum {
    SYS_SIG_DFL = 0u,
    SYS_SIG_IGN = 1u,
    SYS_SIGHUP = 1u,
    SYS_SIGINT = 2u,
    SYS_SIGQUIT = 3u,
    SYS_SIGKILL = 9u,
    SYS_SIGTERM = 15u,
    SYS_SIGCHLD = 17u,
    SYS_SIGSTOP = 19u,
    SYS_SIG_BLOCK = 0u,
    SYS_SIG_UNBLOCK = 1u,
    SYS_SIG_SETMASK = 2u
};

typedef struct syscall_sigaction32 {
    uint32_t handler;
    uint32_t flags;
    uint32_t mask;
    uint32_t restorer;
} syscall_sigaction32_t;

typedef struct syscall_regs {
    uint32_t nr;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t arg2;
    uint32_t arg3;
    uint32_t arg4;
    uint32_t arg5;
    uint32_t abi_addr;
} syscall_regs_t;

void syscall_init(void);
uint32_t syscall_dispatch(const syscall_regs_t *regs);

#endif
