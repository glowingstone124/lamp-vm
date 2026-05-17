#include "../include/kernel/console.h"
#include "../include/kernel/fd_selftest.h"
#include "../include/kernel/platform.h"
#include "../include/kernel/printk.h"
#include "../include/kernel/sched.h"
#include "../include/kernel/spinlock.h"
#include "../include/kernel/syscall.h"
#include "../include/kernel/types.h"

enum {
    ABI_OFF_LAST_NR = 0x08u,
    ABI_OFF_RET = 0x24u,
    ABI_OFF_ERRNO = 0x28u,
    ABI_OFF_TICK = 0x2Cu
};

enum {
    TEST_ERRNO_EAGAIN = 11u,
    TEST_ERRNO_EBADF = 9u,
    TEST_ERRNO_ESRCH = 3u,
    TEST_ERRNO_EACCES = 13u,
    TEST_ERRNO_ENOENT = 2u,
    TEST_ERRNO_ENOTDIR = 20u,
    TEST_ERRNO_EISDIR = 21u,
    TEST_ERRNO_EINVAL = 22u,
    TEST_ERRNO_ENOTTY = 25u,
    TEST_ERRNO_ESPIPE = 29u,
    TEST_ERRNO_EROFS = 30u,
    TEST_ERRNO_EPIPE = 32u,
    TEST_ERRNO_ERANGE = 34u,
    TEST_ERRNO_ENOTSOCK = 88u,
    TEST_ERRNO_EOPNOTSUPP = 95u,
    TEST_ERRNO_EAFNOSUPPORT = 97u,
    TEST_ERRNO_ENOTCONN = 107u
};

typedef struct fdtest_pollfd32 {
    int32_t fd;
    int16_t events;
    int16_t revents;
} fdtest_pollfd32_t;

typedef struct fdtest_stat32 {
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
} fdtest_stat32_t;

typedef struct fdtest_dirent32 {
    uint32_t d_ino;
    uint32_t d_off;
    uint32_t d_reclen;
    uint32_t d_type;
    char d_name[256];
} fdtest_dirent32_t;

typedef struct fdtest_termios32 {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_cc[SYS_TERMIOS_NCCS];
} fdtest_termios32_t;

typedef struct fdtest_winsize32 {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} fdtest_winsize32_t;

enum {
    FDTEST_STRESS_LINES = 32u,
    FDTEST_STRESS_TIMEOUT_MS = 200,
    FDTEST_WAITQ_WORKERS = 4u,
    FDTEST_WAITQ_ROUNDS = 12u,
    FDTEST_WAITQ_SPIN_TICKS = 128u
};

typedef struct fdtest_waitq_worker_state {
    uint32_t waiting;
} fdtest_waitq_worker_state_t;

static uint8_t g_fdtest_buf[64];
static fdtest_pollfd32_t g_fdtest_pfd;
static fdtest_stat32_t g_fdtest_stat;
static fdtest_stat32_t g_fdtest_fstat;
static fdtest_dirent32_t g_fdtest_dirents[8];
static fdtest_termios32_t g_fdtest_termios;
static fdtest_winsize32_t g_fdtest_winsize;
static syscall_sigaction32_t g_fdtest_sigaction;
static syscall_sigaction32_t g_fdtest_old_sigaction;
static uint32_t g_fdtest_sigmask;
static uint32_t g_fdtest_old_sigmask;
static int32_t g_fdtest_pipefd[2];
static char g_fdtest_cwd[64];
static uint32_t g_fdtest_sel_read;
static const char g_fdtest_path_dev_null[] = "/dev/null";
static const char g_fdtest_path_dev_zero[] = "/dev/zero";
static const char g_fdtest_path_dev_tty[] = "/dev/tty";
static const char g_fdtest_path_missing[] = "/dev/missing";
static const char g_fdtest_path_bin_hello[] = "/bin/hello";
static const char g_fdtest_path_bin[] = "/bin";
static const char g_fdtest_path_tmp_fdtest[] = "/tmp_fdtest";
static volatile uint32_t g_fdstress_total_lines;
static volatile uint32_t g_fdstress_sent_lines;
static volatile uint32_t g_fdstress_done;
static spinlock_t g_fdtest_run_lock;
static volatile uint32_t g_fdtest_running;
static sched_waitq_t g_fdtest_waitq;
static spinlock_t g_fdtest_waitq_lock;
static fdtest_waitq_worker_state_t g_fdtest_waitq_workers[FDTEST_WAITQ_WORKERS];
static volatile uint32_t g_fdtest_waitq_goal;
static volatile uint32_t g_fdtest_waitq_ack;
static volatile uint32_t g_fdtest_waitq_waiting;
static volatile uint32_t g_fdtest_waitq_done;
static volatile uint32_t g_fdtest_getppid_child_ret;
static volatile uint32_t g_fdtest_getppid_child_err;
static volatile uint32_t g_fdtest_signal_child_started;

static void fdtest_fail(const char *msg, uint32_t got, uint32_t want, uint32_t *fails);

static inline uint32_t abi_read32(uint32_t off) {
    return *(volatile uint32_t *)(uintptr_t)(SYSCALL_ABI_ADDR + off);
}

static inline uint32_t ptr32(const void *p) {
    return (uint32_t)(uintptr_t)p;
}

#define FDTEST_SYSCALL(nr_, a0_, a1_, a2_, a3_, a4_, a5_, ret_lval_, err_lval_)       \
    do {                                                                                \
        syscall_regs_t __regs;                                                          \
        (__regs).nr = (nr_);                                                            \
        (__regs).arg0 = (a0_);                                                          \
        (__regs).arg1 = (a1_);                                                          \
        (__regs).arg2 = (a2_);                                                          \
        (__regs).arg3 = (a3_);                                                          \
        (__regs).arg4 = (a4_);                                                          \
        (__regs).arg5 = (a5_);                                                          \
        (__regs).abi_addr = 0u;                                                         \
        (void)syscall_dispatch(&__regs);                                                \
        (ret_lval_) = abi_read32(ABI_OFF_RET);                                          \
        (err_lval_) = (((int32_t)(ret_lval_)) == -1) ? abi_read32(ABI_OFF_ERRNO) : 0u; \
    } while (0)

static void fdtest_drain_stdin(void) {
    uint8_t buf[32];
    while (console_read(buf, (uint32_t)sizeof(buf), 1u) > 0) {
    }
}

static void fdtest_wait_child_reap(int32_t tid, uint32_t *fails) {
    uint32_t status = 0u;
    for (;;) {
        int rc = sched_waitpid(tid, 0u, &status);
        if (rc == SCHED_WAITPID_BLOCKED) {
            sched_block_until_runnable();
            continue;
        }
        if (rc <= 0) {
            fdtest_fail("waitpid child", (uint32_t)rc, 1u, fails);
        }
        break;
    }
}

static uint32_t fdtest_wait_child_reap_status(int32_t tid, uint32_t *fails) {
    uint32_t status = 0u;
    for (;;) {
        int rc = sched_waitpid(tid, 0u, &status);
        if (rc == SCHED_WAITPID_BLOCKED) {
            sched_block_until_runnable();
            continue;
        }
        if (rc <= 0) {
            fdtest_fail("waitpid child", (uint32_t)rc, 1u, fails);
        }
        break;
    }
    return status;
}

static void fdtest_feeder_task(sched_task_t *task, void *arg) {
    uint32_t n;
    (void)task;
    (void)arg;

    n = g_fdstress_sent_lines;
    if (n >= g_fdstress_total_lines) {
        g_fdstress_done = 1u;
        sched_exit_code(0u);
        return;
    }

    /* Feed one full line without textual payload to avoid shell pollution. */
    console_rx_feed((uint8_t)'\n');
    g_fdstress_sent_lines = n + 1u;
    sched_sleep_ticks(1u);
}

static uint32_t fdtest_count_newlines(const uint8_t *buf, uint32_t n) {
    uint32_t lines = 0u;
    for (uint32_t i = 0u; i < n; i++) {
        if (buf[i] == (uint8_t)'\n') {
            lines++;
        }
    }
    return lines;
}

