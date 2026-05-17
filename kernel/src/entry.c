#include "../include/kernel/console.h"
#include "../include/kernel/console_fb.h"
#include "../include/kernel/fs.h"
#include "../include/kernel/irq.h"
#include "../include/kernel/init_task.h"
#include "../include/kernel/kernel.h"
#include "../include/kernel/mmu.h"
#include "../include/kernel/platform.h"
#include "../include/kernel/printk.h"
#include "../include/kernel/sched.h"
#include "../include/kernel/smp.h"
#include "../include/kernel/syscall.h"
#include "../include/kernel/trap.h"
#include "../include/kernel/types.h"
#include "../include/kernel/vm_info.h"
#include "../include/kernel/panic.h"
static volatile uint32_t g_kernel_booted;

static void kernel_early_init(void) {
    g_kernel_booted = 0u;
}

static void kernel_late_init(void) {
    g_kernel_booted = 1u;
}

void kernel_entry(void) {
    kernel_early_init();
    console_fb_init();
    console_init();
    KLOGI("kernel", "LAMP KERNEL V0.31 boot");
    vm_info_log_boot();
    mmu_init();
    /* Kernel owns IVT policy after BIOS handoff. */
    trap_init();
    irq_input_init();
    syscall_init();
    KLOGI("trap", "ready, input irq enabled");
    KLOGI("syscall", "irq 0x80 dispatch ready");

    /* Keep single-core path first, then expand to SMP. */
    smp_init_bsp();
    KLOGI("smp", "bsp online");

    sched_init();
    fs_init();
    init_task_spawn();
    smp_start_aps();
    KLOGI("smp", "aps online");
    kernel_late_init();
    KLOGI("sched", "start");
    sched_run();
    for (;;) {
        __asm__ __volatile__("" ::: "memory");
    }
}
