#include "../include/kernel/blk.h"
#include "../include/kernel/console.h"
#include "../include/kernel/fs.h"
#include "../include/kernel/platform.h"
#include "../include/kernel/sched.h"
#include "../include/kernel/syscall.h"
#include "../include/kernel/user_exec.h"

/*
 * Current VM interrupt model restores caller registers on IRET.
 * So syscall return values are published via a fixed ABI mailbox in RAM.
 * Register return ABI can be added after low-level context plumbing changes.
 */
enum {
    SYSCALL_ABI_OFF_MAGIC = 0x00u,
    SYSCALL_ABI_OFF_VERSION = 0x04u,
    SYSCALL_ABI_OFF_LAST_NR = 0x08u,
    SYSCALL_ABI_OFF_ARG0 = 0x0Cu,
    SYSCALL_ABI_OFF_ARG1 = 0x10u,
    SYSCALL_ABI_OFF_ARG2 = 0x14u,
    SYSCALL_ABI_OFF_ARG3 = 0x18u,
    SYSCALL_ABI_OFF_ARG4 = 0x1Cu,
    SYSCALL_ABI_OFF_ARG5 = 0x20u,
    SYSCALL_ABI_OFF_RET = 0x24u,
    SYSCALL_ABI_OFF_ERRNO = 0x28u,
    SYSCALL_ABI_OFF_TICK = 0x2Cu
};

enum {
    ERRNO_ENOENT = 2u,
    ERRNO_OK = 0u,
    ERRNO_EINTR = 4u,
    ERRNO_EIO = 5u,
    ERRNO_EBADF = 9u,
    ERRNO_ECHILD = 10u,
    ERRNO_EAGAIN = 11u,
    ERRNO_ENOMEM = 12u,
    ERRNO_EFAULT = 14u,
    ERRNO_EBUSY = 16u,
    ERRNO_ENOTDIR = 20u,
    ERRNO_EISDIR = 21u,
    ERRNO_EMFILE = 24u,
    ERRNO_ENOSPC = 28u,
    ERRNO_EROFS = 30u,
    ERRNO_EINVAL = 22u,
    ERRNO_ENOTSOCK = 88u,
    ERRNO_EOPNOTSUPP = 95u,
    ERRNO_EAFNOSUPPORT = 97u,
    ERRNO_ENOTCONN = 107u,
    ERRNO_ENOSYS = 38u,
    ERRNO_EOVERFLOW = 75u
};

typedef struct syscall_timespec32 {
    int32_t tv_sec;
    int32_t tv_nsec;
} syscall_timespec32_t;

typedef struct syscall_pollfd32 {
    int32_t fd;
    int16_t events;
    int16_t revents;
} syscall_pollfd32_t;

typedef struct syscall_timeval32 {
    int32_t tv_sec;
    int32_t tv_usec;
} syscall_timeval32_t;

enum {
    NS_PER_SEC = 1000000000u,
    US_PER_SEC = 1000000u,
    NS_PER_US = 1000u,
    FS_PATH_CAP = 64u,
    CLOCK_RESOLUTION_NS = 1u,
    TIME_MMIO_REALTIME_LO = TIMER_MMIO_BASE + 0x04u,
    TIME_MMIO_REALTIME_HI = TIMER_MMIO_BASE + 0x08u,
    TIME_MMIO_MONOTONIC_LO = TIMER_MMIO_BASE + 0x0Cu,
    TIME_MMIO_MONOTONIC_HI = TIMER_MMIO_BASE + 0x10u,
    TIME_MMIO_BOOT_LO = TIMER_MMIO_BASE + 0x14u,
    TIME_MMIO_BOOT_HI = TIMER_MMIO_BASE + 0x18u,
    SOCK_AF_UNIX = 1u,
    SOCK_AF_INET = 2u,
    SOCK_AF_INET6 = 10u,
    SOCK_NONBLOCK = 0x00000800u,
    SYSCALL_EXEC_MAX_ARGV = 16u,
    SYSCALL_EXEC_MAX_ENVP = 16u
};

static uint32_t g_realtime_offset_neg;
static uint64_t g_realtime_offset_mag_ns;

static inline void abi_write32(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(uintptr_t)(SYSCALL_ABI_ADDR + off) = v;
}

static inline uint32_t abi_ptr_range_ok(uint32_t addr, uint32_t len) {
    if (addr == 0u || len == 0u) {
        return 0u;
    }
    if (addr >= KERNEL_MEM_SIZE) {
        return 0u;
    }
    return (len <= (KERNEL_MEM_SIZE - addr)) ? 1u : 0u;
}

static inline uint32_t abi_user_write32(uint32_t addr, uint32_t v) {
    if (!abi_ptr_range_ok(addr, 4u)) {
        return 0u;
    }
    *(volatile uint32_t *)(uintptr_t)addr = v;
    return 1u;
}

static inline uint32_t abi_user_read32(uint32_t addr, uint32_t *out) {
    if (!out || !abi_ptr_range_ok(addr, 4u)) {
        return 0u;
    }
    *out = *(volatile uint32_t *)(uintptr_t)addr;
    return 1u;
}

static inline uint32_t abi_user_write_bytes(uint32_t addr, const uint8_t *src, uint32_t len) {
    uint32_t i;
    if (!src || len == 0u) {
        return 1u;
    }
    if (!abi_ptr_range_ok(addr, len)) {
        return 0u;
    }
    for (i = 0u; i < len; i++) {
        *(volatile uint8_t *)(uintptr_t)(addr + i) = src[i];
    }
    return 1u;
}

static inline uint32_t abi_user_read_bytes(uint32_t addr, uint8_t *dst, uint32_t len) {
    uint32_t i;
    if (!dst || len == 0u) {
        return 1u;
    }
    if (!abi_ptr_range_ok(addr, len)) {
        return 0u;
    }
    for (i = 0u; i < len; i++) {
        dst[i] = *(volatile uint8_t *)(uintptr_t)(addr + i);
    }
    return 1u;
}

static inline uint32_t abi_user_read_cstr(uint32_t addr, char *dst, uint32_t cap) {
    uint32_t i;
    if (!dst || cap == 0u || addr == 0u) {
        return 0u;
    }
    for (i = 0u; i + 1u < cap; i++) {
        uint8_t c;
        if (!abi_ptr_range_ok(addr + i, 1u)) {
            return 0u;
        }
        c = *(volatile uint8_t *)(uintptr_t)(addr + i);
        dst[i] = (char)c;
        if (c == 0u) {
            return 1u;
        }
    }
    return 0u;
}

