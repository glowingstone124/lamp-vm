#include "../include/kernel/console_fb.h"
#include "../include/kernel/platform.h"
#include "../include/kernel/printk.h"
#include "../include/kernel/spinlock.h"
#include "../include/kernel/types.h"
#include "../include/kernel/timedate.h"

#define KLOG_OWNER_NONE 0xFFFFFFFFu

static volatile uint32_t g_klog_level = KERNEL_LOG_LEVEL_DEFAULT;
static spinlock_t g_klog_lock;
static volatile uint32_t g_klog_owner = KLOG_OWNER_NONE;
static volatile uint32_t g_klog_depth = 0u;

static inline uint32_t klog_cpu_id(void) {
    uint32_t id = 0u;
    __asm__ volatile("cpuid %0" : "=r"(id));
    return id;
}

static inline void io_out32(uint32_t addr, uint32_t value) {
    __asm__ volatile("out %0, %1" :: "r"(value), "r"(addr));
}

static inline uint32_t vm_read32(uint32_t addr) {
    return *(volatile uint32_t *)(uintptr_t)addr;
}

static inline void kputc_raw(uint32_t c) {
    io_out32(IO_SERIAL_TX, c & 0xFFu);
    console_fb_putc(c);
}

static inline void kputs_raw(const char *s) {
    const char *p = s;
    while (p && *p) {
        kputc_raw((uint32_t)(uint8_t)*p);
        p++;
    }
}

static inline void kprint_hex32_raw(uint32_t v) {
    static const char hexdig[] = "0123456789ABCDEF";
    kputc_raw((uint32_t)'0');
    kputc_raw((uint32_t)'x');
    for (int shift = 28; shift >= 0; shift -= 4) {
        uint32_t nib = (v >> (uint32_t)shift) & 0xFu;
        kputc_raw((uint32_t)(uint8_t)hexdig[nib]);
    }
}

static inline uint64_t klog_read_monotonic_ns(void) {
    uint32_t hi_a;
    uint32_t lo;
    uint32_t hi_b;
    do {
        hi_a = vm_read32(TIMER_MMIO_BASE + 0x10u);
        lo = vm_read32(TIMER_MMIO_BASE + 0x0Cu);
        hi_b = vm_read32(TIMER_MMIO_BASE + 0x10u);
    } while (hi_a != hi_b);
    return ((uint64_t)hi_a << 32) | (uint64_t)lo;
}

static inline int64_t klog_read_real_ns(void) {
    uint32_t hi_a;
    uint32_t lo;
    uint32_t hi_b;
    do {
        hi_a = vm_read32(TIMER_MMIO_BASE + 0x08u);
        lo = vm_read32(TIMER_MMIO_BASE + 0x04u);
        hi_b = vm_read32(TIMER_MMIO_BASE + 0x08u);
    } while (hi_a != hi_b);
    return ((uint64_t)hi_a << 32) | (uint64_t)lo;
}
static inline void kprint_u32_dec_raw(uint32_t v) {
    char buf[10]; // max 4294967295
    int i = 0;

    if (v == 0) {
        kputc_raw('0');
        return;
    }

    while (v > 0) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0) {
        kputc_raw((uint32_t)buf[--i]);
    }
}
static inline void kprint_u64_dec_raw(uint64_t v) {
    char buf[20]; // max 18446744073709551615
    int i = 0;

    if (v == 0) {
        kputc_raw('0');
        return;
    }

    while (v > 0) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }

    while (i > 0) {
        kputc_raw((uint32_t)buf[--i]);
    }
}
static inline void kprint_2d(uint32_t v) {
    kputc_raw((uint32_t)('0' + (v / 10)));
    kputc_raw((uint32_t)('0' + (v % 10)));
}

static inline void kprint_4d(uint32_t v) {
    kputc_raw((uint32_t)('0' + (v / 1000) % 10));
    kputc_raw((uint32_t)('0' + (v / 100) % 10));
    kputc_raw((uint32_t)('0' + (v / 10) % 10));
    kputc_raw((uint32_t)('0' + (v % 10)));
}