static uint32_t fdtest_streq(const char *a, const char *b) {
    uint32_t i = 0u;
    if (!a || !b) {
        return 0u;
    }
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return 0u;
        }
        i++;
    }
    return (a[i] == '\0' && b[i] == '\0') ? 1u : 0u;
}

static void fdtest_getppid_child_task(sched_task_t *task, void *arg) {
    uint32_t ret = 0u;
    uint32_t err = 0u;
    (void)task;
    (void)arg;
    FDTEST_SYSCALL(SYS_GETPPID, 0u, 0u, 0u, 0u, 0u, 0u, ret, err);
    g_fdtest_getppid_child_ret = ret;
    g_fdtest_getppid_child_err = err;
    sched_exit_code(err == 0u ? 0u : 1u);
}

static void fdtest_signal_child_task(sched_task_t *task, void *arg) {
    (void)task;
    (void)arg;
    g_fdtest_signal_child_started = 1u;
    sched_sleep_ticks(1000u);
    sched_exit_code(0u);
}

static uint32_t fdtest_waitq_state_get_ack(void) {
    uint32_t ack;
    spinlock_lock(&g_fdtest_waitq_lock);
    ack = g_fdtest_waitq_ack;
    spinlock_unlock(&g_fdtest_waitq_lock);
    return ack;
}

static uint32_t fdtest_waitq_state_get_waiting(void) {
    uint32_t waiting;
    spinlock_lock(&g_fdtest_waitq_lock);
    waiting = g_fdtest_waitq_waiting;
    spinlock_unlock(&g_fdtest_waitq_lock);
    return waiting;
}

static uint32_t fdtest_waitq_wait_for_waiters(uint32_t min_waiters, uint32_t spin_ticks) {
    for (uint32_t spin = 0u; spin < spin_ticks; spin++) {
        if (fdtest_waitq_state_get_waiting() >= min_waiters) {
            return 1u;
        }
        sched_sleep_ticks(1u);
        sched_block_until_runnable();
    }
    return 0u;
}

static void fdtest_waitq_state_stop_all(void) {
    spinlock_lock(&g_fdtest_waitq_lock);
    g_fdtest_waitq_done = 1u;
    spinlock_unlock(&g_fdtest_waitq_lock);
    sched_waitq_wake_all(&g_fdtest_waitq);
}

static void fdtest_waitq_worker_task(sched_task_t *task, void *arg) {
    fdtest_waitq_worker_state_t *st = (fdtest_waitq_worker_state_t *)arg;
    uint32_t wake_all = 0u;
    (void)task;

    if (!st) {
        sched_exit_code(1u);
        return;
    }

    spinlock_lock(&g_fdtest_waitq_lock);
    if (g_fdtest_waitq_done) {
        spinlock_unlock(&g_fdtest_waitq_lock);
        sched_exit_code(0u);
        return;
    }
    if (st->waiting == 0u) {
        st->waiting = 1u;
        g_fdtest_waitq_waiting++;
        spinlock_unlock(&g_fdtest_waitq_lock);
        sched_waitq_sleep(&g_fdtest_waitq, 0u);
        return;
    }

    st->waiting = 0u;
    if (g_fdtest_waitq_waiting != 0u) {
        g_fdtest_waitq_waiting--;
    }
    if (g_fdtest_waitq_ack < g_fdtest_waitq_goal) {
        g_fdtest_waitq_ack++;
    }
    if (g_fdtest_waitq_ack >= g_fdtest_waitq_goal) {
        g_fdtest_waitq_done = 1u;
        wake_all = 1u;
    }
    spinlock_unlock(&g_fdtest_waitq_lock);

    if (wake_all) {
        sched_waitq_wake_all(&g_fdtest_waitq);
    }
    sched_yield();
}

static void fdtest_waitq_stress(uint32_t *fails) {
    int32_t tids[FDTEST_WAITQ_WORKERS];
    uint32_t i;
    uint32_t max_loops;

    sched_waitq_init(&g_fdtest_waitq);
    spinlock_init(&g_fdtest_waitq_lock);
    spinlock_lock(&g_fdtest_waitq_lock);
    g_fdtest_waitq_goal = FDTEST_WAITQ_WORKERS * FDTEST_WAITQ_ROUNDS;
    g_fdtest_waitq_ack = 0u;
    g_fdtest_waitq_waiting = 0u;
    g_fdtest_waitq_done = 0u;
    for (i = 0u; i < FDTEST_WAITQ_WORKERS; i++) {
        g_fdtest_waitq_workers[i].waiting = 0u;
    }
    spinlock_unlock(&g_fdtest_waitq_lock);

    for (i = 0u; i < FDTEST_WAITQ_WORKERS; i++) {
        tids[i] = sched_spawn("fdwaitq", fdtest_waitq_worker_task, &g_fdtest_waitq_workers[i]);
        if (tids[i] < 0) {
            fdtest_fail("spawn waitq worker", (uint32_t)tids[i], 1u, fails);
            fdtest_waitq_state_stop_all();
            for (uint32_t j = 0u; j < i; j++) {
                if (tids[j] > 0) {
                    fdtest_wait_child_reap(tids[j], fails);
                }
            }
            return;
        }
    }

    max_loops = FDTEST_WAITQ_WORKERS * FDTEST_WAITQ_SPIN_TICKS;
    if (!fdtest_waitq_wait_for_waiters(FDTEST_WAITQ_WORKERS, max_loops)) {
        fdtest_fail("waitq workers blocked", fdtest_waitq_state_get_waiting(), FDTEST_WAITQ_WORKERS, fails);
        fdtest_waitq_state_stop_all();
        for (i = 0u; i < FDTEST_WAITQ_WORKERS; i++) {
            fdtest_wait_child_reap(tids[i], fails);
        }
        return;
    }

    max_loops = g_fdtest_waitq_goal * FDTEST_WAITQ_SPIN_TICKS * 4u;
    for (i = 0u; i < max_loops; i++) {
        uint32_t prev_ack = fdtest_waitq_state_get_ack();
        uint32_t progressed = 0u;
        if (prev_ack >= g_fdtest_waitq_goal) {
            break;
        }
        if (!fdtest_waitq_wait_for_waiters(1u, FDTEST_WAITQ_SPIN_TICKS * 2u)) {
            continue;
        }
        sched_waitq_wake_one(&g_fdtest_waitq);
        for (uint32_t spin = 0u; spin < FDTEST_WAITQ_SPIN_TICKS; spin++) {
            if (fdtest_waitq_state_get_ack() > prev_ack) {
                progressed = 1u;
                break;
            }
            sched_sleep_ticks(1u);
            sched_block_until_runnable();
        }
        if (!progressed) {
            /* Recovery for transient no-op wake windows. */
            sched_waitq_wake_all(&g_fdtest_waitq);
        }
    }

    if (fdtest_waitq_state_get_ack() != g_fdtest_waitq_goal) {
        fdtest_fail("waitq wake count", fdtest_waitq_state_get_ack(), g_fdtest_waitq_goal, fails);
    }

    fdtest_waitq_state_stop_all();
    for (i = 0u; i < FDTEST_WAITQ_WORKERS; i++) {
        uint32_t status = fdtest_wait_child_reap_status(tids[i], fails);
        if (status != 0u) {
            fdtest_fail("waitq worker exit", status, 0u, fails);
        }
    }
}

