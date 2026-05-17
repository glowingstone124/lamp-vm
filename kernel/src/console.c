#include "../include/kernel/console.h"
#include "../include/kernel/console_fb.h"
#include "../include/kernel/irq.h"
#include "../include/kernel/platform.h"
#include "../include/kernel/printk.h"
#include "../include/kernel/sched.h"
#include "../include/kernel/spinlock.h"
#include "../include/kernel/types.h"

enum {
    CONSOLE_RX_CAP = 256u,
    CONSOLE_RX_MASK = CONSOLE_RX_CAP - 1u,
    TTY_LFLAG_SUPPORTED = TTY_LFLAG_ECHO | TTY_LFLAG_ICANON | TTY_LFLAG_ISIG,
    TTY_CC_VINTR = 0x03u,
    TTY_CC_VEOF = 0x04u,
    TTY_CC_VERASE_BS = 0x08u,
    TTY_CC_VERASE_DEL = 0x7Fu,
    TTY_CC_VKILL = 0x15u
};

static volatile uint32_t g_rx_head;
static volatile uint32_t g_rx_tail;
static volatile uint32_t g_rx_dropped;
static volatile uint32_t g_rx_lines;
static volatile uint32_t g_rx_eofs;
static volatile uint32_t g_tty_lflag;
static volatile uint32_t g_console_read_wait_count;
static uint8_t g_rx_buf[CONSOLE_RX_CAP];
static sched_waitq_t g_rx_waitq;
static spinlock_t g_rx_lock;

static inline void console_io_out32(uint32_t addr, uint32_t value) {
    __asm__ volatile("out %0, %1" :: "r"(value), "r"(addr));
}

static void console_echo_data_char(uint8_t c) {
    if ((g_tty_lflag & TTY_LFLAG_ECHO) == 0u) {
        return;
    }
    if (c == (uint8_t)'\a' ||
        c == (uint8_t)'\n' || c == (uint8_t)'\r' ||
        c == (uint8_t)'\t' || c == (uint8_t)'\v' || c == (uint8_t)'\f' ||
        (c >= (uint8_t)' ' && c <= (uint8_t)'~')) {
        kio_lock();
        console_fb_putc((uint32_t)c);
        kio_unlock();
    }
}

static void console_echo_backspace(void) {
    if ((g_tty_lflag & TTY_LFLAG_ECHO) == 0u) {
        return;
    }
    kio_lock();
    console_fb_putc((uint32_t)'\b');
    kio_unlock();
}

static void console_echo_intr(void) {
    if ((g_tty_lflag & TTY_LFLAG_ECHO) == 0u) {
        return;
    }
    kio_lock();
    console_fb_putc((uint32_t)'^');
    console_fb_putc((uint32_t)'C');
    console_fb_putc((uint32_t)'\n');
    kio_unlock();
}

static inline uint32_t console_can_read_locked(void) {
    if ((g_tty_lflag & TTY_LFLAG_ICANON) != 0u) {
        return (g_rx_lines != 0u || g_rx_eofs != 0u) ? 1u : 0u;
    }
    return (g_rx_head != g_rx_tail) ? 1u : 0u;
}

static uint32_t console_rx_pop_last_editable(void) {
    uint32_t head = g_rx_head;
    if (head == g_rx_tail) {
        return 0u;
    }
    {
        uint32_t prev = (head - 1u) & CONSOLE_RX_MASK;
        if (g_rx_buf[prev] == (uint8_t)'\n' || g_rx_buf[prev] == (uint8_t)TTY_CC_VEOF) {
            return 0u;
        }
        g_rx_head = prev;
    }
    return 1u;
}

static uint32_t console_rx_enqueue_char_locked(uint8_t c) {
    uint32_t head = g_rx_head;
    uint32_t next = (head + 1u) & CONSOLE_RX_MASK;
    if (next == g_rx_tail) {
        g_rx_dropped++;
        return 0u;
    }

    g_rx_buf[head] = c;
    g_rx_head = next;
    if (c == (uint8_t)'\n') {
        g_rx_lines++;
    } else if (c == (uint8_t)TTY_CC_VEOF) {
        g_rx_eofs++;
    }
    return 1u;
}

