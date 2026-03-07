#include "../include/kernel/console_fb.h"
#include "../include/kernel/irq.h"
#include "../include/kernel/panic.h"
#include "../include/kernel/printk.h"
#include "../include/kernel/types.h"

static void panic_halt(void) {
    for (;;) {
        __asm__ __volatile__("" ::: "memory");
    }
}

static void kpanic_emit_prefix(void) {
    console_fb_set_colors(0x00FFFFFFu, 0x00800000u);
    console_fb_clear();
    KLOGE("panic", "kernel panic");
}

void kpanic(const char *msg) {
    const trap_frame_t *tf = irq_last_trap_frame();
    kpanic_emit_prefix();
    klog_begin(KLOG_LEVEL_ERROR, "panic");
    klog_puts("reason=");
    if (msg) {
        klog_puts(msg);
    } else {
        klog_puts("<null>");
    }
    klog_end();
    if (tf) {
        klog_begin(KLOG_LEVEL_ERROR, "panic");
        klog_puts("last_irq=");
        klog_hex32(tf->irq_no);
        klog_puts(" dispatch_count=");
        klog_hex32(tf->dispatch_count);
        klog_puts(" ticks=");
        klog_hex32(tf->tick_snapshot);
        klog_end();
    }
    klog_begin(KLOG_LEVEL_ERROR, "panic");
    klog_puts("rx_dropped=");
    klog_hex32(irq_input_dropped());
    klog_end();
    panic_halt();
}
