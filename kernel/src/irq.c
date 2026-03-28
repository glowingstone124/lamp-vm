#include "../include/kernel/blk.h"
#include "../include/kernel/console.h"
#include "../include/kernel/irq.h"
#include "../include/kernel/panic.h"
#include "../include/kernel/platform.h"
#include "../include/kernel/sched.h"
#include "../include/kernel/syscall.h"
#include "../include/kernel/trap.h"
#include "../include/kernel/types.h"

#define STR1(x) #x
#define STR(x) STR1(x)

volatile uint32_t g_irq_stub_no[32];
volatile uint32_t g_irq_stub_r0[32];
volatile uint32_t g_irq_stub_r1[32];
volatile uint32_t g_irq_stub_r2[32];
volatile uint32_t g_irq_stub_r3[32];
volatile uint32_t g_irq_stub_r4[32];
volatile uint32_t g_irq_stub_r5[32];
volatile uint32_t g_irq_stub_r6[32];
volatile uint32_t g_irq_saved_user_csp[32];
volatile uint32_t g_irq_saved_user_dsp[32];
static volatile trap_frame_t g_last_trap_frame;
static volatile uint32_t g_irq_counts[KERNEL_IVT_SIZE];
static volatile uint32_t g_fault_div0;
static volatile uint32_t g_ps2_kbd_shift_l;
static volatile uint32_t g_ps2_kbd_shift_r;
static volatile uint32_t g_ps2_kbd_ctrl_l;
static volatile uint32_t g_ps2_kbd_ctrl_r;
static volatile uint32_t g_ps2_kbd_caps_lock;
static volatile uint32_t g_ps2_kbd_ext;