static inline uint32_t abi_user_read_timespec(uint32_t addr, syscall_timespec32_t *out) {
    if (!out || !abi_ptr_range_ok(addr, 8u)) {
        return 0u;
    }
    out->tv_sec = *(volatile int32_t *)(uintptr_t)(addr + 0u);
    out->tv_nsec = *(volatile int32_t *)(uintptr_t)(addr + 4u);
    return 1u;
}

static uint32_t abi_user_read_cstrv(uint32_t vec_addr,
                                    char storage[][FS_PATH_CAP],
                                    const char *out_vec[],
                                    uint32_t storage_cap) {
    uint32_t i;
    if (!out_vec) {
        return 0u;
    }
    if (vec_addr == 0u) {
        out_vec[0] = 0;
        return 1u;
    }
    if (!storage || storage_cap == 0u) {
        return 0u;
    }
    for (i = 0u; i < storage_cap; i++) {
        uint32_t p = 0u;
        if (!abi_user_read32(vec_addr + i * 4u, &p)) {
            return 0u;
        }
        if (p == 0u) {
            out_vec[i] = 0;
            return 1u;
        }
        if (!abi_user_read_cstr(p, &storage[i][0], FS_PATH_CAP)) {
            return 0u;
        }
        out_vec[i] = &storage[i][0];
    }
    return 0u;
}

static inline uint32_t abi_user_write_timespec(uint32_t addr, const syscall_timespec32_t *ts) {
    if (!ts || !abi_ptr_range_ok(addr, 8u)) {
        return 0u;
    }
    *(volatile uint32_t *)(uintptr_t)(addr + 0u) = (uint32_t)ts->tv_sec;
    *(volatile uint32_t *)(uintptr_t)(addr + 4u) = (uint32_t)ts->tv_nsec;
    return 1u;
}

static inline uint32_t abi_user_write_timespec_zero(uint32_t addr) {
    if (addr == 0u) {
        return 1u;
    }
    if (!abi_ptr_range_ok(addr, 8u)) {
        return 0u;
    }
    *(volatile uint32_t *)(uintptr_t)(addr + 0u) = 0u;
    *(volatile uint32_t *)(uintptr_t)(addr + 4u) = 0u;
    return 1u;
}

static inline uint32_t abi_user_write_timeval(uint32_t addr, const syscall_timeval32_t *tv) {
    if (!tv || !abi_ptr_range_ok(addr, 8u)) {
        return 0u;
    }
    *(volatile uint32_t *)(uintptr_t)(addr + 0u) = (uint32_t)tv->tv_sec;
    *(volatile uint32_t *)(uintptr_t)(addr + 4u) = (uint32_t)tv->tv_usec;
    return 1u;
}

static inline uint32_t div_ceil_u32(uint32_t numer, uint32_t denom) {
    if (denom == 0u) {
        return 0xFFFFFFFFu;
    }
    return (numer / denom) + ((numer % denom) ? 1u : 0u);
}

static inline uint32_t count_bits_u32(uint32_t v) {
    uint32_t n = 0u;
    while (v != 0u) {
        n += v & 1u;
        v >>= 1u;
    }
    return n;
}

static inline uint32_t fd_is_valid(int32_t fd) {
    return sched_fd_is_open(fd);
}

static inline uint32_t fd_type(int32_t fd) {
    uint32_t t = SCHED_FD_TYPE_NONE;
    if (sched_fd_get_type(fd, &t) != SCHED_FD_OK) {
        return SCHED_FD_TYPE_NONE;
    }
    return t;
}

static inline uint32_t fd_supports_read(int32_t fd) {
    return sched_fd_can_read(fd);
}

static inline uint32_t fd_supports_write(int32_t fd) {
    return sched_fd_can_write(fd);
}

static inline uint32_t fd_is_nonblock(int32_t fd) {
    return sched_fd_is_nonblock(fd);
}

static inline uint32_t fd_waits_console_read(int32_t fd) {
    uint32_t t = fd_type(fd);
    return (t == SCHED_FD_TYPE_STDIN || t == SCHED_FD_TYPE_DEV_TTY) ? 1u : 0u;
}

static inline uint32_t fd_read_ready(int32_t fd) {
    uint32_t t = fd_type(fd);
    if (!fd_supports_read(fd)) {
        return 0u;
    }
    if (t == SCHED_FD_TYPE_STDIN || t == SCHED_FD_TYPE_DEV_TTY) {
        return console_can_read();
    }
    if (t == SCHED_FD_TYPE_DEV_NULL || t == SCHED_FD_TYPE_DEV_ZERO) {
        return 1u;
    }
    if (t == SCHED_FD_TYPE_REGULAR) {
        return 1u;
    }
    return 0u;
}

static inline uint32_t fd_write_ready(int32_t fd) {
    uint32_t t = fd_type(fd);
    if (!fd_supports_write(fd)) {
        return 0u;
    }
    if (t == SCHED_FD_TYPE_SOCKET) {
        return 0u;
    }
    return 1u;
}

static uint32_t errno_from_sched_fd_rc(int rc) {
    if (rc == SCHED_FD_EBADF) {
        return ERRNO_EBADF;
    }
    if (rc == SCHED_FD_EMFILE) {
        return ERRNO_EMFILE;
    }
    if (rc == SCHED_FD_EINVAL) {
        return ERRNO_EINVAL;
    }
    return ERRNO_EINVAL;
}

static uint32_t errno_from_fs_rc(int rc) {
    if (rc == FS_ERR_BADF) {
        return ERRNO_EBADF;
    }
    if (rc == FS_ERR_NOENT) {
        return ERRNO_ENOENT;
    }
    if (rc == FS_ERR_INVAL) {
        return ERRNO_EINVAL;
    }
    if (rc == FS_ERR_BUSY) {
        return ERRNO_EBUSY;
    }
    if (rc == FS_ERR_NOSPC) {
        return ERRNO_ENOSPC;
    }
    if (rc == FS_ERR_NOSYS) {
        return ERRNO_ENOSYS;
    }
    if (rc == FS_ERR_ROFS) {
        return ERRNO_EROFS;
    }
    if (rc == FS_ERR_NOTDIR) {
        return ERRNO_ENOTDIR;
    }
    if (rc == FS_ERR_ISDIR) {
        return ERRNO_EISDIR;
    }
    if (rc == FS_ERR_IO || rc == BLK_ERR_IO) {
        return ERRNO_EIO;
    }
    return errno_from_sched_fd_rc(rc);
}

