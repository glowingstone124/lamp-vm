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
    SYS_VFORK = 29u
};

enum {
    SYS_WAITPID_WNOHANG = 1u,
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
    SYS_FCNTL_F_GETFD = 1u,
    SYS_FCNTL_F_SETFD = 2u,
    SYS_FCNTL_F_GETFL = 3u,
    SYS_FCNTL_F_SETFL = 4u
};

enum {
    SYS_O_ACCMODE = 0x00000003u,
    SYS_O_RDONLY = 0x00000000u,
    SYS_O_WRONLY = 0x00000001u,
    SYS_O_RDWR = 0x00000002u,
    SYS_O_CREAT = 0x00000040u,
    SYS_O_TRUNC = 0x00000200u,
    SYS_O_NONBLOCK = 0x00000800u
};

enum {
    SYS_FD_CLOEXEC = 0x00000001u
};

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