static inline void kprint_datetime_raw(const DateTime *dt) {
    kputc_raw('[');

    kprint_4d(dt->year);
    kputc_raw('-');
    kprint_2d(dt->month);
    kputc_raw('-');
    kprint_2d(dt->day);
    kputc_raw(' ');

    kprint_2d(dt->hour);
    kputc_raw(':');
    kprint_2d(dt->minute);
    kputc_raw(':');
    kprint_2d(dt->second);

    kputc_raw(']');
}
static inline void klog_print_timestamp_raw(void) {
    DateTime dt;
    uint64_t ns = klog_read_real_ns();

    unix_to_datetime_utc((int64_t)(ns / 1000000000ull), &dt);

    kprint_datetime_raw(&dt);
}
static inline void klog_lock_enter(void) {
    uint32_t cpu_id = klog_cpu_id();
    if (g_klog_owner == cpu_id) {
        g_klog_depth++;
        return;
    }
    spinlock_lock(&g_klog_lock);
    g_klog_owner = cpu_id;
    g_klog_depth = 1u;
}

static inline void klog_lock_leave(void) {
    if (g_klog_owner != klog_cpu_id() || g_klog_depth == 0u) {
        return;
    }
    g_klog_depth--;
    if (g_klog_depth != 0u) {
        return;
    }
    g_klog_owner = KLOG_OWNER_NONE;
    spinlock_unlock(&g_klog_lock);
}

static const char *klog_level_name(uint32_t level) {
    switch (level) {
        case KLOG_LEVEL_ERROR:
            return "ERR";
        case KLOG_LEVEL_WARN:
            return "WRN";
        case KLOG_LEVEL_INFO:
            return "INF";
        case KLOG_LEVEL_DEBUG:
            return "DBG";
        default:
            return "LOG";
    }
}

void kputc(uint32_t c) {
    klog_lock_enter();
    kputc_raw(c);
    klog_lock_leave();
}

void kputs(const char *s) {
    klog_lock_enter();
    kputs_raw(s);
    klog_lock_leave();
}

void kprintf(const char *s) {
    kputs(s);
}

void kprint_u32(uint32_t v) {
    /* Backend currently has unstable lowering for unsigned div/cmp patterns. */
    kprint_hex32(v);
}

void kprint_hex32(uint32_t v) {
    klog_lock_enter();
    kprint_hex32_raw(v);
    klog_lock_leave();
}

uint32_t klog_should_emit(uint32_t level) {
    if (level == 0u) {
        return 0u;
    }
    return (level <= g_klog_level) ? 1u : 0u;
}

void klog_set_level(uint32_t level) {
    if (level < KLOG_LEVEL_ERROR) {
        level = KLOG_LEVEL_ERROR;
    } else if (level > KLOG_LEVEL_DEBUG) {
        level = KLOG_LEVEL_DEBUG;
    }
    g_klog_level = level;
}

uint32_t klog_get_level(void) {
    return g_klog_level;
}

void kio_lock(void) {
    klog_lock_enter();
}

void kio_unlock(void) {
    klog_lock_leave();
}

void klog_begin(uint32_t level, const char *tag) {
    if (!klog_should_emit(level)) {
        return;
    }
    klog_lock_enter();
    klog_print_timestamp_raw();
    kputc_raw((uint32_t)'[');
    kputs_raw(klog_level_name(level));
    kputc_raw((uint32_t)']');
    kputc_raw((uint32_t)'[');
    if (tag && tag[0] != '\0') {
        kputs_raw(tag);
    } else {
        kputs_raw("kernel");
    }
    kputc_raw((uint32_t)']');
    kputc_raw((uint32_t)' ');
}

void klog_putc(uint32_t c) {
    if (g_klog_owner != klog_cpu_id() || g_klog_depth == 0u) {
        return;
    }
    kputc_raw(c);
}

void klog_puts(const char *s) {
    if (g_klog_owner != klog_cpu_id() || g_klog_depth == 0u) {
        return;
    }
    kputs_raw(s);
}

void klog_hex32(uint32_t v) {
    if (g_klog_owner != klog_cpu_id() || g_klog_depth == 0u) {
        return;
    }
    kprint_hex32_raw(v);
}

void klog_end(void) {
    if (g_klog_owner != klog_cpu_id() || g_klog_depth == 0u) {
        return;
    }
    kputc_raw((uint32_t)'\n');
    klog_lock_leave();
}

void klog_prefix(uint32_t level, const char *tag) {
    klog_begin(level, tag);
}

void klog_line(uint32_t level, const char *tag, const char *msg) {
    if (!klog_should_emit(level)) {
        return;
    }
    klog_begin(level, tag);
    if (msg) {
        klog_puts(msg);
    }
    klog_end();
}