static inline uint64_t mmio_read_time_ns(uint32_t low_addr, uint32_t high_addr) {
    uint32_t lo = *(volatile uint32_t *)(uintptr_t)low_addr;
    uint32_t hi = *(volatile uint32_t *)(uintptr_t)high_addr;
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static uint32_t clockid_is_supported(int32_t clock_id) {
    switch (clock_id) {
        case (int32_t)SYS_CLOCK_REALTIME:
        case (int32_t)SYS_CLOCK_MONOTONIC:
        case 2:
        case (int32_t)SYS_CLOCK_BOOTTIME:
            return 1u;
        default:
            return 0u;
    }
}

static inline uint64_t clock_apply_realtime_offset(uint64_t raw_ns) {
    if (g_realtime_offset_neg == 0u) {
        const uint64_t max_u64 = ~(uint64_t)0u;
        if (raw_ns > max_u64 - g_realtime_offset_mag_ns) {
            return max_u64;
        }
        return raw_ns + g_realtime_offset_mag_ns;
    }
    if (raw_ns <= g_realtime_offset_mag_ns) {
        return 0u;
    }
    return raw_ns - g_realtime_offset_mag_ns;
}

static uint32_t clockid_read_time_ns(int32_t clock_id, uint64_t *out_ns) {
    if (!out_ns) {
        return 0u;
    }
    switch (clock_id) {
        case (int32_t)SYS_CLOCK_REALTIME: {
            uint64_t raw_ns = mmio_read_time_ns(TIME_MMIO_REALTIME_LO, TIME_MMIO_REALTIME_HI);
            *out_ns = clock_apply_realtime_offset(raw_ns);
            return 1u;
        }
        case (int32_t)SYS_CLOCK_MONOTONIC:
            *out_ns = mmio_read_time_ns(TIME_MMIO_MONOTONIC_LO, TIME_MMIO_MONOTONIC_HI);
            return 1u;
        case 2:
        case (int32_t)SYS_CLOCK_BOOTTIME:
            *out_ns = mmio_read_time_ns(TIME_MMIO_BOOT_LO, TIME_MMIO_BOOT_HI);
            return 1u;
        default:
            return 0u;
    }
}

static uint32_t time_ns_to_timespec(uint64_t ns, syscall_timespec32_t *ts_out) {
    uint64_t sec64;
    if (!ts_out) {
        return 0u;
    }
    sec64 = ns / (uint64_t)NS_PER_SEC;
    if (sec64 > 0x7FFFFFFFu) {
        return 0u;
    }
    ts_out->tv_sec = (int32_t)sec64;
    ts_out->tv_nsec = (int32_t)(ns % (uint64_t)NS_PER_SEC);
    return 1u;
}

static uint32_t time_ns_to_timeval(uint64_t ns, syscall_timeval32_t *tv_out) {
    uint64_t sec64;
    if (!tv_out) {
        return 0u;
    }
    sec64 = ns / (uint64_t)NS_PER_SEC;
    if (sec64 > 0x7FFFFFFFu) {
        return 0u;
    }
    tv_out->tv_sec = (int32_t)sec64;
    tv_out->tv_usec = (int32_t)((ns % (uint64_t)NS_PER_SEC) / (uint64_t)NS_PER_US);
    return 1u;
}

static uint32_t timespec_to_ns(const syscall_timespec32_t *ts, uint64_t *out_ns) {
    uint64_t sec_ns;
    if (!ts || !out_ns) {
        return 0u;
    }
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= (int32_t)NS_PER_SEC) {
        return 0u;
    }
    sec_ns = (uint64_t)(uint32_t)ts->tv_sec * (uint64_t)NS_PER_SEC;
    *out_ns = sec_ns + (uint64_t)(uint32_t)ts->tv_nsec;
    return 1u;
}

static uint32_t timeout_ms_to_ticks(int32_t timeout_ms) {
    uint32_t tick_us;
    uint32_t timeout_us;
    if (timeout_ms <= 0) {
        return 0u;
    }
    tick_us = sched_tick_period_us();
    if (tick_us == 0u) {
        return 1u;
    }
    if ((uint32_t)timeout_ms > (0xFFFFFFFFu / NS_PER_US)) {
        timeout_us = 0xFFFFFFFFu;
    } else {
        timeout_us = (uint32_t)timeout_ms * NS_PER_US;
    }
    return div_ceil_u32(timeout_us, tick_us);
}

#define SYSCALL_PUBLISH_RESULT(regs_, ret_, err_)                                \
    do {                                                                         \
        const syscall_regs_t *const __regs = (regs_);                           \
        abi_write32(SYSCALL_ABI_OFF_MAGIC, SYSCALL_ABI_MAGIC);                  \
        abi_write32(SYSCALL_ABI_OFF_VERSION, SYSCALL_ABI_VERSION);              \
        abi_write32(SYSCALL_ABI_OFF_LAST_NR, __regs ? __regs->nr : 0u);         \
        abi_write32(SYSCALL_ABI_OFF_ARG0, __regs ? __regs->arg0 : 0u);          \
        abi_write32(SYSCALL_ABI_OFF_ARG1, __regs ? __regs->arg1 : 0u);          \
        abi_write32(SYSCALL_ABI_OFF_ARG2, __regs ? __regs->arg2 : 0u);          \
        abi_write32(SYSCALL_ABI_OFF_ARG3, __regs ? __regs->arg3 : 0u);          \
        abi_write32(SYSCALL_ABI_OFF_ARG4, __regs ? __regs->arg4 : 0u);          \
        abi_write32(SYSCALL_ABI_OFF_ARG5, __regs ? __regs->arg5 : 0u);          \
        abi_write32(SYSCALL_ABI_OFF_RET, (ret_));                               \
        abi_write32(SYSCALL_ABI_OFF_ERRNO, (err_));                             \
        abi_write32(SYSCALL_ABI_OFF_TICK, sched_ticks());                       \
    } while (0)

void syscall_init(void) {
    g_realtime_offset_neg = 0u;
    g_realtime_offset_mag_ns = 0u;
    abi_write32(SYSCALL_ABI_OFF_MAGIC, SYSCALL_ABI_MAGIC);
    abi_write32(SYSCALL_ABI_OFF_VERSION, SYSCALL_ABI_VERSION);
    abi_write32(SYSCALL_ABI_OFF_LAST_NR, 0u);
    abi_write32(SYSCALL_ABI_OFF_ARG0, 0u);
    abi_write32(SYSCALL_ABI_OFF_ARG1, 0u);
    abi_write32(SYSCALL_ABI_OFF_ARG2, 0u);
    abi_write32(SYSCALL_ABI_OFF_ARG3, 0u);
    abi_write32(SYSCALL_ABI_OFF_ARG4, 0u);
    abi_write32(SYSCALL_ABI_OFF_ARG5, 0u);
    abi_write32(SYSCALL_ABI_OFF_RET, 0u);
    abi_write32(SYSCALL_ABI_OFF_ERRNO, 0u);
    abi_write32(SYSCALL_ABI_OFF_TICK, 0u);
}

