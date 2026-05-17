#include "../include/kernel/console.h"
#include "../include/kernel/console_fb.h"
#include "../include/kernel/fd_selftest.h"
#include "../include/kernel/fs.h"
#include "../include/kernel/init_task.h"
#include "../include/kernel/printk.h"
#include "../include/kernel/sched.h"
#include "../include/kernel/spinlock.h"
#include "../include/kernel/types.h"
#include "../include/kernel/user_exec.h"

enum {
    INIT_LINE_CAP = 128u
};

typedef struct init_state {
    uint8_t line[INIT_LINE_CAP];
    uint32_t len;
    uint32_t started;
} init_state_t;

static init_state_t g_init_state;
static spinlock_t g_init_owner_lock;
static spinlock_t g_init_cmd_lock;
static volatile uint32_t g_init_owner_tid;
static volatile uint32_t g_init_busy;
static volatile uint32_t g_init_fdtest_running;

static void init_puts(const char *s) {
    uint32_t len = 0u;
    if (!s) {
        return;
    }
    while (s[len] != '\0') {
        len++;
    }
    if (len != 0u) {
        (void)console_write((const uint8_t *)s, len);
    }
}

static uint32_t init_streq(const char *a, const char *b) {
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

static uint32_t init_starts_with(const char *s, const char *prefix) {
    uint32_t i = 0u;
    if (!s || !prefix) {
        return 0u;
    }
    while (prefix[i] != '\0') {
        if (s[i] != prefix[i]) {
            return 0u;
        }
        i++;
    }
    return 1u;
}

static void init_prompt(void) {
    init_puts("init$ ");
}

static void init_show_tty_mode(void) {
    init_puts("tty lflag=");
    kprint_hex32(console_tty_get_lflag());
    init_puts(" [");
    if ((console_tty_get_lflag() & TTY_LFLAG_ECHO) != 0u) {
        init_puts("ECHO ");
    }
    if ((console_tty_get_lflag() & TTY_LFLAG_ICANON) != 0u) {
        init_puts("ICANON ");
    }
    if ((console_tty_get_lflag() & TTY_LFLAG_ISIG) != 0u) {
        init_puts("ISIG ");
    }
    init_puts("]\n");
}

static void init_set_flag(uint32_t flag, uint32_t on) {
    uint32_t v = console_tty_get_lflag();
    if (on) {
        v |= flag;
    } else {
        v &= ~flag;
    }
    (void)console_tty_set_lflag(v);
    init_show_tty_mode();
}

static void init_show_poll_state(void) {
    init_puts("stdin can_read=");
    kprint_hex32(console_can_read());
    init_puts(" lines=");
    kprint_hex32(console_rx_lines());
    init_puts(" dropped=");
    kprint_hex32(console_rx_dropped());
    init_puts("\n");
}

static void init_set_log_level(const char *lvl) {
    if (init_streq(lvl, "err")) {
        klog_set_level(KLOG_LEVEL_ERROR);
    } else if (init_streq(lvl, "warn")) {
        klog_set_level(KLOG_LEVEL_WARN);
    } else if (init_streq(lvl, "info")) {
        klog_set_level(KLOG_LEVEL_INFO);
    } else if (init_streq(lvl, "debug")) {
        klog_set_level(KLOG_LEVEL_DEBUG);
    } else {
        init_puts("usage: log <err|warn|info|debug>\n");
        return;
    }
    init_puts("log level=");
    kprint_hex32(klog_get_level());
    init_puts("\n");
}

static uint32_t init_parse_u32(const char *s, uint32_t *out) {
    uint32_t v = 0u;
    uint32_t seen = 0u;
    if (!s || !out) {
        return 0u;
    }
    while (*s == ' ') {
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        uint32_t digit = (uint32_t)(*s - '0');
        if (v > 429496729u || (v == 429496729u && digit > 5u)) {
            return 0u;
        }
        v = v * 10u + digit;
        seen = 1u;
        s++;
    }
    while (*s == ' ') {
        s++;
    }
    if (!seen || *s != '\0') {
        return 0u;
    }
    *out = v;
    return 1u;
}

static void init_wait_child_and_report_tag(const char *tag, int32_t tid) {
    uint32_t status = 0u;
    uint32_t start_tick = sched_ticks();
    const uint32_t wait_timeout_ticks = 2000u; /* 10s @ 5ms tick */
    for (;;) {
        int rc = sched_waitpid(tid, SCHED_WAITPID_WNOHANG, &status);
        if (rc == 0) {
            if ((sched_ticks() - start_tick) >= wait_timeout_ticks) {
                if (tag) {
                    init_puts(tag);
                    init_puts(" ");
                }
                init_puts("waitpid timeout\n");
                return;
            }
            sched_sleep_ticks(1u);
            continue;
        }
        if (rc <= 0) {
            if (tag) {
                init_puts(tag);
                init_puts(" ");
            }
            init_puts("waitpid failed\n");
            return;
        }
        if (tag) {
            init_puts(tag);
            init_puts(" ");
        }
        init_puts("exit status=");
        kprint_hex32((status >> 8u) & 0xFFu);
        init_puts("\n");
        return;
    }
}

static void init_wait_child_and_report(int32_t tid) {
    init_wait_child_and_report_tag("uhello", tid);
}

static void init_wait_child_and_report_tag_forever(const char *tag, int32_t tid) {
    uint32_t status = 0u;
    for (;;) {
        int rc = sched_waitpid(tid, SCHED_WAITPID_WNOHANG, &status);
        if (rc == 0) {
            sched_sleep_ticks(1u);
            continue;
        }
        if (rc <= 0) {
            if (tag) {
                init_puts(tag);
                init_puts(" ");
            }
            init_puts("waitpid failed\n");
            return;
        }
        if (tag) {
            init_puts(tag);
            init_puts(" ");
        }
        init_puts("exit status=");
        kprint_hex32((status >> 8u) & 0xFFu);
        init_puts("\n");
        return;
    }
}

static void init_report_spawn_failed(const char *tag, int32_t rc, const char *path) {
    if (tag) {
        init_puts(tag);
        init_puts(" ");
    }
    init_puts("spawn failed rc=");
    kprint_hex32((uint32_t)rc);
    init_puts("\n");
    if (rc == FS_ERR_NOENT && path) {
        init_puts("missing user binary: ");
        init_puts(path);
        init_puts("\n");
        init_puts("hint: run `bash user/install_m1_to_disk.sh`\n");
    }
}

static void init_run_uhello(uint32_t count) {
    static const char *const argv[] = {"/bin/hello", 0};
    if (count == 0u) {
        init_puts("uhello count must be >= 1\n");
        return;
    }
    for (uint32_t i = 0u; i < count; i++) {
        int32_t tid = user_exec_spawn_path("/bin/hello", argv, 0);
        if (tid < 0) {
            init_report_spawn_failed("uhello", tid, "/bin/hello");
            return;
        }
        init_puts("uhello tid=");
        kprint_hex32((uint32_t)tid);
        if (count > 1u) {
            init_puts(" run=");
            kprint_hex32(i + 1u);
            init_puts("/");
            kprint_hex32(count);
        }
        init_puts("\n");
        init_wait_child_and_report(tid);
    }
}

static void init_run_uvfork(uint32_t count) {
    static const char *const argv[] = {"/bin/vfork_exec", 0};
    if (count == 0u) {
        init_puts("uvfork count must be >= 1\n");
        return;
    }
    for (uint32_t i = 0u; i < count; i++) {
        int32_t tid = user_exec_spawn_path("/bin/vfork_exec", argv, 0);
        if (tid < 0) {
            init_report_spawn_failed("uvfork", tid, "/bin/vfork_exec");
            return;
        }
        init_puts("uvfork tid=");
        kprint_hex32((uint32_t)tid);
        if (count > 1u) {
            init_puts(" run=");
            kprint_hex32(i + 1u);
            init_puts("/");
            kprint_hex32(count);
        }
        init_puts("\n");
        init_wait_child_and_report_tag("uvfork", tid);
    }
}

static void init_run_upipe(uint32_t count) {
    static const char *const argv[] = {"/bin/pipe_exec", 0};
    if (count == 0u) {
        init_puts("upipe count must be >= 1\n");
        return;
    }
    for (uint32_t i = 0u; i < count; i++) {
        int32_t tid = user_exec_spawn_path("/bin/pipe_exec", argv, 0);
        if (tid < 0) {
            init_report_spawn_failed("upipe", tid, "/bin/pipe_exec");
            return;
        }
        init_puts("upipe tid=");
        kprint_hex32((uint32_t)tid);
        if (count > 1u) {
            init_puts(" run=");
            kprint_hex32(i + 1u);
            init_puts("/");
            kprint_hex32(count);
        }
        init_puts("\n");
        init_wait_child_and_report_tag("upipe", tid);
    }
}

static void init_run_upwd(void) {
    static const char *const argv[] = {"/bin/pwd", 0};
    int32_t tid = user_exec_spawn_path("/bin/pwd", argv, 0);
    if (tid < 0) {
        init_report_spawn_failed("upwd", tid, "/bin/pwd");
        return;
    }
    init_puts("upwd tid=");
    kprint_hex32((uint32_t)tid);
    init_puts("\n");
    init_wait_child_and_report_tag("upwd", tid);
}

static void init_run_uls(const char *path) {
    static const char *const argv_default[] = {"/bin/ls", 0};
    const char *argv_path[] = {"/bin/ls", path, 0};
    const char *const *argv = path && path[0] != '\0' ? argv_path : argv_default;
    int32_t tid = user_exec_spawn_path("/bin/ls", argv, 0);
    if (tid < 0) {
        init_report_spawn_failed("uls", tid, "/bin/ls");
        return;
    }
    init_puts("uls tid=");
    kprint_hex32((uint32_t)tid);
    init_puts("\n");
    init_wait_child_and_report_tag("uls", tid);
}

static void init_run_ush(void) {
    static const char *const argv[] = {"-/bin/sh", 0};
    int32_t tid = user_exec_spawn_path("/bin/sh", argv, 0);
    if (tid < 0) {
        init_report_spawn_failed("ush", tid, "/bin/sh");
        return;
    }
    init_puts("ush tid=");
    kprint_hex32((uint32_t)tid);
    init_puts("\n");
    init_wait_child_and_report_tag_forever("ush", tid);
}

static void init_run_uexec() {

}

static void init_handle_cmd(char *line) {
    while (*line == ' ') {
        line++;
    }
    if (*line == '\0') {
        return;
    }

    if (init_streq(line, "help")) {
        init_puts("commands:\n");
        init_puts("  help\n");
        init_puts("  tty\n");
        init_puts("  tty echo <on|off>\n");
        init_puts("  tty canon <on|off>\n");
        init_puts("  tty isig <on|off>\n");
        init_puts("  log <err|warn|info|debug>\n");
        init_puts("  poll\n");
        init_puts("  clear\n");
        init_puts("  fdtest\n");
        init_puts("  uhello [count]\n");
        init_puts("  uvfork [count]\n");
        init_puts("  upipe [count]\n");
        init_puts("  upwd\n");
        init_puts("  uls [path]\n");
        init_puts("  ush\n");
        init_puts("  halt\n");
        init_puts("  uexec [path]\n");
        return;
    }
    if (init_streq(line, "halt")) {
        __asm__ __volatile__("halt");
        return;
    }
    if (init_streq(line, "tty")) {
        init_show_tty_mode();
        return;
    }
    if (init_streq(line, "poll")) {
        init_show_poll_state();
        return;
    }
    if (init_streq(line, "clear")) {
        console_fb_clear();
        return;
    }
    if (init_streq(line, "fdtest")) {
        uint32_t do_run = 0u;
        spinlock_lock(&g_init_cmd_lock);
        if (g_init_fdtest_running == 0u) {
            g_init_fdtest_running = 1u;
            do_run = 1u;
        }
        spinlock_unlock(&g_init_cmd_lock);
        if (do_run) {
            fd_selftest_run();
            spinlock_lock(&g_init_cmd_lock);
            g_init_fdtest_running = 0u;
            spinlock_unlock(&g_init_cmd_lock);
        }
        return;
    }
    if (init_streq(line, "uhello")) {
        init_run_uhello(1u);
        return;
    }
    if (init_streq(line, "uvfork")) {
        init_run_uvfork(1u);
        return;
    }
    if (init_streq(line, "upipe")) {
        init_run_upipe(1u);
        return;
    }
    if (init_streq(line, "upwd")) {
        init_run_upwd();
        return;
    }
    if (init_streq(line, "uls")) {
        init_run_uls(0);
        return;
    }
    if (init_streq(line, "ush")) {
        init_run_ush();
        return;
    }
    if (init_starts_with(line, "uhello ")) {
        uint32_t count = 0u;
        if (!init_parse_u32(line + 7, &count)) {
            init_puts("usage: uhello [count]\n");
            return;
        }
        init_run_uhello(count);
        return;
    }
    if (init_starts_with(line, "uvfork ")) {
        uint32_t count = 0u;
        if (!init_parse_u32(line + 7, &count)) {
            init_puts("usage: uvfork [count]\n");
            return;
        }
        init_run_uvfork(count);
        return;
    }
    if (init_starts_with(line, "upipe ")) {
        uint32_t count = 0u;
        if (!init_parse_u32(line + 6, &count)) {
            init_puts("usage: upipe [count]\n");
            return;
        }
        init_run_upipe(count);
        return;
    }
    if (init_starts_with(line, "uexec ")) {
        init_run_uexec();
        return;
    }
    if (init_starts_with(line, "uls ")) {
        init_run_uls(line + 4);
        return;
    }
    if (init_starts_with(line, "log ")) {
        init_set_log_level(line + 4);
        return;
    }
    if (init_streq(line, "tty echo on")) {
        init_set_flag(TTY_LFLAG_ECHO, 1u);
        return;
    }
    if (init_streq(line, "tty echo off")) {
        init_set_flag(TTY_LFLAG_ECHO, 0u);
        return;
    }
    if (init_streq(line, "tty canon on")) {
        init_set_flag(TTY_LFLAG_ICANON, 1u);
        return;
    }
    if (init_streq(line, "tty canon off")) {
        init_set_flag(TTY_LFLAG_ICANON, 0u);
        return;
    }
    if (init_streq(line, "tty isig on")) {
        init_set_flag(TTY_LFLAG_ISIG, 1u);
        return;
    }
    if (init_streq(line, "tty isig off")) {
        init_set_flag(TTY_LFLAG_ISIG, 0u);
        return;
    }

    init_puts("unknown command: ");
    init_puts(line);
    init_puts("\n");
}

static void init_task_entry(sched_task_t *task, void *arg) {
    init_state_t *st = (init_state_t *)arg;
    uint8_t cmd[INIT_LINE_CAP];
    int self_tid;
    uint8_t c = 0u;
    int n;
    (void)task;
    if (!st) {
        return;
    }

    self_tid = sched_current_tid();
    if (self_tid < 0) {
        return;
    }
    spinlock_lock(&g_init_owner_lock);
    if (g_init_owner_tid == 0u) {
        g_init_owner_tid = (uint32_t)self_tid;
    } else if (g_init_owner_tid != (uint32_t)self_tid) {
        spinlock_unlock(&g_init_owner_lock);
        sched_exit();
        return;
    }
    if (g_init_busy != 0u) {
        spinlock_unlock(&g_init_owner_lock);
        return;
    }
    g_init_busy = 1u;
    spinlock_unlock(&g_init_owner_lock);

    if (!st->started) {
        st->started = 1u;
        init_puts("init task online (type 'help')\n");
        init_prompt();
    }

    n = console_read(&c, 1u, 1u);
    if (n <= 0) {
        goto out_release;
    }

    if (c == (uint8_t)'\n') {
        uint32_t i;
        uint32_t cmd_len = st->len;
        if (cmd_len >= (INIT_LINE_CAP - 1u)) {
            cmd_len = INIT_LINE_CAP - 1u;
        }
        for (i = 0u; i < cmd_len; i++) {
            cmd[i] = st->line[i];
        }
        cmd[cmd_len] = '\0';
        st->len = 0u;
        init_handle_cmd((char *)cmd);
        init_prompt();
        goto out_release;
    }

    if (c == (uint8_t)'\t' || c == (uint8_t)'\v' || c == (uint8_t)'\f') {
        c = (uint8_t)' ';
    }

    if (c >= (uint8_t)' ' && c <= (uint8_t)'~') {
        if (st->len + 1u < INIT_LINE_CAP) {
            st->line[st->len++] = c;
        }
    }

out_release:
    spinlock_lock(&g_init_owner_lock);
    g_init_busy = 0u;
    spinlock_unlock(&g_init_owner_lock);
}

void init_task_spawn(void) {
    g_init_state.len = 0u;
    g_init_state.started = 0u;
    spinlock_init(&g_init_owner_lock);
    spinlock_init(&g_init_cmd_lock);
    g_init_owner_tid = 0u;
    g_init_busy = 0u;
    g_init_fdtest_running = 0u;
    if (sched_spawn("init", init_task_entry, &g_init_state) < 0) {
        KLOGW("init", "spawn failed");
    } else {
        KLOGI("init", "spawned");
    }
}