void console_init(void) {
    g_rx_head = 0u;
    g_rx_tail = 0u;
    g_rx_dropped = 0u;
    g_rx_lines = 0u;
    g_rx_eofs = 0u;
    g_console_read_wait_count = 0u;
    g_tty_lflag = TTY_LFLAG_ECHO | TTY_LFLAG_ICANON | TTY_LFLAG_ISIG;
    spinlock_init(&g_rx_lock);
    sched_waitq_init(&g_rx_waitq);
}

void console_rx_feed(uint8_t c) {
    uint32_t canonical;
    uint32_t do_wake = 0u;
    uint32_t do_echo_data = 0u;
    uint32_t do_echo_backspace = 0u;
    uint32_t do_echo_intr_msg = 0u;

    spinlock_lock(&g_rx_lock);
    canonical = ((g_tty_lflag & TTY_LFLAG_ICANON) != 0u) ? 1u : 0u;

    if (c == (uint8_t)'\r') {
        c = (uint8_t)'\n';
    }
    if (c == (uint8_t)TTY_CC_VINTR && (g_tty_lflag & TTY_LFLAG_ISIG) != 0u) {
        while (console_rx_pop_last_editable()) {
            do_echo_backspace++;
        }
        if (console_rx_enqueue_char_locked((uint8_t)'\n')) {
            do_wake = 1u;
        }
        do_echo_intr_msg = 1u;
        spinlock_unlock(&g_rx_lock);
        if (do_wake) {
            sched_waitq_wake_one(&g_rx_waitq);
        }
        while (do_echo_backspace != 0u) {
            do_echo_backspace--;
            console_echo_backspace();
        }
        if (do_echo_intr_msg) {
            console_echo_intr();
        }
        return;
    }
    if (canonical && c == (uint8_t)TTY_CC_VKILL) {
        while (console_rx_pop_last_editable()) {
            do_echo_backspace++;
        }
        spinlock_unlock(&g_rx_lock);
        while (do_echo_backspace != 0u) {
            do_echo_backspace--;
            console_echo_backspace();
        }
        return;
    }
    if (canonical &&
        (c == (uint8_t)TTY_CC_VERASE_BS || c == (uint8_t)TTY_CC_VERASE_DEL)) {
        if (console_rx_pop_last_editable()) {
            do_echo_backspace = 1u;
        }
        spinlock_unlock(&g_rx_lock);
        if (do_echo_backspace) {
            console_echo_backspace();
        }
        return;
    }
    if (canonical && c == (uint8_t)TTY_CC_VEOF) {
        if (console_rx_enqueue_char_locked(c)) {
            do_wake = 1u;
        }
        spinlock_unlock(&g_rx_lock);
        if (do_wake) {
            sched_waitq_wake_one(&g_rx_waitq);
        }
        return;
    }
    if (c == 0u) {
        spinlock_unlock(&g_rx_lock);
        return;
    }

    if (console_rx_enqueue_char_locked(c)) {
        do_wake = 1u;
        do_echo_data = 1u;
    }
    spinlock_unlock(&g_rx_lock);
    if (do_wake) {
        sched_waitq_wake_one(&g_rx_waitq);
    }
    if (do_echo_data) {
        console_echo_data_char(c);
    }
}

uint32_t console_rx_dropped(void) {
    uint32_t v;
    spinlock_lock(&g_rx_lock);
    v = g_rx_dropped;
    spinlock_unlock(&g_rx_lock);
    return v;
}

uint32_t console_rx_lines(void) {
    uint32_t v;
    spinlock_lock(&g_rx_lock);
    v = g_rx_lines;
    spinlock_unlock(&g_rx_lock);
    return v;
}

uint32_t console_can_read(void) {
    uint32_t ready;
    irq_serial_drain_rx();
    spinlock_lock(&g_rx_lock);
    ready = console_can_read_locked();
    spinlock_unlock(&g_rx_lock);
    return ready;
}