uint32_t syscall_dispatch(const syscall_regs_t *regs) {
    uint32_t ret = 0u;
    uint32_t err = ERRNO_OK;
    if (!regs) {
        SYSCALL_PUBLISH_RESULT(0, (uint32_t)-1, ERRNO_ENOSYS);
        return (uint32_t)-1;
    }

    switch (regs->nr) {
        case SYS_GETPID:
            ret = (uint32_t)sched_current_tid();
            break;
        case SYS_YIELD:
            sched_yield();
            ret = 0u;
            break;
        case SYS_SLEEP_TICKS:
            sched_sleep_ticks(regs->arg0);
            ret = 0u;
            break;
        case SYS_EXIT:
            sched_exit_code(regs->arg0);
            ret = 0u;
            break;
        case SYS_WAITPID: {
            int32_t pid = (int32_t)regs->arg0;
            uint32_t status_addr = regs->arg1;
            uint32_t options = regs->arg2;
            uint32_t status = 0u;
            int rc;

            /*
             * POSIX waitpid(pid=0): wait any child in caller's process group.
             * Current scheduler has no process groups; map pid=0 to "any child".
             */
            if (pid == 0) {
                pid = SCHED_WAITPID_ANY;
            }
            if (pid < SCHED_WAITPID_ANY) {
                err = ERRNO_EINVAL;
                ret = (uint32_t)-1;
                break;
            }
            if ((options & ~SYS_WAITPID_WNOHANG) != 0u) {
                err = ERRNO_EINVAL;
                ret = (uint32_t)-1;
                break;
            }

            for (;;) {
                rc = sched_waitpid(pid, (options & SYS_WAITPID_WNOHANG) ? SCHED_WAITPID_WNOHANG : 0u, &status);
                if (rc > 0) {
                    if (status_addr != 0u && !abi_user_write32(status_addr, status)) {
                        err = ERRNO_EFAULT;
                        ret = (uint32_t)-1;
                        break;
                    }
                    ret = (uint32_t)rc;
                    break;
                }
                if (rc == 0) {
                    ret = 0u;
                    break;
                }
                if (rc == SCHED_WAITPID_BLOCKED) {
                    /* blocking path parks task and resumes here on wakeup */
                    continue;
                }
                if (rc == SCHED_WAITPID_NO_CHILD) {
                    err = ERRNO_ECHILD;
                    ret = (uint32_t)-1;
                    break;
                }
                err = ERRNO_ECHILD;
                ret = (uint32_t)-1;
                break;
            }
            break;
        }
        case SYS_NANOSLEEP: {
            syscall_timespec32_t req;
            uint32_t req_addr = regs->arg0;
            uint32_t rem_addr = regs->arg1;
            uint32_t total_us;
            uint32_t tick_us;
            uint32_t req_ns_us;
            uint32_t ticks;

            if (!abi_user_read_timespec(req_addr, &req)) {
                err = ERRNO_EFAULT;
                ret = (uint32_t)-1;
                break;
            }
            if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= (int32_t)NS_PER_SEC) {
                err = ERRNO_EINVAL;
                ret = (uint32_t)-1;
                break;
            }

            if (!abi_user_write_timespec_zero(rem_addr)) {
                err = ERRNO_EFAULT;
                ret = (uint32_t)-1;
                break;
            }

            if (req.tv_sec == 0 && req.tv_nsec == 0) {
                ret = 0u;
                break;
            }

            tick_us = sched_tick_period_us();
            if (tick_us == 0u) {
                err = ERRNO_EINTR;
                ret = (uint32_t)-1;
                break;
            }

            /*
             * Convert to microseconds using ceil for sub-us nanos.
             * Keep the whole path in 32-bit math to avoid __udivdi3 dependency.
             */
            if ((uint32_t)req.tv_sec > (0xFFFFFFFFu / US_PER_SEC)) {
                total_us = 0xFFFFFFFFu;
            } else {
                total_us = (uint32_t)req.tv_sec * US_PER_SEC;
                req_ns_us = div_ceil_u32((uint32_t)req.tv_nsec, NS_PER_US);
                if (total_us > 0xFFFFFFFFu - req_ns_us) {
                    total_us = 0xFFFFFFFFu;
                } else {
                    total_us += req_ns_us;
                }
            }

            ticks = div_ceil_u32(total_us, tick_us);
            if (ticks == 0u) {
                ticks = 1u;
            }
            sched_sleep_ticks(ticks);
            ret = 0u;
            break;
        }
        case SYS_READ: {
            int32_t fd = (int32_t)regs->arg0;
            uint32_t buf_addr = regs->arg1;
            uint32_t len = regs->arg2;
            uint32_t flags = regs->arg3;
            uint8_t byte_buf[64];
            uint32_t copied = 0u;
            uint32_t nonblock = fd_is_nonblock(fd) | ((flags & SYS_IO_NONBLOCK) ? 1u : 0u);
            uint32_t t = fd_type(fd);
            uint32_t i;

            if (!fd_supports_read(fd)) {
                err = ERRNO_EBADF;
                ret = (uint32_t)-1;
                break;
            }
            if (len == 0u) {
                ret = 0u;
                break;
            }
            if (buf_addr == 0u || !abi_ptr_range_ok(buf_addr, len)) {
                err = ERRNO_EFAULT;
                ret = (uint32_t)-1;
                break;
            }

            if (t == SCHED_FD_TYPE_DEV_NULL) {
                ret = 0u;
                break;
            }
            if (t == SCHED_FD_TYPE_DEV_ZERO) {
                while (copied < len) {
                    uint32_t remain = len - copied;
                    uint32_t want = (remain > (uint32_t)sizeof(byte_buf)) ? (uint32_t)sizeof(byte_buf) : remain;
                    for (i = 0u; i < want; i++) {
                        byte_buf[i] = 0u;
                    }
                    if (!abi_user_write_bytes(buf_addr + copied, byte_buf, want)) {
                        err = ERRNO_EFAULT;
                        ret = (uint32_t)-1;
                        break;
                    }
                    copied += want;
                }
                if (err == ERRNO_OK) {
                    ret = copied;
                }
                break;
            }
            if (t == SCHED_FD_TYPE_SOCKET) {
                err = ERRNO_ENOTCONN;
                ret = (uint32_t)-1;
                break;
            }
            if (t == SCHED_FD_TYPE_REGULAR) {
                while (copied < len) {
                    uint32_t remain = len - copied;
                    uint32_t want = (remain > (uint32_t)sizeof(byte_buf)) ? (uint32_t)sizeof(byte_buf) : remain;
                    int n = fs_read(fd, byte_buf, want);
                    if (n < 0) {
                        if (copied != 0u) {
                            ret = copied;
                        } else {
                            err = errno_from_fs_rc(n);
                            ret = (uint32_t)-1;
                        }
                        break;
                    }
                    if (n == 0) {
                        ret = copied;
                        break;
                    }
                    if (!abi_user_write_bytes(buf_addr + copied, byte_buf, (uint32_t)n)) {
                        err = ERRNO_EFAULT;
                        ret = (uint32_t)-1;
                        break;
                    }
                    copied += (uint32_t)n;
                    ret = copied;
                    if ((uint32_t)n < want) {
                        break;
                    }
                }
                break;
            }

            while (copied < len) {
                uint32_t remain = len - copied;
                uint32_t want = (remain > (uint32_t)sizeof(byte_buf)) ? (uint32_t)sizeof(byte_buf) : remain;
                /*
                 * If some bytes are already copied, do not block for the next chunk.
                 * This preserves short-read behavior instead of potentially sleeping.
                 */
                int n = console_read(byte_buf, want, (copied != 0u) ? 1u : nonblock);
                if (n == CONSOLE_IO_BLOCKED) {
                    if (copied != 0u) {
                        ret = copied;
                        break;
                    }
                    if (nonblock) {
                        err = ERRNO_EAGAIN;
                        ret = (uint32_t)-1;
                        break;
                    }
                    sched_block_until_runnable();
                    continue;
                }
                if (n == 0) {
                    if (copied != 0u) {
                        ret = copied;
                    } else if (nonblock) {
                        err = ERRNO_EAGAIN;
                        ret = (uint32_t)-1;
                    } else {
                        ret = 0u;
                    }
                    break;
                }
                if (!abi_user_write_bytes(buf_addr + copied, byte_buf, (uint32_t)n)) {
                    err = ERRNO_EFAULT;
                    ret = (uint32_t)-1;
                    break;
                }
                copied += (uint32_t)n;
                ret = copied;
                if ((uint32_t)n < want) {
                    break;
                }
            }
            break;
        }
        case SYS_WRITE: {
            int32_t fd = (int32_t)regs->arg0;
            uint32_t buf_addr = regs->arg1;
            uint32_t len = regs->arg2;
            uint8_t byte_buf[128];
            uint32_t total = 0u;
            uint32_t t = fd_type(fd);

            if (!fd_supports_write(fd)) {
                err = ERRNO_EBADF;
                ret = (uint32_t)-1;
                break;
            }
            if (len == 0u) {
                ret = 0u;
                break;
            }
            if (buf_addr == 0u || !abi_ptr_range_ok(buf_addr, len)) {
                err = ERRNO_EFAULT;
                ret = (uint32_t)-1;
                break;
            }

            if (t == SCHED_FD_TYPE_DEV_NULL || t == SCHED_FD_TYPE_DEV_ZERO) {
                ret = len;
                break;
            }
            if (t == SCHED_FD_TYPE_SOCKET) {
                err = ERRNO_ENOTCONN;
                ret = (uint32_t)-1;
                break;
            }
            if (t == SCHED_FD_TYPE_REGULAR) {
                while (total < len) {
                    uint32_t remain = len - total;
                    uint32_t want = (remain > (uint32_t)sizeof(byte_buf)) ? (uint32_t)sizeof(byte_buf) : remain;
                    int n;

                    if (!abi_user_read_bytes(buf_addr + total, byte_buf, want)) {
                        err = ERRNO_EFAULT;
                        ret = (uint32_t)-1;
                        break;
                    }
                    n = fs_write(fd, byte_buf, want);
                    if (n < 0) {
                        if (total != 0u) {
                            ret = total;
                        } else {
                            err = errno_from_fs_rc(n);
                            ret = (uint32_t)-1;
                        }
                        break;
                    }
                    total += (uint32_t)n;
                    ret = total;
                    if ((uint32_t)n < want) {
                        break;
                    }
                }
                break;
            }

            while (total < len) {
                uint32_t remain = len - total;
                uint32_t want = (remain > (uint32_t)sizeof(byte_buf)) ? (uint32_t)sizeof(byte_buf) : remain;
                uint32_t written;

                if (!abi_user_read_bytes(buf_addr + total, byte_buf, want)) {
                    err = ERRNO_EFAULT;
                    ret = (uint32_t)-1;
                    break;
                }
                written = console_write(byte_buf, want);
                total += written;
                ret = total;
                if (written < want) {
                    break;
                }
            }
            break;
        }
        case SYS_CLOSE: {
            int rc = sched_fd_close((int32_t)regs->arg0);
            if (rc != SCHED_FD_OK) {
                err = errno_from_sched_fd_rc(rc);
                ret = (uint32_t)-1;
                break;
            }
            ret = 0u;
            break;
        }
        case SYS_DUP: {
            int rc = sched_fd_dup((int32_t)regs->arg0);
            if (rc < 0) {
                err = errno_from_sched_fd_rc(rc);
                ret = (uint32_t)-1;
                break;
            }
            ret = (uint32_t)rc;
            break;
        }
        case SYS_DUP2: {
            int rc = sched_fd_dup2((int32_t)regs->arg0, (int32_t)regs->arg1);
            if (rc < 0) {
                err = errno_from_sched_fd_rc(rc);
                ret = (uint32_t)-1;
                break;
            }
            ret = (uint32_t)rc;
            break;
        }
        case SYS_FCNTL: {
            int32_t fd = (int32_t)regs->arg0;
            uint32_t cmd = regs->arg1;
            uint32_t arg = regs->arg2;
            int rc;
            switch (cmd) {
                case SYS_FCNTL_F_GETFD: {
                    uint32_t flags = 0u;
                    rc = sched_fd_fcntl_getfd(fd, &flags);
                    if (rc != SCHED_FD_OK) {
                        err = errno_from_sched_fd_rc(rc);
                        ret = (uint32_t)-1;
                        break;
                    }
                    ret = flags;
                    break;
                }
                case SYS_FCNTL_F_SETFD:
                    rc = sched_fd_fcntl_setfd(fd, arg);
                    if (rc != SCHED_FD_OK) {
                        err = errno_from_sched_fd_rc(rc);
                        ret = (uint32_t)-1;
                        break;
                    }
                    ret = 0u;
                    break;
                case SYS_FCNTL_F_GETFL: {
                    uint32_t flags = 0u;
                    rc = sched_fd_fcntl_getfl(fd, &flags);
                    if (rc != SCHED_FD_OK) {
                        err = errno_from_sched_fd_rc(rc);
                        ret = (uint32_t)-1;
                        break;
                    }
                    ret = flags;
                    break;
                }
                case SYS_FCNTL_F_SETFL:
                    rc = sched_fd_fcntl_setfl(fd, arg);
                    if (rc != SCHED_FD_OK) {
                        err = errno_from_sched_fd_rc(rc);
                        ret = (uint32_t)-1;
                        break;
                    }
                    ret = 0u;
                    break;
                default:
                    err = ERRNO_EINVAL;
                    ret = (uint32_t)-1;
                    break;
            }
            break;
        }
        case SYS_OPEN: {
            uint32_t path_addr = regs->arg0;
            uint32_t flags = regs->arg1;
            uint32_t accmode = flags & SYS_O_ACCMODE;
            int rc;
            char path[FS_PATH_CAP];

            if ((flags & ~(SYS_O_ACCMODE | SYS_O_NONBLOCK | SYS_O_CREAT | SYS_O_TRUNC)) != 0u) {
                err = ERRNO_EINVAL;
                ret = (uint32_t)-1;
                break;
            }
            if (!(accmode == SYS_O_RDONLY || accmode == SYS_O_WRONLY || accmode == SYS_O_RDWR)) {
                err = ERRNO_EINVAL;
                ret = (uint32_t)-1;
                break;
            }
            if (!abi_user_read_cstr(path_addr, path, (uint32_t)sizeof(path))) {
                err = ERRNO_EFAULT;
                ret = (uint32_t)-1;
                break;
            }
            rc = fs_open(path, flags);
            if (rc < 0) {
                err = errno_from_fs_rc(rc);
                ret = (uint32_t)-1;
                break;
            }
            ret = (uint32_t)rc;
            break;
        }
        case SYS_SOCKET: {
            uint32_t domain = regs->arg0;
            uint32_t type = regs->arg1;
            uint32_t status_flags = SCHED_FD_O_RDWR;
            uint32_t sock_kind = type & 0x0Fu;
            int rc;

            (void)regs->arg2; /* protocol: currently ignored in kernel stub */
            if (!(domain == SOCK_AF_UNIX || domain == SOCK_AF_INET || domain == SOCK_AF_INET6)) {
                err = ERRNO_EAFNOSUPPORT;
                ret = (uint32_t)-1;
                break;
            }
            if (sock_kind == 0u) {
                err = ERRNO_EINVAL;
                ret = (uint32_t)-1;
                break;
            }
            if ((type & SOCK_NONBLOCK) != 0u) {
                status_flags |= SCHED_FD_O_NONBLOCK;
            }
            rc = sched_fd_open_special(SCHED_FD_SPECIAL_SOCKET, status_flags);
            if (rc < 0) {
                err = errno_from_sched_fd_rc(rc);
                ret = (uint32_t)-1;
                break;
            }
            ret = (uint32_t)rc;
            break;
        }
        case SYS_EXECVE: {
            uint32_t path_addr = regs->arg0;
            uint32_t argv_addr = regs->arg1;
            uint32_t envp_addr = regs->arg2;
            char path[FS_PATH_CAP];
            char argv_storage[SYSCALL_EXEC_MAX_ARGV][FS_PATH_CAP];
            char envp_storage[SYSCALL_EXEC_MAX_ENVP][FS_PATH_CAP];
            const char *argv_vec[SYSCALL_EXEC_MAX_ARGV + 1u];
            const char *envp_vec[SYSCALL_EXEC_MAX_ENVP + 1u];
            int rc;

            if (!abi_user_read_cstr(path_addr, path, (uint32_t)sizeof(path))) {
                err = ERRNO_EFAULT;
                ret = (uint32_t)-1;
                break;
            }
            if (!abi_user_read_cstrv(argv_addr, argv_storage, argv_vec, SYSCALL_EXEC_MAX_ARGV)) {
                err = ERRNO_EFAULT;
                ret = (uint32_t)-1;
                break;
            }
            if (!abi_user_read_cstrv(envp_addr, envp_storage, envp_vec, SYSCALL_EXEC_MAX_ENVP)) {
                err = ERRNO_EFAULT;
                ret = (uint32_t)-1;
                break;
            }
            if (argv_vec[0] == 0) {
                argv_storage[0][0] = '\0';
                for (uint32_t i = 0u; path[i] != '\0' && i + 1u < FS_PATH_CAP; i++) {
                    argv_storage[0][i] = path[i];
                    argv_storage[0][i + 1u] = '\0';
                }
                argv_vec[0] = &argv_storage[0][0];
                argv_vec[1] = 0;
            }

            rc = user_exec_execve_current(path, argv_vec, envp_vec);
            err = errno_from_fs_rc(rc);
            ret = (uint32_t)-1;
            break;
        }
        case SYS_VFORK: {
            int rc = sched_vfork();
            if (rc < 0) {
                err = ERRNO_EAGAIN;
                ret = (uint32_t)-1;
                break;
            }
            ret = (uint32_t)rc;
            break;
        }
        case SYS_CONNECT:
        case SYS_BIND:
        case SYS_LISTEN:
        case SYS_ACCEPT:
        case SYS_SEND:
        case SYS_RECV: {
            int32_t fd = (int32_t)regs->arg0;
            if (!fd_is_valid(fd)) {
                err = ERRNO_EBADF;
                ret = (uint32_t)-1;
                break;
            }
            if (fd_type(fd) != SCHED_FD_TYPE_SOCKET) {
                err = ERRNO_ENOTSOCK;
                ret = (uint32_t)-1;
                break;
            }

            if (regs->nr == SYS_SEND || regs->nr == SYS_RECV) {
                err = ERRNO_ENOTCONN;
            } else {
                err = ERRNO_EOPNOTSUPP;
            }
            ret = (uint32_t)-1;
            break;
        }
        case SYS_POLL: {
            uint32_t fds_addr = regs->arg0;
            uint32_t nfds = regs->arg1;
            int32_t timeout_ms = (int32_t)regs->arg2;
            uint32_t timeout_ticks = timeout_ms_to_ticks(timeout_ms);
            uint32_t start_tick = sched_ticks();
            uint32_t i;

            if (nfds == 0u) {
                if (timeout_ms == 0) {
                    ret = 0u;
                    break;
                }
                for (;;) {
                    if (timeout_ms > 0) {
                        uint32_t elapsed = sched_ticks() - start_tick;
                        uint32_t remaining = (elapsed >= timeout_ticks) ? 0u : (timeout_ticks - elapsed);
                        if (remaining == 0u) {
                            ret = 0u;
                            break;
                        }
                        sched_sleep_ticks(remaining);
                    } else {
                        sched_sleep_ticks(1u);
                    }
                    sched_block_until_runnable();
                }
                break;
            }
            if (nfds > (KERNEL_MEM_SIZE / (uint32_t)sizeof(syscall_pollfd32_t))) {
                err = ERRNO_EINVAL;
                ret = (uint32_t)-1;
                break;
            }
            if (fds_addr == 0u || !abi_ptr_range_ok(fds_addr, nfds * (uint32_t)sizeof(syscall_pollfd32_t))) {
                err = ERRNO_EFAULT;
                ret = (uint32_t)-1;
                break;
            }

            for (;;) {
                uint32_t ready = 0u;
                uint32_t wait_read_stdin = 0u;
                uint32_t remaining_ticks = 0u;

                for (i = 0u; i < nfds; i++) {
                    syscall_pollfd32_t pfd;
                    uint16_t revents = 0u;
                    uint32_t off = fds_addr + i * (uint32_t)sizeof(syscall_pollfd32_t);

                    if (!abi_user_read_bytes(off, (uint8_t *)&pfd, (uint32_t)sizeof(pfd))) {
                        err = ERRNO_EFAULT;
                        ret = (uint32_t)-1;
                        break;
                    }

                    if (pfd.fd < 0) {
                        /* POSIX poll: negative fd entries are ignored. */
                    } else if (!fd_is_valid(pfd.fd)) {
                        revents |= (uint16_t)SYS_POLLNVAL;
                    } else {
                        if ((pfd.events & (int16_t)SYS_POLLIN) != 0 && fd_read_ready(pfd.fd)) {
                            revents |= (uint16_t)SYS_POLLIN;
                        }
                        if ((pfd.events & (int16_t)SYS_POLLOUT) != 0 && fd_write_ready(pfd.fd)) {
                            revents |= (uint16_t)SYS_POLLOUT;
                        }
                        if (fd_waits_console_read(pfd.fd) && (pfd.events & (int16_t)SYS_POLLIN) != 0 &&
                            !fd_read_ready(pfd.fd)) {
                            wait_read_stdin = 1u;
                        }
                    }

                    pfd.revents = (int16_t)revents;
                    if (!abi_user_write_bytes(off, (const uint8_t *)&pfd, (uint32_t)sizeof(pfd))) {
                        err = ERRNO_EFAULT;
                        ret = (uint32_t)-1;
                        break;
                    }
                    if (revents != 0u) {
                        ready++;
                    }
                }
                if (err != ERRNO_OK) {
                    break;
                }
                if (ready != 0u) {
                    ret = ready;
                    break;
                }
                if (timeout_ms == 0) {
                    ret = 0u;
                    break;
                }

                if (timeout_ms > 0) {
                    uint32_t elapsed = sched_ticks() - start_tick;
                    if (elapsed >= timeout_ticks) {
                        ret = 0u;
                        break;
                    }
                    remaining_ticks = timeout_ticks - elapsed;
                }

                if (wait_read_stdin) {
                    uint32_t wait_ticks = (timeout_ms > 0) ? remaining_ticks : 0u;
                    if (console_wait_readable(wait_ticks, 0u) == CONSOLE_IO_BLOCKED) {
                        sched_block_until_runnable();
                    }
                    continue;
                }

                if (timeout_ms < 0) {
                    sched_sleep_ticks(1u);
                } else {
                    sched_sleep_ticks(remaining_ticks ? remaining_ticks : 1u);
                }
                sched_block_until_runnable();
                continue;
            }
            break;
        }
        case SYS_SELECT: {
            uint32_t nfds = regs->arg0;
            uint32_t read_addr = regs->arg1;
            uint32_t write_addr = regs->arg2;
            uint32_t except_addr = regs->arg3;
            int32_t timeout_ms = (int32_t)regs->arg4;
            uint32_t timeout_ticks = timeout_ms_to_ticks(timeout_ms);
            uint32_t start_tick = sched_ticks();
            uint32_t in_read = 0u;
            uint32_t in_write = 0u;
            uint32_t in_except = 0u;
            uint32_t i;

            if (nfds > 32u) {
                err = ERRNO_EINVAL;
                ret = (uint32_t)-1;
                break;
            }
            if (read_addr != 0u) {
                if (!abi_ptr_range_ok(read_addr, 4u)) {
                    err = ERRNO_EFAULT;
                    ret = (uint32_t)-1;
                    break;
                }
                in_read = *(volatile uint32_t *)(uintptr_t)read_addr;
            }
            if (write_addr != 0u) {
                if (!abi_ptr_range_ok(write_addr, 4u)) {
                    err = ERRNO_EFAULT;
                    ret = (uint32_t)-1;
                    break;
                }
                in_write = *(volatile uint32_t *)(uintptr_t)write_addr;
            }
            if (except_addr != 0u) {
                if (!abi_ptr_range_ok(except_addr, 4u)) {
                    err = ERRNO_EFAULT;
                    ret = (uint32_t)-1;
                    break;
                }
                in_except = *(volatile uint32_t *)(uintptr_t)except_addr;
            }

            for (;;) {
                uint32_t out_read = 0u;
                uint32_t out_write = 0u;
                uint32_t out_except = 0u;
                uint32_t wait_read_stdin = 0u;
                uint32_t ready;
                uint32_t remaining_ticks = 0u;

                for (i = 0u; i < nfds; i++) {
                    uint32_t bit = 1u << i;
                    int32_t fd = (int32_t)i;
                    uint32_t watched = (in_read & bit) | (in_write & bit) | (in_except & bit);
                    if (watched == 0u) {
                        continue;
                    }
                    if (!fd_is_valid(fd)) {
                        err = ERRNO_EBADF;
                        ret = (uint32_t)-1;
                        break;
                    }
                    if ((in_read & bit) != 0u) {
                        if (fd_read_ready(fd)) {
                            out_read |= bit;
                        }
                        if (fd_waits_console_read(fd) && !fd_read_ready(fd)) {
                            wait_read_stdin = 1u;
                        }
                    }
                    if ((in_write & bit) != 0u && fd_write_ready(fd)) {
                        out_write |= bit;
                    }
                }
                if (err != ERRNO_OK) {
                    break;
                }

                if (read_addr != 0u && !abi_user_write32(read_addr, out_read)) {
                    err = ERRNO_EFAULT;
                    ret = (uint32_t)-1;
                    break;
                }
                if (write_addr != 0u && !abi_user_write32(write_addr, out_write)) {
                    err = ERRNO_EFAULT;
                    ret = (uint32_t)-1;
                    break;
                }
                if (except_addr != 0u && !abi_user_write32(except_addr, out_except)) {
                    err = ERRNO_EFAULT;
                    ret = (uint32_t)-1;
                    break;
                }

                ready = count_bits_u32(out_read) + count_bits_u32(out_write) + count_bits_u32(out_except);
                if (ready != 0u) {
                    ret = ready;
                    break;
                }
                if (timeout_ms == 0) {
                    ret = 0u;
                    break;
                }

                if (timeout_ms > 0) {
                    uint32_t elapsed = sched_ticks() - start_tick;
                    if (elapsed >= timeout_ticks) {
                        ret = 0u;
                        break;
                    }
                    remaining_ticks = timeout_ticks - elapsed;
                }

                if (wait_read_stdin) {
                    uint32_t wait_ticks = (timeout_ms > 0) ? remaining_ticks : 0u;
                    if (console_wait_readable(wait_ticks, 0u) == CONSOLE_IO_BLOCKED) {
                        sched_block_until_runnable();
                    }
                    continue;
                }

                if (timeout_ms < 0) {
                    sched_sleep_ticks(1u);
                } else {
                    sched_sleep_ticks(remaining_ticks ? remaining_ticks : 1u);
                }
                sched_block_until_runnable();
                continue;
            }
            break;
        }
        case SYS_TTY_GETMODE: {
            int32_t fd = (int32_t)regs->arg0;
            if (!sched_fd_is_tty(fd)) {
                err = ERRNO_EBADF;
                ret = (uint32_t)-1;
                break;
            }
            ret = console_tty_get_lflag();
            break;
        }
        case SYS_TTY_SETMODE: {
            int32_t fd = (int32_t)regs->arg0;
            uint32_t lflag = regs->arg1;
            if (!sched_fd_is_tty(fd)) {
                err = ERRNO_EBADF;
                ret = (uint32_t)-1;
                break;
            }
            ret = console_tty_set_lflag(lflag);
            break;
        }
        case SYS_CLOCK_GETRES: {
            int32_t clock_id = (int32_t)regs->arg0;
            uint32_t res_addr = regs->arg1;
            syscall_timespec32_t res;
            if (!clockid_is_supported(clock_id)) {
                err = ERRNO_EINVAL;
                ret = (uint32_t)-1;
                break;
            }
            if (res_addr != 0u) {
                res.tv_sec = 0;
                res.tv_nsec = (int32_t)CLOCK_RESOLUTION_NS;
                if (!abi_user_write_timespec(res_addr, &res)) {
                    err = ERRNO_EFAULT;
                    ret = (uint32_t)-1;
                    break;
                }
            }
            ret = 0u;
            break;
        }
        case SYS_CLOCK_GETTIME: {
            int32_t clock_id = (int32_t)regs->arg0;
            uint32_t ts_addr = regs->arg1;
            uint64_t ns;
            syscall_timespec32_t ts;
            if (ts_addr == 0u) {
                err = ERRNO_EFAULT;
                ret = (uint32_t)-1;
                break;
            }
            if (!clockid_read_time_ns(clock_id, &ns)) {
                err = ERRNO_EINVAL;
                ret = (uint32_t)-1;
                break;
            }
            if (!time_ns_to_timespec(ns, &ts)) {
                err = ERRNO_EOVERFLOW;
                ret = (uint32_t)-1;
                break;
            }
            if (!abi_user_write_timespec(ts_addr, &ts)) {
                err = ERRNO_EFAULT;
                ret = (uint32_t)-1;
                break;
            }
            ret = 0u;
            break;
        }
        case SYS_CLOCK_SETTIME: {
            int32_t clock_id = (int32_t)regs->arg0;
            uint32_t tp_addr = regs->arg1;
            syscall_timespec32_t tp;
            uint64_t target_ns;
            uint64_t raw_ns;
            if (clock_id == (int32_t)SYS_CLOCK_MONOTONIC) {
                err = ERRNO_EINVAL;
                ret = (uint32_t)-1;
                break;
            }
            if (clock_id != (int32_t)SYS_CLOCK_REALTIME) {
                err = ERRNO_EINVAL;
                ret = (uint32_t)-1;
                break;
            }
            if (tp_addr == 0u || !abi_user_read_timespec(tp_addr, &tp)) {
                err = ERRNO_EFAULT;
                ret = (uint32_t)-1;
                break;
            }
            if (!timespec_to_ns(&tp, &target_ns)) {
                err = ERRNO_EINVAL;
                ret = (uint32_t)-1;
                break;
            }

            raw_ns = mmio_read_time_ns(TIME_MMIO_REALTIME_LO, TIME_MMIO_REALTIME_HI);
            if (target_ns >= raw_ns) {
                g_realtime_offset_neg = 0u;
                g_realtime_offset_mag_ns = target_ns - raw_ns;
            } else {
                g_realtime_offset_neg = 1u;
                g_realtime_offset_mag_ns = raw_ns - target_ns;
            }
            ret = 0u;
            break;
        }
        case SYS_GETTIMEOFDAY: {
            uint32_t tv_addr = regs->arg0;
            uint32_t tz_addr = regs->arg1;
            uint64_t ns;
            syscall_timeval32_t tv;
            if (tv_addr == 0u) {
                err = ERRNO_EFAULT;
                ret = (uint32_t)-1;
                break;
            }
            ns = clock_apply_realtime_offset(mmio_read_time_ns(TIME_MMIO_REALTIME_LO, TIME_MMIO_REALTIME_HI));
            if (!time_ns_to_timeval(ns, &tv)) {
                err = ERRNO_EOVERFLOW;
                ret = (uint32_t)-1;
                break;
            }
            if (!abi_user_write_timeval(tv_addr, &tv)) {
                err = ERRNO_EFAULT;
                ret = (uint32_t)-1;
                break;
            }
            if (tz_addr != 0u) {
                if (!abi_user_write32(tz_addr + 0u, 0u) || !abi_user_write32(tz_addr + 4u, 0u)) {
                    err = ERRNO_EFAULT;
                    ret = (uint32_t)-1;
                    break;
                }
            }
            ret = 0u;
            break;
        }
        default:
            err = ERRNO_ENOSYS;
            ret = (uint32_t)-1;
            break;
    }

    SYSCALL_PUBLISH_RESULT(regs, ret, err);
    return ret;
}