static void fdtest_poll_stress(uint32_t *fails) {
    uint32_t ret = 0u;
    uint32_t err = 0u;
    uint32_t lines = 0u;
    uint32_t loops = 0u;
    int feeder_tid;
    fdtest_pollfd32_t pfd;

    fdtest_drain_stdin();
    g_fdstress_total_lines = FDTEST_STRESS_LINES;
    g_fdstress_sent_lines = 0u;
    g_fdstress_done = 0u;

    feeder_tid = sched_spawn("fdfeed_poll", fdtest_feeder_task, 0);
    if (feeder_tid < 0) {
        fdtest_fail("spawn poll feeder", (uint32_t)feeder_tid, 1u, fails);
        return;
    }

    while (lines < FDTEST_STRESS_LINES && loops < FDTEST_STRESS_LINES * 8u) {
        pfd.fd = 0;
        pfd.events = (int16_t)SYS_POLLIN;
        pfd.revents = 0;
        FDTEST_SYSCALL(SYS_POLL, ptr32(&pfd), 1u, (uint32_t)FDTEST_STRESS_TIMEOUT_MS, 0u, 0u, 0u, ret, err);
        if (err != 0u) {
            fdtest_fail("poll stress errno", err, 0u, fails);
            break;
        }
        if (ret == 0u) {
            loops++;
            continue;
        }
        FDTEST_SYSCALL(SYS_READ, 0u, ptr32(g_fdtest_buf), (uint32_t)sizeof(g_fdtest_buf), 0u, 0u, 0u, ret, err);
        if (err != 0u || (int32_t)ret < 0) {
            fdtest_fail("poll stress read", err, 0u, fails);
            break;
        }
        lines += fdtest_count_newlines(g_fdtest_buf, ret);
        loops++;
    }

    if (lines < FDTEST_STRESS_LINES) {
        fdtest_fail("poll stress lines", lines, FDTEST_STRESS_LINES, fails);
    }
    fdtest_wait_child_reap(feeder_tid, fails);
    if (g_fdstress_done == 0u) {
        fdtest_fail("poll feeder done", g_fdstress_done, 1u, fails);
    }
}

static void fdtest_select_stress(uint32_t *fails) {
    uint32_t ret = 0u;
    uint32_t err = 0u;
    uint32_t lines = 0u;
    uint32_t loops = 0u;
    int feeder_tid;

    fdtest_drain_stdin();
    g_fdstress_total_lines = FDTEST_STRESS_LINES;
    g_fdstress_sent_lines = 0u;
    g_fdstress_done = 0u;

    feeder_tid = sched_spawn("fdfeed_sel", fdtest_feeder_task, 0);
    if (feeder_tid < 0) {
        fdtest_fail("spawn select feeder", (uint32_t)feeder_tid, 1u, fails);
        return;
    }

    while (lines < FDTEST_STRESS_LINES && loops < FDTEST_STRESS_LINES * 8u) {
        g_fdtest_sel_read = 1u << 0;
        FDTEST_SYSCALL(SYS_SELECT, 1u, ptr32((const void *)&g_fdtest_sel_read), 0u, 0u,
                       (uint32_t)FDTEST_STRESS_TIMEOUT_MS, 0u, ret, err);
        if (err != 0u) {
            fdtest_fail("select stress errno", err, 0u, fails);
            break;
        }
        if (ret == 0u) {
            loops++;
            continue;
        }
        FDTEST_SYSCALL(SYS_READ, 0u, ptr32(g_fdtest_buf), (uint32_t)sizeof(g_fdtest_buf), 0u, 0u, 0u, ret, err);
        if (err != 0u || (int32_t)ret < 0) {
            fdtest_fail("select stress read", err, 0u, fails);
            break;
        }
        lines += fdtest_count_newlines(g_fdtest_buf, ret);
        loops++;
    }

    if (lines < FDTEST_STRESS_LINES) {
        fdtest_fail("select stress lines", lines, FDTEST_STRESS_LINES, fails);
    }
    fdtest_wait_child_reap(feeder_tid, fails);
    if (g_fdstress_done == 0u) {
        fdtest_fail("select feeder done", g_fdstress_done, 1u, fails);
    }
}

static void fdtest_fail(const char *msg, uint32_t got, uint32_t want, uint32_t *fails) {
    if (fails) {
        (*fails)++;
    }
    klog_begin(KLOG_LEVEL_ERROR, "fdtest");
    klog_puts(msg);
    klog_puts(" got=");
    klog_hex32(got);
    klog_puts(" want=");
    klog_hex32(want);
    klog_end();
}