int console_wait_readable(uint32_t timeout_ticks, uint32_t nonblock) {
    uint32_t ready;
    irq_serial_drain_rx();
    spinlock_lock(&g_rx_lock);
    ready = console_can_read_locked();
    if (ready) {
        spinlock_unlock(&g_rx_lock);
        return CONSOLE_IO_OK;
    }
    if (nonblock) {
        spinlock_unlock(&g_rx_lock);
        return 0;
    }
    sched_waitq_sleep(&g_rx_waitq, timeout_ticks ? timeout_ticks : 1u);
    spinlock_unlock(&g_rx_lock);
    return CONSOLE_IO_BLOCKED;
}

uint32_t console_tty_get_lflag(void) {
    uint32_t lflag;
    spinlock_lock(&g_rx_lock);
    lflag = g_tty_lflag;
    spinlock_unlock(&g_rx_lock);
    return lflag;
}

uint32_t console_tty_set_lflag(uint32_t lflag) {
    uint32_t out;
    spinlock_lock(&g_rx_lock);
    g_tty_lflag = lflag & TTY_LFLAG_SUPPORTED;
    out = g_tty_lflag;
    spinlock_unlock(&g_rx_lock);
    return out;
}

int console_read(uint8_t *dst, uint32_t len, uint32_t nonblock) {
    uint32_t n = 0u;
    uint32_t canonical;
    uint32_t saw_eof = 0u;
    if (!dst || len == 0u) {
        return 0;
    }

    irq_serial_drain_rx();
    spinlock_lock(&g_rx_lock);
    canonical = (g_tty_lflag & TTY_LFLAG_ICANON) ? 1u : 0u;
    if (canonical && g_rx_lines == 0u && g_rx_eofs == 0u) {
        if (nonblock) {
            spinlock_unlock(&g_rx_lock);
            return 0;
        }
        g_console_read_wait_count++;
        if ((g_console_read_wait_count & 0x3Fu) == 1u) {
            klog_begin(KLOG_LEVEL_DEBUG, "console");
            klog_puts("read wait head=");
            klog_hex32(g_rx_head);
            klog_puts(" tail=");
            klog_hex32(g_rx_tail);
            klog_puts(" lines=");
            klog_hex32(g_rx_lines);
            klog_end();
        }
        sched_waitq_sleep(&g_rx_waitq, 1u);
        spinlock_unlock(&g_rx_lock);
        return CONSOLE_IO_BLOCKED;
    }

    while (n < len) {
        uint32_t tail = g_rx_tail;
        if (tail == g_rx_head) {
            break;
        }
        {
            uint8_t c = g_rx_buf[tail];
            if (canonical && c == (uint8_t)TTY_CC_VEOF) {
                if (g_rx_eofs != 0u) {
                    g_rx_eofs--;
                }
                g_rx_tail = (tail + 1u) & CONSOLE_RX_MASK;
                saw_eof = 1u;
                break;
            }
            dst[n++] = c;
            if (c == (uint8_t)'\n' && g_rx_lines != 0u) {
                g_rx_lines--;
                if (canonical) {
                    g_rx_tail = (tail + 1u) & CONSOLE_RX_MASK;
                    break;
                }
            }
        }
        g_rx_tail = (tail + 1u) & CONSOLE_RX_MASK;
    }

    spinlock_unlock(&g_rx_lock);

    if (n != 0u) {
        return (int)n;
    }
    if (canonical && saw_eof) {
        return 0;
    }
    if (nonblock) {
        return 0;
    }

    sched_waitq_sleep(&g_rx_waitq, 1u);
    return CONSOLE_IO_BLOCKED;
}

uint32_t console_write(const uint8_t *src, uint32_t len) {
    uint32_t n = 0u;
    if (!src || len == 0u) {
        return 0u;
    }
    kio_lock();
    for (n = 0u; n < len; n++) {
        console_io_out32(IO_SERIAL_TX, (uint32_t)src[n]);
        console_fb_putc((uint32_t)src[n]);
    }
    kio_unlock();
    return n;
}
