#include "../include/kernel/console.h"
#include "../include/kernel/console_fb.h"
#include "../include/kernel/fd_selftest.h"
#include "../include/kernel/init_task.h"
#include "../include/kernel/printk.h"
#include "../include/kernel/sched.h"
#include "../include/kernel/spinlock.h"
#include "../include/kernel/types.h"

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