void fd_selftest_run(void) {
    uint32_t fails = 0u;
    uint32_t ret = 0u;
    uint32_t err = 0u;
    uint32_t dupfd;
    uint32_t fd;
    uint32_t i;
    uint32_t old_fl0 = 0u;
    uint32_t saved_tty_lflag = 0u;
    fdtest_pollfd32_t *pfd = &g_fdtest_pfd;
    volatile uint32_t *sel_read = &g_fdtest_sel_read;

    spinlock_lock(&g_fdtest_run_lock);
    if (g_fdtest_running != 0u) {
        spinlock_unlock(&g_fdtest_run_lock);
        KLOGW("fdtest", "already running");
        return;
    }
    g_fdtest_running = 1u;
    spinlock_unlock(&g_fdtest_run_lock);

    KLOGI("fdtest", "start v18");
    saved_tty_lflag = console_tty_get_lflag();
    (void)console_tty_set_lflag(saved_tty_lflag & ~TTY_LFLAG_ECHO);
    fdtest_drain_stdin();

    FDTEST_SYSCALL(SYS_GETPID, 0u, 0u, 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || (int32_t)ret < 0) {
        fdtest_fail("getpid", err, 0u, &fails);
    } else {
        int32_t child_tid;
        uint32_t parent_pid = ret;
        g_fdtest_getppid_child_ret = 0xFFFFFFFFu;
        g_fdtest_getppid_child_err = 0xFFFFFFFFu;
        child_tid = sched_spawn("fdgetppid", fdtest_getppid_child_task, 0);
        if (child_tid < 0) {
            fdtest_fail("spawn getppid child", (uint32_t)child_tid, 1u, &fails);
        } else {
            uint32_t status = fdtest_wait_child_reap_status(child_tid, &fails);
            if (status != 0u) {
                fdtest_fail("getppid child exit", status, 0u, &fails);
            }
            if (g_fdtest_getppid_child_err != 0u || g_fdtest_getppid_child_ret != parent_pid) {
                fdtest_fail("getppid child parent", g_fdtest_getppid_child_ret, parent_pid, &fails);
            }
        }
    }

    FDTEST_SYSCALL(SYS_FCNTL, 0u, SYS_FCNTL_F_GETFL, 0u, 0u, 0u, 0u, old_fl0, err);
    if (err != 0u) {
        fdtest_fail("fcntl(F_GETFL,0)", err, 0u, &fails);
        goto out_restore_tty;
    }

    FDTEST_SYSCALL(SYS_DUP, 0u, 0u, 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || (int32_t)ret < 0) {
        fdtest_fail("dup(stdin) errno", err, 0u, &fails);
        fdtest_fail("dup(stdin) ret", ret, 0u, &fails);
        klog_begin(KLOG_LEVEL_ERROR, "fdtest");
        klog_puts("dup(stdin) abi last_nr=");
        klog_hex32(abi_read32(ABI_OFF_LAST_NR));
        klog_puts(" abi errno=");
        klog_hex32(abi_read32(ABI_OFF_ERRNO));
        klog_puts(" abi ret=");
        klog_hex32(abi_read32(ABI_OFF_RET));
        klog_puts(" abi tick=");
        klog_hex32(abi_read32(ABI_OFF_TICK));
        klog_end();
        if ((int32_t)ret >= 0) {
            (void)sched_fd_close((int32_t)ret);
        }
        klog_begin(KLOG_LEVEL_ERROR, "fdtest");
        klog_puts("dup(stdin) fd0_open=");
        klog_hex32(sched_fd_is_open(0));
        klog_end();
        goto out_restore_tty;
    }
    dupfd = ret;
    if (dupfd < 3u) {
        fdtest_fail("dup(stdin) fd range", dupfd, 3u, &fails);
    }

    FDTEST_SYSCALL(SYS_FCNTL, dupfd, SYS_FCNTL_F_GETFD, 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || ret != 0u) {
        fdtest_fail("fcntl(F_GETFD) initial", ret, 0u, &fails);
    }

    FDTEST_SYSCALL(SYS_FCNTL, dupfd, SYS_FCNTL_F_SETFD, SYS_FD_CLOEXEC, 0u, 0u, 0u, ret, err);
    if (err != 0u || ret != 0u) {
        fdtest_fail("fcntl(F_SETFD,FD_CLOEXEC)", err, 0u, &fails);
    }

    FDTEST_SYSCALL(SYS_FCNTL, dupfd, SYS_FCNTL_F_GETFD, 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || (ret & SYS_FD_CLOEXEC) == 0u) {
        fdtest_fail("fcntl(F_GETFD) cloexec", ret, SYS_FD_CLOEXEC, &fails);
    }

    FDTEST_SYSCALL(SYS_FCNTL, dupfd, SYS_FCNTL_F_SETFL, SYS_O_NONBLOCK, 0u, 0u, 0u, ret, err);
    if (err != 0u || ret != 0u) {
        fdtest_fail("fcntl(F_SETFL,O_NONBLOCK)", err, 0u, &fails);
    }

    FDTEST_SYSCALL(SYS_FCNTL, 0u, SYS_FCNTL_F_GETFL, 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || (ret & SYS_O_NONBLOCK) == 0u) {
        fdtest_fail("fcntl(F_GETFL,stdin) shared", ret, SYS_O_NONBLOCK, &fails);
    }

    FDTEST_SYSCALL(SYS_READ, dupfd, ptr32(g_fdtest_buf), 1u, 0u, 0u, 0u, ret, err);
    if ((int32_t)ret == -1) {
        if (err != TEST_ERRNO_EAGAIN) {
            fdtest_fail("read(nonblock) errno", err, TEST_ERRNO_EAGAIN, &fails);
        }
    } else {
        if (err != 0u) {
            fdtest_fail("read(nonblock) err", err, 0u, &fails);
        }
        if (ret > 1u) {
            fdtest_fail("read(nonblock) ret", ret, 1u, &fails);
        }
    }

    pfd->fd = (int32_t)dupfd;
    pfd->events = (int16_t)SYS_POLLIN;
    pfd->revents = 0;
    FDTEST_SYSCALL(SYS_POLL, ptr32(pfd), 1u, 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u) {
        fdtest_fail("poll(stdin,timeout=0) errno", err, 0u, &fails);
    }
    if (ret > 1u) {
        fdtest_fail("poll(stdin,timeout=0) ret", ret, 1u, &fails);
    } else if (ret == 0u) {
        if (pfd->revents != 0) {
            fdtest_fail("poll revents empty", (uint32_t)(uint16_t)pfd->revents, 0u, &fails);
        }
    } else {
        if (((uint16_t)pfd->revents & (uint16_t)SYS_POLLIN) == 0u) {
            fdtest_fail("poll revents ready", (uint32_t)(uint16_t)pfd->revents, SYS_POLLIN, &fails);
        }
    }

    pfd->fd = -1;
    pfd->events = (int16_t)SYS_POLLIN;
    pfd->revents = (int16_t)0x7FFF;
    FDTEST_SYSCALL(SYS_POLL, ptr32(pfd), 1u, 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || ret != 0u) {
        fdtest_fail("poll(fd<0) ret/errno", err, 0u, &fails);
        fdtest_fail("poll(fd<0) ret", ret, 0u, &fails);
    }
    if (pfd->revents != 0) {
        fdtest_fail("poll(fd<0) revents", (uint32_t)(uint16_t)pfd->revents, 0u, &fails);
    }

    if (dupfd >= 32u) {
        fdtest_fail("dupfd out of select range", dupfd, 31u, &fails);
    }
    *sel_read = (dupfd < 32u) ? (1u << dupfd) : 0u;
    FDTEST_SYSCALL(SYS_SELECT, dupfd + 1u, ptr32((const void *)sel_read), 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u) {
        fdtest_fail("select(stdin,timeout=0) errno", err, 0u, &fails);
    }
    if (ret > 1u) {
        fdtest_fail("select(stdin,timeout=0) ret", ret, 1u, &fails);
    } else if (ret == 0u) {
        if (*sel_read != 0u) {
            fdtest_fail("select readmask empty", *sel_read, 0u, &fails);
        }
    } else if (dupfd < 32u) {
        if (((*sel_read) & (1u << dupfd)) == 0u) {
            fdtest_fail("select readmask ready", *sel_read, 1u << dupfd, &fails);
        }
    }

    FDTEST_SYSCALL(SYS_CLOSE, dupfd, 0u, 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || ret != 0u) {
        fdtest_fail("close(dupfd)", err, 0u, &fails);
    }

    FDTEST_SYSCALL(SYS_CLOSE, dupfd, 0u, 0u, 0u, 0u, 0u, ret, err);
    if ((int32_t)ret != -1 || err != TEST_ERRNO_EBADF) {
        fdtest_fail("close(dupfd) second", err, TEST_ERRNO_EBADF, &fails);
    }

    FDTEST_SYSCALL(SYS_FCNTL, 0u, SYS_FCNTL_F_SETFL, old_fl0, 0u, 0u, 0u, ret, err);
    if (err != 0u || ret != 0u) {
        fdtest_fail("restore stdin flags", err, 0u, &fails);
    }

    FDTEST_SYSCALL(SYS_IOCTL, 0u, SYS_IOCTL_TCGETS, ptr32(&g_fdtest_termios), 0u, 0u, 0u, ret, err);
    if (err != 0u || ret != 0u) {
        fdtest_fail("ioctl(TCGETS)", err, 0u, &fails);
    } else {
        uint32_t old_termios_lflag = g_fdtest_termios.c_lflag;
        if ((old_termios_lflag & SYS_TERMIOS_ICANON) == 0u) {
            fdtest_fail("termios ICANON", old_termios_lflag & SYS_TERMIOS_ICANON, SYS_TERMIOS_ICANON, &fails);
        }
        if ((old_termios_lflag & SYS_TERMIOS_ISIG) == 0u) {
            fdtest_fail("termios ISIG", old_termios_lflag & SYS_TERMIOS_ISIG, SYS_TERMIOS_ISIG, &fails);
        }
        g_fdtest_termios.c_lflag = old_termios_lflag | SYS_TERMIOS_ECHO;
        FDTEST_SYSCALL(SYS_IOCTL, 0u, SYS_IOCTL_TCSETS, ptr32(&g_fdtest_termios), 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 0u || (console_tty_get_lflag() & TTY_LFLAG_ECHO) == 0u) {
            fdtest_fail("ioctl(TCSETS echo on)", console_tty_get_lflag() & TTY_LFLAG_ECHO, TTY_LFLAG_ECHO, &fails);
        }
        g_fdtest_termios.c_lflag = old_termios_lflag;
        FDTEST_SYSCALL(SYS_IOCTL, 0u, SYS_IOCTL_TCSETS, ptr32(&g_fdtest_termios), 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 0u ||
            (console_tty_get_lflag() & TTY_LFLAG_ECHO) != ((old_termios_lflag & SYS_TERMIOS_ECHO) ? TTY_LFLAG_ECHO : 0u)) {
            fdtest_fail("ioctl(TCSETS restore)", console_tty_get_lflag() & TTY_LFLAG_ECHO,
                        (old_termios_lflag & SYS_TERMIOS_ECHO) ? TTY_LFLAG_ECHO : 0u, &fails);
        }
    }

    FDTEST_SYSCALL(SYS_IOCTL, 0u, SYS_IOCTL_TIOCGWINSZ, ptr32(&g_fdtest_winsize), 0u, 0u, 0u, ret, err);
    if (err != 0u || ret != 0u || g_fdtest_winsize.ws_row != 30u || g_fdtest_winsize.ws_col != 80u) {
        fdtest_fail("ioctl(TIOCGWINSZ)", ((uint32_t)g_fdtest_winsize.ws_row << 16) | g_fdtest_winsize.ws_col,
                    (30u << 16) | 80u, &fails);
    }

    FDTEST_SYSCALL(SYS_IOCTL, 99u, SYS_IOCTL_TCGETS, ptr32(&g_fdtest_termios), 0u, 0u, 0u, ret, err);
    if ((int32_t)ret != -1 || err != TEST_ERRNO_EBADF) {
        fdtest_fail("ioctl(bad fd)", err, TEST_ERRNO_EBADF, &fails);
    }

    g_fdtest_sigaction.handler = SYS_SIG_IGN;
    g_fdtest_sigaction.flags = 0u;
    g_fdtest_sigaction.mask = 0u;
    g_fdtest_sigaction.restorer = 0u;
    FDTEST_SYSCALL(SYS_SIGACTION, SYS_SIGINT, ptr32(&g_fdtest_sigaction), ptr32(&g_fdtest_old_sigaction),
                   0u, 0u, 0u, ret, err);
    if (err != 0u || ret != 0u || g_fdtest_old_sigaction.handler != SYS_SIG_DFL) {
        fdtest_fail("sigaction(SIGINT,IGN)", err, 0u, &fails);
    }
    FDTEST_SYSCALL(SYS_KILL, 0x7FFFFFFFu, SYS_SIGINT, 0u, 0u, 0u, 0u, ret, err);
    if ((int32_t)ret != -1 || err != TEST_ERRNO_ESRCH) {
        fdtest_fail("kill(non-pid)", err, TEST_ERRNO_ESRCH, &fails);
    }
    FDTEST_SYSCALL(SYS_GETPID, 0u, 0u, 0u, 0u, 0u, 0u, ret, err);
    if (err == 0u) {
        FDTEST_SYSCALL(SYS_KILL, ret, SYS_SIGINT, 0u, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 0u) {
            fdtest_fail("kill(self ignored SIGINT)", err, 0u, &fails);
        }
    }
    g_fdtest_sigaction.handler = SYS_SIG_DFL;
    FDTEST_SYSCALL(SYS_SIGACTION, SYS_SIGINT, ptr32(&g_fdtest_sigaction), 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || ret != 0u) {
        fdtest_fail("sigaction(SIGINT,DFL)", err, 0u, &fails);
    }
    FDTEST_SYSCALL(SYS_SIGACTION, SYS_SIGKILL, ptr32(&g_fdtest_sigaction), 0u, 0u, 0u, 0u, ret, err);
    if ((int32_t)ret != -1 || err != TEST_ERRNO_EINVAL) {
        fdtest_fail("sigaction(SIGKILL)", err, TEST_ERRNO_EINVAL, &fails);
    }

    g_fdtest_sigmask = 1u << (SYS_SIGINT - 1u);
    FDTEST_SYSCALL(SYS_SIGPROCMASK, SYS_SIG_BLOCK, ptr32(&g_fdtest_sigmask), ptr32(&g_fdtest_old_sigmask),
                   0u, 0u, 0u, ret, err);
    if (err != 0u || ret != 0u) {
        fdtest_fail("sigprocmask(BLOCK)", err, 0u, &fails);
    }
    FDTEST_SYSCALL(SYS_SIGPROCMASK, SYS_SIG_UNBLOCK, ptr32(&g_fdtest_sigmask), ptr32(&g_fdtest_old_sigmask),
                   0u, 0u, 0u, ret, err);
    if (err != 0u || ret != 0u || (g_fdtest_old_sigmask & g_fdtest_sigmask) == 0u) {
        fdtest_fail("sigprocmask(UNBLOCK)", g_fdtest_old_sigmask & g_fdtest_sigmask, g_fdtest_sigmask, &fails);
    }
    FDTEST_SYSCALL(SYS_SIGPROCMASK, 0xFFFFu, ptr32(&g_fdtest_sigmask), 0u, 0u, 0u, 0u, ret, err);
    if ((int32_t)ret != -1 || err != TEST_ERRNO_EINVAL) {
        fdtest_fail("sigprocmask(bad how)", err, TEST_ERRNO_EINVAL, &fails);
    }

    g_fdtest_signal_child_started = 0u;
    {
        int32_t sig_tid = sched_spawn("fdsig", fdtest_signal_child_task, 0);
        if (sig_tid < 0) {
            fdtest_fail("spawn signal child", (uint32_t)sig_tid, 1u, &fails);
        } else {
            uint32_t sig_status;
            for (i = 0u; i < 20u && g_fdtest_signal_child_started == 0u; i++) {
                sched_yield();
            }
            FDTEST_SYSCALL(SYS_KILL, (uint32_t)sig_tid, SYS_SIGTERM, 0u, 0u, 0u, 0u, ret, err);
            if (err != 0u || ret != 0u) {
                fdtest_fail("kill(child SIGTERM)", err, 0u, &fails);
            }
            sig_status = fdtest_wait_child_reap_status(sig_tid, &fails);
            if (sig_status != ((128u + SYS_SIGTERM) << 8u)) {
                fdtest_fail("wait killed child", sig_status, (128u + SYS_SIGTERM) << 8u, &fails);
            }
        }
    }

    FDTEST_SYSCALL(SYS_UMASK, 077u, 0u, 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || ret != 022u) {
        fdtest_fail("umask set", ret, 022u, &fails);
    }
    FDTEST_SYSCALL(SYS_UMASK, 022u, 0u, 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || ret != 077u) {
        fdtest_fail("umask restore", ret, 077u, &fails);
    }

    FDTEST_SYSCALL(SYS_MKDIR, ptr32(g_fdtest_path_tmp_fdtest), 0777u, 0u, 0u, 0u, 0u, ret, err);
    if ((int32_t)ret != -1 || err != TEST_ERRNO_EROFS) {
        fdtest_fail("mkdir readonly", err, TEST_ERRNO_EROFS, &fails);
    }
    FDTEST_SYSCALL(SYS_UNLINK, ptr32(g_fdtest_path_bin_hello), 0u, 0u, 0u, 0u, 0u, ret, err);
    if ((int32_t)ret != -1 || err != TEST_ERRNO_EROFS) {
        fdtest_fail("unlink readonly", err, TEST_ERRNO_EROFS, &fails);
    }
    FDTEST_SYSCALL(SYS_RMDIR, ptr32(g_fdtest_path_bin), 0u, 0u, 0u, 0u, 0u, ret, err);
    if ((int32_t)ret != -1 || err != TEST_ERRNO_EROFS) {
        fdtest_fail("rmdir readonly", err, TEST_ERRNO_EROFS, &fails);
    }
    FDTEST_SYSCALL(SYS_RENAME, ptr32(g_fdtest_path_bin_hello), ptr32(g_fdtest_path_tmp_fdtest),
                   0u, 0u, 0u, 0u, ret, err);
    if ((int32_t)ret != -1 || err != TEST_ERRNO_EROFS) {
        fdtest_fail("rename readonly", err, TEST_ERRNO_EROFS, &fails);
    }
    FDTEST_SYSCALL(SYS_LINK, ptr32(g_fdtest_path_bin_hello), ptr32(g_fdtest_path_tmp_fdtest),
                   0u, 0u, 0u, 0u, ret, err);
    if ((int32_t)ret != -1 || err != TEST_ERRNO_EROFS) {
        fdtest_fail("link readonly", err, TEST_ERRNO_EROFS, &fails);
    }
    FDTEST_SYSCALL(SYS_SYMLINK, ptr32(g_fdtest_path_bin_hello), ptr32(g_fdtest_path_tmp_fdtest),
                   0u, 0u, 0u, 0u, ret, err);
    if ((int32_t)ret != -1 || err != TEST_ERRNO_EROFS) {
        fdtest_fail("symlink readonly", err, TEST_ERRNO_EROFS, &fails);
    }
    FDTEST_SYSCALL(SYS_READLINK, ptr32(g_fdtest_path_bin_hello), ptr32(g_fdtest_buf),
                   (uint32_t)sizeof(g_fdtest_buf), 0u, 0u, 0u, ret, err);
    if ((int32_t)ret != -1 || err != TEST_ERRNO_EINVAL) {
        fdtest_fail("readlink non-symlink", err, TEST_ERRNO_EINVAL, &fails);
    }
    FDTEST_SYSCALL(SYS_READLINK, ptr32(g_fdtest_path_missing), ptr32(g_fdtest_buf),
                   (uint32_t)sizeof(g_fdtest_buf), 0u, 0u, 0u, ret, err);
    if ((int32_t)ret != -1 || err != TEST_ERRNO_ENOENT) {
        fdtest_fail("readlink missing", err, TEST_ERRNO_ENOENT, &fails);
    }

    FDTEST_SYSCALL(SYS_PIPE, ptr32(g_fdtest_pipefd), 0u, 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || ret != 0u || g_fdtest_pipefd[0] < 0 || g_fdtest_pipefd[1] < 0) {
        fdtest_fail("pipe()", err, 0u, &fails);
    } else {
        fdtest_stat32_t *pst = &g_fdtest_fstat;
        pfd->fd = g_fdtest_pipefd[0];
        pfd->events = (int16_t)SYS_POLLIN;
        pfd->revents = 0;
        FDTEST_SYSCALL(SYS_POLL, ptr32(pfd), 1u, 0u, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 0u || pfd->revents != 0) {
            fdtest_fail("poll(empty pipe)", ret, 0u, &fails);
        }

        FDTEST_SYSCALL(SYS_FSTAT, (uint32_t)g_fdtest_pipefd[0], ptr32(pst), 0u, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 0u || (pst->st_mode & SYS_S_IFMT) != SYS_S_IFIFO) {
            fdtest_fail("fstat(pipe)", pst->st_mode & SYS_S_IFMT, SYS_S_IFIFO, &fails);
        }

        FDTEST_SYSCALL(SYS_WRITE, (uint32_t)g_fdtest_pipefd[1], ptr32("pipe"), 4u, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 4u) {
            fdtest_fail("write(pipe)", ret, 4u, &fails);
        }
        pfd->revents = 0;
        FDTEST_SYSCALL(SYS_POLL, ptr32(pfd), 1u, 0u, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 1u || ((uint16_t)pfd->revents & SYS_POLLIN) == 0u) {
            fdtest_fail("poll(pipe ready)", (uint32_t)(uint16_t)pfd->revents, SYS_POLLIN, &fails);
        }
        FDTEST_SYSCALL(SYS_READ, (uint32_t)g_fdtest_pipefd[0], ptr32(g_fdtest_buf), 4u, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 4u || g_fdtest_buf[0] != (uint8_t)'p' ||
            g_fdtest_buf[1] != (uint8_t)'i' || g_fdtest_buf[2] != (uint8_t)'p' ||
            g_fdtest_buf[3] != (uint8_t)'e') {
            fdtest_fail("read(pipe)", ret, 4u, &fails);
        }
        FDTEST_SYSCALL(SYS_FCNTL, (uint32_t)g_fdtest_pipefd[0], SYS_FCNTL_F_SETFL, SYS_O_NONBLOCK,
                       0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 0u) {
            fdtest_fail("pipe read nonblock", err, 0u, &fails);
        }
        FDTEST_SYSCALL(SYS_READ, (uint32_t)g_fdtest_pipefd[0], ptr32(g_fdtest_buf), 1u, 0u, 0u, 0u, ret, err);
        if ((int32_t)ret != -1 || err != TEST_ERRNO_EAGAIN) {
            fdtest_fail("read(empty pipe nonblock)", err, TEST_ERRNO_EAGAIN, &fails);
        }
        FDTEST_SYSCALL(SYS_CLOSE, (uint32_t)g_fdtest_pipefd[1], 0u, 0u, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 0u) {
            fdtest_fail("close(pipe write)", err, 0u, &fails);
        }
        FDTEST_SYSCALL(SYS_READ, (uint32_t)g_fdtest_pipefd[0], ptr32(g_fdtest_buf), 1u, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 0u) {
            fdtest_fail("read(pipe EOF)", ret, 0u, &fails);
        }
        FDTEST_SYSCALL(SYS_CLOSE, (uint32_t)g_fdtest_pipefd[0], 0u, 0u, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 0u) {
            fdtest_fail("close(pipe read)", err, 0u, &fails);
        }
    }

    FDTEST_SYSCALL(SYS_PIPE, ptr32(g_fdtest_pipefd), 0u, 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || ret != 0u) {
        fdtest_fail("pipe(EPIPE setup)", err, 0u, &fails);
    } else {
        FDTEST_SYSCALL(SYS_CLOSE, (uint32_t)g_fdtest_pipefd[0], 0u, 0u, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 0u) {
            fdtest_fail("close(pipe read for EPIPE)", err, 0u, &fails);
        }
        FDTEST_SYSCALL(SYS_WRITE, (uint32_t)g_fdtest_pipefd[1], ptr32("x"), 1u, 0u, 0u, 0u, ret, err);
        if ((int32_t)ret != -1 || err != TEST_ERRNO_EPIPE) {
            fdtest_fail("write(pipe no readers)", err, TEST_ERRNO_EPIPE, &fails);
        }
        FDTEST_SYSCALL(SYS_CLOSE, (uint32_t)g_fdtest_pipefd[1], 0u, 0u, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 0u) {
            fdtest_fail("close(pipe write for EPIPE)", err, 0u, &fails);
        }
    }

    FDTEST_SYSCALL(SYS_OPEN, ptr32(g_fdtest_path_dev_null), SYS_O_RDWR, 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || (int32_t)ret < 0) {
        fdtest_fail("open(/dev/null)", err, 0u, &fails);
    } else {
        fd = ret;
        FDTEST_SYSCALL(SYS_READ, fd, ptr32(g_fdtest_buf), 8u, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 0u) {
            fdtest_fail("read(/dev/null)", err, 0u, &fails);
            fdtest_fail("read(/dev/null) ret", ret, 0u, &fails);
        }
        FDTEST_SYSCALL(SYS_WRITE, fd, ptr32("x"), 1u, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 1u) {
            fdtest_fail("write(/dev/null)", err, 0u, &fails);
            fdtest_fail("write(/dev/null) ret", ret, 1u, &fails);
        }
        FDTEST_SYSCALL(SYS_IOCTL, fd, SYS_IOCTL_TCGETS, ptr32(&g_fdtest_termios), 0u, 0u, 0u, ret, err);
        if ((int32_t)ret != -1 || err != TEST_ERRNO_ENOTTY) {
            fdtest_fail("ioctl(/dev/null)", err, TEST_ERRNO_ENOTTY, &fails);
        }
        FDTEST_SYSCALL(SYS_CLOSE, fd, 0u, 0u, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 0u) {
            fdtest_fail("close(/dev/null)", err, 0u, &fails);
        }
    }

    for (i = 0u; i < 8u; i++) {
        g_fdtest_buf[i] = 0xA5u;
    }
    FDTEST_SYSCALL(SYS_OPEN, ptr32(g_fdtest_path_dev_zero), SYS_O_RDONLY, 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || (int32_t)ret < 0) {
        fdtest_fail("open(/dev/zero)", err, 0u, &fails);
    } else {
        fd = ret;
        FDTEST_SYSCALL(SYS_READ, fd, ptr32(g_fdtest_buf), 8u, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 8u) {
            fdtest_fail("read(/dev/zero)", err, 0u, &fails);
            fdtest_fail("read(/dev/zero) ret", ret, 8u, &fails);
        }
        for (i = 0u; i < 8u; i++) {
            if (g_fdtest_buf[i] != 0u) {
                fdtest_fail("read(/dev/zero) byte", g_fdtest_buf[i], 0u, &fails);
                break;
            }
        }
        FDTEST_SYSCALL(SYS_CLOSE, fd, 0u, 0u, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 0u) {
            fdtest_fail("close(/dev/zero)", err, 0u, &fails);
        }
    }

    FDTEST_SYSCALL(SYS_OPEN, ptr32(g_fdtest_path_dev_tty), SYS_O_RDWR, 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || (int32_t)ret < 0) {
        fdtest_fail("open(/dev/tty)", err, 0u, &fails);
    } else {
        fd = ret;
        FDTEST_SYSCALL(SYS_CLOSE, fd, 0u, 0u, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 0u) {
            fdtest_fail("close(/dev/tty)", err, 0u, &fails);
        }
    }

    FDTEST_SYSCALL(SYS_OPEN, ptr32(g_fdtest_path_missing), SYS_O_RDONLY, 0u, 0u, 0u, 0u, ret, err);
    if ((int32_t)ret != -1 || err != TEST_ERRNO_ENOENT) {
        fdtest_fail("open(/dev/missing) errno", err, TEST_ERRNO_ENOENT, &fails);
    }

    FDTEST_SYSCALL(SYS_STAT, ptr32(g_fdtest_path_dev_null), ptr32(&g_fdtest_stat), 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || ret != 0u || (g_fdtest_stat.st_mode & SYS_S_IFMT) != SYS_S_IFCHR) {
        fdtest_fail("stat(/dev/null)", g_fdtest_stat.st_mode & SYS_S_IFMT, SYS_S_IFCHR, &fails);
    }

    FDTEST_SYSCALL(SYS_STAT, ptr32(g_fdtest_path_bin), ptr32(&g_fdtest_stat), 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || ret != 0u || (g_fdtest_stat.st_mode & SYS_S_IFMT) != SYS_S_IFDIR) {
        fdtest_fail("stat(/bin)", g_fdtest_stat.st_mode & SYS_S_IFMT, SYS_S_IFDIR, &fails);
    }

    FDTEST_SYSCALL(SYS_GETCWD, ptr32(g_fdtest_cwd), (uint32_t)sizeof(g_fdtest_cwd), 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || ret != ptr32(g_fdtest_cwd) || !fdtest_streq(g_fdtest_cwd, "/")) {
        fdtest_fail("getcwd(/)", err, 0u, &fails);
    }
    FDTEST_SYSCALL(SYS_CHDIR, ptr32(g_fdtest_path_bin), 0u, 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || ret != 0u) {
        fdtest_fail("chdir(/bin)", err, 0u, &fails);
    }
    FDTEST_SYSCALL(SYS_GETCWD, ptr32(g_fdtest_cwd), 2u, 0u, 0u, 0u, 0u, ret, err);
    if ((int32_t)ret != -1 || err != TEST_ERRNO_ERANGE) {
        fdtest_fail("getcwd small", err, TEST_ERRNO_ERANGE, &fails);
    }
    FDTEST_SYSCALL(SYS_GETCWD, ptr32(g_fdtest_cwd), (uint32_t)sizeof(g_fdtest_cwd), 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || ret != ptr32(g_fdtest_cwd) || !fdtest_streq(g_fdtest_cwd, "/bin")) {
        fdtest_fail("getcwd(/bin)", err, 0u, &fails);
    }
    FDTEST_SYSCALL(SYS_STAT, ptr32("hello"), ptr32(&g_fdtest_stat), 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || ret != 0u || (g_fdtest_stat.st_mode & SYS_S_IFMT) != SYS_S_IFREG) {
        fdtest_fail("stat(relative hello)", g_fdtest_stat.st_mode & SYS_S_IFMT, SYS_S_IFREG, &fails);
    }
    FDTEST_SYSCALL(SYS_ACCESS, ptr32("hello"), SYS_F_OK, 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || ret != 0u) {
        fdtest_fail("access(relative hello)", err, 0u, &fails);
    }
    FDTEST_SYSCALL(SYS_OPEN, ptr32("hello"), SYS_O_RDONLY, 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || (int32_t)ret < 0) {
        fdtest_fail("open(relative hello)", err, 0u, &fails);
    } else {
        fd = ret;
        FDTEST_SYSCALL(SYS_CLOSE, fd, 0u, 0u, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 0u) {
            fdtest_fail("close(relative hello)", err, 0u, &fails);
        }
    }
    FDTEST_SYSCALL(SYS_CHDIR, ptr32(".."), 0u, 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || ret != 0u) {
        fdtest_fail("chdir(..)", err, 0u, &fails);
    }
    FDTEST_SYSCALL(SYS_GETCWD, ptr32(g_fdtest_cwd), (uint32_t)sizeof(g_fdtest_cwd), 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || !fdtest_streq(g_fdtest_cwd, "/")) {
        fdtest_fail("getcwd(back root)", err, 0u, &fails);
    }
    FDTEST_SYSCALL(SYS_CHDIR, ptr32("bin/.."), 0u, 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || ret != 0u) {
        fdtest_fail("chdir(bin/..)", err, 0u, &fails);
    }
    FDTEST_SYSCALL(SYS_CHDIR, ptr32(g_fdtest_path_bin_hello), 0u, 0u, 0u, 0u, 0u, ret, err);
    if ((int32_t)ret != -1 || err != TEST_ERRNO_ENOTDIR) {
        fdtest_fail("chdir(file)", err, TEST_ERRNO_ENOTDIR, &fails);
    }

    FDTEST_SYSCALL(SYS_ACCESS, ptr32(g_fdtest_path_bin_hello), SYS_F_OK, 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || ret != 0u) {
        fdtest_fail("access(/bin/hello,F_OK)", err, 0u, &fails);
    }
    FDTEST_SYSCALL(SYS_ACCESS, ptr32(g_fdtest_path_bin_hello), SYS_R_OK | SYS_X_OK, 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || ret != 0u) {
        fdtest_fail("access(/bin/hello,RX)", err, 0u, &fails);
    }
    FDTEST_SYSCALL(SYS_ACCESS, ptr32(g_fdtest_path_missing), SYS_F_OK, 0u, 0u, 0u, 0u, ret, err);
    if ((int32_t)ret != -1 || err != TEST_ERRNO_ENOENT) {
        fdtest_fail("access(missing)", err, TEST_ERRNO_ENOENT, &fails);
    }
    FDTEST_SYSCALL(SYS_ACCESS, ptr32(g_fdtest_path_bin_hello), 0x80u, 0u, 0u, 0u, 0u, ret, err);
    if ((int32_t)ret != -1 || err != TEST_ERRNO_EINVAL) {
        fdtest_fail("access(bad mode)", err, TEST_ERRNO_EINVAL, &fails);
    }

    FDTEST_SYSCALL(SYS_OPEN, ptr32(g_fdtest_path_bin), SYS_O_RDONLY, 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || (int32_t)ret < 0) {
        fdtest_fail("open(/bin)", err, 0u, &fails);
    } else {
        uint32_t found_hello = 0u;
        fd = ret;
        FDTEST_SYSCALL(SYS_READ, fd, ptr32(g_fdtest_buf), 1u, 0u, 0u, 0u, ret, err);
        if ((int32_t)ret != -1 || err != TEST_ERRNO_EISDIR) {
            fdtest_fail("read(/bin dir)", err, TEST_ERRNO_EISDIR, &fails);
        }
        FDTEST_SYSCALL(SYS_GETDENTS, fd, ptr32(g_fdtest_dirents), (uint32_t)sizeof(g_fdtest_dirents),
                       0u, 0u, 0u, ret, err);
        if (err != 0u || ret == 0u || (ret % (uint32_t)sizeof(g_fdtest_dirents[0])) != 0u) {
            fdtest_fail("getdents(/bin)", ret, (uint32_t)sizeof(g_fdtest_dirents[0]), &fails);
        } else {
            uint32_t nents = ret / (uint32_t)sizeof(g_fdtest_dirents[0]);
            for (i = 0u; i < nents && i < 8u; i++) {
                if (g_fdtest_dirents[i].d_reclen != (uint32_t)sizeof(g_fdtest_dirents[0])) {
                    fdtest_fail("getdents reclen", g_fdtest_dirents[i].d_reclen,
                                (uint32_t)sizeof(g_fdtest_dirents[0]), &fails);
                    break;
                }
                if (fdtest_streq(g_fdtest_dirents[i].d_name, "hello")) {
                    found_hello = 1u;
                    if (g_fdtest_dirents[i].d_type != SYS_DT_REG) {
                        fdtest_fail("getdents hello type", g_fdtest_dirents[i].d_type, SYS_DT_REG, &fails);
                    }
                }
            }
            if (found_hello == 0u) {
                fdtest_fail("getdents hello", found_hello, 1u, &fails);
            }
        }
        FDTEST_SYSCALL(SYS_LSEEK, fd, 0u, SYS_SEEK_SET, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 0u) {
            fdtest_fail("lseek(/bin dir)", err, 0u, &fails);
        }
        FDTEST_SYSCALL(SYS_CLOSE, fd, 0u, 0u, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 0u) {
            fdtest_fail("close(/bin)", err, 0u, &fails);
        }
    }

    FDTEST_SYSCALL(SYS_OPEN, ptr32(g_fdtest_path_bin_hello), SYS_O_RDONLY, 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || (int32_t)ret < 0) {
        fdtest_fail("open(/bin/hello)", err, 0u, &fails);
    } else {
        fd = ret;
        FDTEST_SYSCALL(SYS_STAT, ptr32(g_fdtest_path_bin_hello), ptr32(&g_fdtest_stat), 0u, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 0u || (g_fdtest_stat.st_mode & SYS_S_IFMT) != SYS_S_IFREG ||
            g_fdtest_stat.st_size == 0u) {
            fdtest_fail("stat(/bin/hello)", g_fdtest_stat.st_mode & SYS_S_IFMT, SYS_S_IFREG, &fails);
        }
        FDTEST_SYSCALL(SYS_FSTAT, fd, ptr32(&g_fdtest_fstat), 0u, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 0u || (g_fdtest_fstat.st_mode & SYS_S_IFMT) != SYS_S_IFREG ||
            g_fdtest_fstat.st_size != g_fdtest_stat.st_size) {
            fdtest_fail("fstat(/bin/hello)", g_fdtest_fstat.st_size, g_fdtest_stat.st_size, &fails);
        }
        FDTEST_SYSCALL(SYS_READ, fd, ptr32(g_fdtest_buf), 4u, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 4u || g_fdtest_buf[0] != 0x7Fu ||
            g_fdtest_buf[1] != (uint8_t)'E' || g_fdtest_buf[2] != (uint8_t)'L' ||
            g_fdtest_buf[3] != (uint8_t)'F') {
            fdtest_fail("read(/bin/hello) elf", ret, 4u, &fails);
        }

        FDTEST_SYSCALL(SYS_LSEEK, fd, 0u, SYS_SEEK_SET, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 0u) {
            fdtest_fail("lseek(SET)", err, 0u, &fails);
        }
        FDTEST_SYSCALL(SYS_READ, fd, ptr32(g_fdtest_buf), 4u, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 4u || g_fdtest_buf[0] != 0x7Fu ||
            g_fdtest_buf[1] != (uint8_t)'E' || g_fdtest_buf[2] != (uint8_t)'L' ||
            g_fdtest_buf[3] != (uint8_t)'F') {
            fdtest_fail("read after lseek", ret, 4u, &fails);
        }
        FDTEST_SYSCALL(SYS_LSEEK, fd, (uint32_t)-2, SYS_SEEK_CUR, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 2u) {
            fdtest_fail("lseek(CUR,-2)", ret, 2u, &fails);
        }
        FDTEST_SYSCALL(SYS_LSEEK, fd, 0u, SYS_SEEK_END, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret == 0u) {
            fdtest_fail("lseek(END)", ret, 1u, &fails);
        }
        FDTEST_SYSCALL(SYS_READ, fd, ptr32(g_fdtest_buf), 1u, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 0u) {
            fdtest_fail("read EOF after lseek", ret, 0u, &fails);
        }
        FDTEST_SYSCALL(SYS_LSEEK, fd, (uint32_t)-1, SYS_SEEK_SET, 0u, 0u, 0u, ret, err);
        if ((int32_t)ret != -1 || err != TEST_ERRNO_EINVAL) {
            fdtest_fail("lseek negative SET", err, TEST_ERRNO_EINVAL, &fails);
        }
        FDTEST_SYSCALL(SYS_CLOSE, fd, 0u, 0u, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 0u) {
            fdtest_fail("close(/bin/hello)", err, 0u, &fails);
        }
    }

    FDTEST_SYSCALL(SYS_LSEEK, 0u, 0u, SYS_SEEK_SET, 0u, 0u, 0u, ret, err);
    if ((int32_t)ret != -1 || err != TEST_ERRNO_ESPIPE) {
        fdtest_fail("lseek(stdin)", err, TEST_ERRNO_ESPIPE, &fails);
    }

    FDTEST_SYSCALL(SYS_FSTAT, 0u, ptr32(&g_fdtest_fstat), 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || ret != 0u || (g_fdtest_fstat.st_mode & SYS_S_IFMT) != SYS_S_IFCHR) {
        fdtest_fail("fstat(stdin)", g_fdtest_fstat.st_mode & SYS_S_IFMT, SYS_S_IFCHR, &fails);
    }

    FDTEST_SYSCALL(SYS_GETDENTS, 0u, ptr32(g_fdtest_dirents), (uint32_t)sizeof(g_fdtest_dirents[0]),
                   0u, 0u, 0u, ret, err);
    if ((int32_t)ret != -1 || err != TEST_ERRNO_ENOTDIR) {
        fdtest_fail("getdents(stdin)", err, TEST_ERRNO_ENOTDIR, &fails);
    }

    FDTEST_SYSCALL(SYS_SOCKET, 0xFFFFu, 1u, 0u, 0u, 0u, 0u, ret, err);
    if ((int32_t)ret != -1 || err != TEST_ERRNO_EAFNOSUPPORT) {
        fdtest_fail("socket(bad domain)", err, TEST_ERRNO_EAFNOSUPPORT, &fails);
    }

    FDTEST_SYSCALL(SYS_SOCKET, 2u, 1u, 0u, 0u, 0u, 0u, ret, err);
    if (err != 0u || (int32_t)ret < 0) {
        fdtest_fail("socket(AF_INET)", err, 0u, &fails);
    } else {
        fd = ret;
        FDTEST_SYSCALL(SYS_FSTAT, fd, ptr32(&g_fdtest_fstat), 0u, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 0u || (g_fdtest_fstat.st_mode & SYS_S_IFMT) != SYS_S_IFSOCK) {
            fdtest_fail("fstat(socket)", g_fdtest_fstat.st_mode & SYS_S_IFMT, SYS_S_IFSOCK, &fails);
        }

        FDTEST_SYSCALL(SYS_BIND, fd, 0u, 0u, 0u, 0u, 0u, ret, err);
        if ((int32_t)ret != -1 || err != TEST_ERRNO_EOPNOTSUPP) {
            fdtest_fail("bind(socket)", err, TEST_ERRNO_EOPNOTSUPP, &fails);
        }

        FDTEST_SYSCALL(SYS_SEND, fd, ptr32("x"), 1u, 0u, 0u, 0u, ret, err);
        if ((int32_t)ret != -1 || err != TEST_ERRNO_ENOTCONN) {
            fdtest_fail("send(socket)", err, TEST_ERRNO_ENOTCONN, &fails);
        }

        FDTEST_SYSCALL(SYS_RECV, fd, ptr32(g_fdtest_buf), 1u, 0u, 0u, 0u, ret, err);
        if ((int32_t)ret != -1 || err != TEST_ERRNO_ENOTCONN) {
            fdtest_fail("recv(socket)", err, TEST_ERRNO_ENOTCONN, &fails);
        }

        FDTEST_SYSCALL(SYS_CLOSE, fd, 0u, 0u, 0u, 0u, 0u, ret, err);
        if (err != 0u || ret != 0u) {
            fdtest_fail("close(socket)", err, 0u, &fails);
        }
    }

    FDTEST_SYSCALL(SYS_BIND, 0u, 0u, 0u, 0u, 0u, 0u, ret, err);
    if ((int32_t)ret != -1 || err != TEST_ERRNO_ENOTSOCK) {
        fdtest_fail("bind(non-socket)", err, TEST_ERRNO_ENOTSOCK, &fails);
    }

    fdtest_waitq_stress(&fails);
    fdtest_poll_stress(&fails);
    fdtest_select_stress(&fails);

    if (fails == 0u) {
        KLOGI("fdtest", "pass");
    } else {
        klog_begin(KLOG_LEVEL_ERROR, "fdtest");
        klog_puts("fail count=");
        klog_hex32(fails);
        klog_end();
    }

out_restore_tty:
    /*
     * Keep init shell clean even if stress feeder left residual input:
     * drain in raw(non-canonical) mode so stale line counters cannot block flushing.
     */
    (void)console_tty_set_lflag((saved_tty_lflag & ~(TTY_LFLAG_ECHO | TTY_LFLAG_ICANON)));
    fdtest_drain_stdin();
    (void)console_tty_set_lflag(saved_tty_lflag);
    spinlock_lock(&g_fdtest_run_lock);
    g_fdtest_running = 0u;
    spinlock_unlock(&g_fdtest_run_lock);
}
