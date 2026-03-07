#include "../include/kernel/console.h"
#include "../include/kernel/irq.h"
#include "../include/kernel/panic.h"
#include "../include/kernel/platform.h"
#include "../include/kernel/sched.h"
#include "../include/kernel/syscall.h"
#include "../include/kernel/trap.h"
#include "../include/kernel/types.h"

volatile uint32_t g_irq_stub_no;
volatile uint32_t g_irq_stub_r0;
volatile uint32_t g_irq_stub_r1;
volatile uint32_t g_irq_stub_r2;
volatile uint32_t g_irq_stub_r3;
volatile uint32_t g_irq_stub_r4;
volatile uint32_t g_irq_stub_r5;
volatile uint32_t g_irq_stub_r6;
static volatile trap_frame_t g_last_trap_frame;
static volatile uint32_t g_irq_counts[KERNEL_IVT_SIZE];
static volatile uint32_t g_fault_div0;

static inline uint32_t io_in32(uint32_t addr) {
    uint32_t v;
    __asm__ volatile ("in %0, %1" : "=r"(v) : "r"(addr));
    return v;
}

static inline void io_out32(uint32_t addr, uint32_t value) {
    __asm__ volatile ("out %0, %1" :: "r"(value), "r"(addr));
}

static void serial_drain_rx(void) {
    while ((io_in32(IO_SERIAL_STATUS) & SERIAL_STATUS_RX_READY) != 0u) {
        uint32_t v = io_in32(IO_SERIAL_RX);
        console_rx_feed((uint8_t)(v & 0xFFu));
    }
}

void irq_common_entry(uint32_t irq_no) {
    g_last_trap_frame.irq_no = irq_no;
    g_last_trap_frame.dispatch_count++;
    g_last_trap_frame.tick_snapshot = sched_ticks();
    trap_dispatch(irq_no);
    irq_eoi(irq_no);
}

void irq_common_entry_from_stub(void) {
    irq_common_entry(g_irq_stub_no);
}

void irq_input_init(void) {
    io_out32(IO_SERIAL_STATUS, SERIAL_CTRL_RX_INT_ENABLE);
}

uint32_t irq_input_dropped(void) {
    return console_rx_dropped();
}

const trap_frame_t *irq_last_trap_frame(void) {
    return (const trap_frame_t *)&g_last_trap_frame;
}

void irq_default(uint32_t irq_no) {
    if (irq_no < KERNEL_IVT_SIZE) {
        g_irq_counts[irq_no]++;
    }
}

void irq_divide_by_zero(uint32_t irq_no) {
    (void)irq_no;
    g_fault_div0 = 1u;
    kpanic("divide by zero");
}

void irq_disk_complete(uint32_t irq_no) {
    if (irq_no < KERNEL_IVT_SIZE) {
        g_irq_counts[irq_no]++;
    }
}

void irq_serial(uint32_t irq_no) {
    if (irq_no < KERNEL_IVT_SIZE) {
        g_irq_counts[irq_no]++;
    }
    serial_drain_rx();
}

void irq_keyboard(uint32_t irq_no) {
    if (irq_no < KERNEL_IVT_SIZE) {
        g_irq_counts[irq_no]++;
    }
    serial_drain_rx();
}

void irq_timer(uint32_t irq_no) {
    if (irq_no < KERNEL_IVT_SIZE) {
        g_irq_counts[irq_no]++;
    }
    schedule_tick();
}

void irq_syscall(uint32_t irq_no) {
    syscall_regs_t regs;
    if (irq_no < KERNEL_IVT_SIZE) {
        g_irq_counts[irq_no]++;
    }
    /*
     * Keep IRQ syscall entry on a pointer-based call path.
     * This avoids relying on wide C argument passing in the current backend.
     */
    regs.nr = g_irq_stub_r0;
    regs.arg0 = g_irq_stub_r1;
    regs.arg1 = g_irq_stub_r2;
    regs.arg2 = g_irq_stub_r3;
    regs.arg3 = g_irq_stub_r4;
    regs.arg4 = g_irq_stub_r5;
    regs.arg5 = g_irq_stub_r6;
    (void)syscall_dispatch(&regs);
}

__asm__(
    ".text\n"
    ".globl irq_stub_entry\n"
    "irq_stub_entry:\n"
    "  mov r8, r0\n"
    "  mov r9, r1\n"
    "  mov r10, r2\n"
    "  mov r11, r3\n"
    "  mov r12, r4\n"
    "  mov r13, r5\n"
    "  mov r14, r6\n"
    "  mov r2, r31\n"
    "  movi r30, 0x003FF000\n"
    "  mov r31, r30\n"
    "  movi r0, g_irq_stub_no\n"
    "  store32 r2, r0, 0\n"
    "  movi r0, g_irq_stub_r0\n"
    "  store32 r8, r0, 0\n"
    "  movi r0, g_irq_stub_r1\n"
    "  store32 r9, r0, 0\n"
    "  movi r0, g_irq_stub_r2\n"
    "  store32 r10, r0, 0\n"
    "  movi r0, g_irq_stub_r3\n"
    "  store32 r11, r0, 0\n"
    "  movi r0, g_irq_stub_r4\n"
    "  store32 r12, r0, 0\n"
    "  movi r0, g_irq_stub_r5\n"
    "  store32 r13, r0, 0\n"
    "  movi r0, g_irq_stub_r6\n"
    "  store32 r14, r0, 0\n"
    "  call irq_common_entry_from_stub\n"
    "  iret\n"
);