static const uint8_t g_ps2_kbd_ascii_plain[128] = {
    [0x01] = 0x1Bu,
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
    [0x0A] = '9', [0x0B] = '0', [0x0C] = '-', [0x0D] = '=',
    [0x0E] = 0x08u, [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
    [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',
    [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
    [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm', [0x33] = ',',
    [0x34] = '.', [0x35] = '/', [0x39] = ' '
};

static const uint8_t g_ps2_kbd_ascii_shift[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
    [0x06] = '%', [0x07] = '^', [0x08] = '&', [0x09] = '*',
    [0x0A] = '(', [0x0B] = ')', [0x0C] = '_', [0x0D] = '+',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R',
    [0x14] = 'T', [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I',
    [0x18] = 'O', [0x19] = 'P', [0x1A] = '{', [0x1B] = '}',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F',
    [0x22] = 'G', [0x23] = 'H', [0x24] = 'J', [0x25] = 'K',
    [0x26] = 'L', [0x27] = ':', [0x28] = '"', [0x29] = '~',
    [0x2B] = '|',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V',
    [0x30] = 'B', [0x31] = 'N', [0x32] = 'M', [0x33] = '<',
    [0x34] = '>', [0x35] = '?', [0x39] = ' '
};

static inline uint32_t io_in32(uint32_t addr) {
    uint32_t v;
    __asm__ volatile ("in %0, %1" : "=r"(v) : "r"(addr));
    return v;
}

static inline void io_out32(uint32_t addr, uint32_t value) {
    __asm__ volatile ("out %0, %1" :: "r"(value), "r"(addr));
}

static inline uint32_t irq_stub_cpu_index(void) {
    uint32_t cpu = 0u;
    __asm__ volatile("cpuid %0" : "=r"(cpu));
    if (cpu >= 32u) {
        cpu = 0u;
    }
    return cpu;
}

static void serial_drain_rx(void) {
    while ((io_in32(IO_SERIAL_STATUS) & SERIAL_STATUS_RX_READY) != 0u) {
        uint32_t v = io_in32(IO_SERIAL_RX);
        console_rx_feed((uint8_t)(v & 0xFFu));
    }
}

static void ps2_keyboard_handle_scancode(uint8_t scancode) {
    uint32_t release;
    uint8_t scan;
    uint32_t shift;
    uint32_t ctrl;
    uint8_t out = 0u;

    if (scancode == 0xE0u) {
        g_ps2_kbd_ext = 1u;
        return;
    }

    release = (scancode & 0x80u) ? 1u : 0u;
    scan = (uint8_t)(scancode & 0x7Fu);

    if (g_ps2_kbd_ext != 0u) {
        g_ps2_kbd_ext = 0u;
        if (scan == 0x1Du) {
            g_ps2_kbd_ctrl_r = release ? 0u : 1u;
        }
        return;
    }

    if (scan == 0x2Au) {
        g_ps2_kbd_shift_l = release ? 0u : 1u;
        return;
    }
    if (scan == 0x36u) {
        g_ps2_kbd_shift_r = release ? 0u : 1u;
        return;
    }
    if (scan == 0x1Du) {
        g_ps2_kbd_ctrl_l = release ? 0u : 1u;
        return;
    }
    if (scan == 0x3Au) {
        if (!release) {
            g_ps2_kbd_caps_lock ^= 1u;
        }
        return;
    }
    if (release) {
        return;
    }

    shift = (g_ps2_kbd_shift_l | g_ps2_kbd_shift_r) ? 1u : 0u;
    ctrl = (g_ps2_kbd_ctrl_l | g_ps2_kbd_ctrl_r) ? 1u : 0u;

    if (ctrl != 0u) {
        const uint8_t base = g_ps2_kbd_ascii_plain[scan];
        if (base >= 'a' && base <= 'z') {
            out = (uint8_t)(base - 'a' + 1u);
        } else if (base >= 'A' && base <= 'Z') {
            out = (uint8_t)(base - 'A' + 1u);
        }
    } else {
        out = shift ? g_ps2_kbd_ascii_shift[scan] : g_ps2_kbd_ascii_plain[scan];
        if (g_ps2_kbd_caps_lock != 0u && out >= 'a' && out <= 'z') {
            out = (uint8_t)(out - 'a' + 'A');
        } else if (g_ps2_kbd_caps_lock != 0u && out >= 'A' && out <= 'Z' && shift != 0u) {
            out = (uint8_t)(out - 'A' + 'a');
        }
    }

    if (out != 0u) {
        console_rx_feed(out);
    }
}

static void ps2_keyboard_drain_rx(void) {
    while ((io_in32(IO_PS2_KBD_STATUS) & PS2_STATUS_RX_READY) != 0u) {
        const uint32_t v = io_in32(IO_PS2_KBD_DATA);
        ps2_keyboard_handle_scancode((uint8_t)(v & 0xFFu));
    }
}

static void ps2_mouse_drain_rx(void) {
    while ((io_in32(IO_PS2_MOUSE_STATUS) & PS2_STATUS_RX_READY) != 0u) {
        (void)io_in32(IO_PS2_MOUSE_DATA);
    }
}

void irq_common_entry(uint32_t irq_no) {
    g_last_trap_frame.irq_no = irq_no;
    g_last_trap_frame.dispatch_count++;
    g_last_trap_frame.tick_snapshot = sched_ticks();
    if (irq_no == IRQ_SYSCALL) {
        irq_syscall(irq_no);
        irq_eoi(irq_no);
        return;
    }
    trap_dispatch(irq_no);
    irq_eoi(irq_no);
}

void irq_common_entry_from_stub(void) {
    irq_common_entry(g_irq_stub_no[irq_stub_cpu_index()]);
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

uint32_t irq_saved_user_csp(void) {
    uint32_t cpu = 0u;
    __asm__ volatile("cpuid %0" : "=r"(cpu));
    if (cpu >= 32u) {
        cpu = 0u;
    }
    return g_irq_saved_user_csp[cpu];
}

uint32_t irq_saved_user_dsp(void) {
    uint32_t cpu = 0u;
    __asm__ volatile("cpuid %0" : "=r"(cpu));
    if (cpu >= 32u) {
        cpu = 0u;
    }
    return g_irq_saved_user_dsp[cpu];
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
    blk_irq_complete();
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
    ps2_keyboard_drain_rx();
}

void irq_mouse(uint32_t irq_no) {
    if (irq_no < KERNEL_IVT_SIZE) {
        g_irq_counts[irq_no]++;
    }
    ps2_mouse_drain_rx();
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
    const uint32_t cpu = irq_stub_cpu_index();
    regs.nr = g_irq_stub_r0[cpu];
    regs.arg0 = g_irq_stub_r1[cpu];
    regs.arg1 = g_irq_stub_r2[cpu];
    regs.arg2 = g_irq_stub_r3[cpu];
    regs.arg3 = g_irq_stub_r4[cpu];
    regs.arg4 = g_irq_stub_r5[cpu];
    regs.arg5 = g_irq_stub_r6[cpu];
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
    "  cpuid r15\n"
    "  mov r7, r15\n"
    "  add r7, r7, r7\n"
    "  add r7, r7, r7\n"
    "  movi r0, g_irq_saved_user_csp\n"
    "  add r0, r0, r7\n"
    "  movi r1, " STR(IO_CPU_CTX_CSP) "\n"
    "  in r3, r1\n"
    "  store32 r3, r0, 0\n"
    "  movi r0, g_irq_saved_user_dsp\n"
    "  add r0, r0, r7\n"
    "  movi r1, " STR(IO_CPU_CTX_DSP) "\n"
    "  in r3, r1\n"
    "  store32 r3, r0, 0\n"
    "  shli r15, r15, " STR(KERNEL_IRQ_STACK_SHIFT) "\n"
    "  movi r30, " STR(KERNEL_IRQ_STACK_TOP) "\n"
    "  sub r30, r30, r15\n"
    "  mov r31, r30\n"
    "  movi r0, g_irq_stub_no\n"
    "  add r0, r0, r7\n"
    "  store32 r2, r0, 0\n"
    "  movi r0, g_irq_stub_r0\n"
    "  add r0, r0, r7\n"
    "  store32 r8, r0, 0\n"
    "  movi r0, g_irq_stub_r1\n"
    "  add r0, r0, r7\n"
    "  store32 r9, r0, 0\n"
    "  movi r0, g_irq_stub_r2\n"
    "  add r0, r0, r7\n"
    "  store32 r10, r0, 0\n"
    "  movi r0, g_irq_stub_r3\n"
    "  add r0, r0, r7\n"
    "  store32 r11, r0, 0\n"
    "  movi r0, g_irq_stub_r4\n"
    "  add r0, r0, r7\n"
    "  store32 r12, r0, 0\n"
    "  movi r0, g_irq_stub_r5\n"
    "  add r0, r0, r7\n"
    "  store32 r13, r0, 0\n"
    "  movi r0, g_irq_stub_r6\n"
    "  add r0, r0, r7\n"
    "  store32 r14, r0, 0\n"
    "  call irq_common_entry_from_stub\n"
    "  iret\n"
);
