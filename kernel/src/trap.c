#include "../include/kernel/trap.h"
#include "../include/kernel/irq.h"
#include "../include/kernel/platform.h"
#include "../include/kernel/spinlock.h"
#include "../include/kernel/types.h"
#include "../include/kernel/vm_info.h"

static trap_handler_t g_trap_table[KERNEL_IVT_SIZE];
static volatile uint32_t g_trap_ready;
static volatile uint32_t g_irq_mask[KERNEL_IVT_SIZE / 32u];
static volatile uint32_t g_intc_mmio_ready;
static spinlock_t g_irq_ctrl_lock;

static inline void mmio_write32(uint32_t addr, uint32_t value) {
    *(volatile uint32_t *)(uintptr_t)addr = value;
}

static inline uint32_t irq_no_valid(uint32_t irq_no) {
    return irq_no < KERNEL_IVT_SIZE;
}

static inline uint32_t intc_support_detect(void) {
    boot_info_t info;
    if (!vm_info_load_boot(&info)) {
        return 0u;
    }
    return (info.features & BOOTINFO_FEATURE_INTC_MMIO) ? 1u : 0u;
}

static inline void intc_write_enable_word(uint32_t word_index, uint32_t value) {
    mmio_write32(INTC_MMIO_BASE + INTC_REG_ENABLE + word_index * 4u, value);
}

static inline void intc_write_priority(uint32_t irq_no, uint32_t priority) {
    mmio_write32(INTC_MMIO_BASE + INTC_REG_PRIORITY + irq_no * 4u, priority & 0xFFu);
}

static inline void intc_write_eoi(uint32_t irq_no) {
    mmio_write32(INTC_MMIO_BASE + INTC_REG_EOI, irq_no);
}

static inline void intc_sync_enable_word(uint32_t word_index) {
    if (!g_intc_mmio_ready) {
        return;
    }
    if (word_index >= (KERNEL_IVT_SIZE / 32u)) {
        return;
    }
    intc_write_enable_word(word_index, g_irq_mask[word_index]);
}

static inline void irq_enable_unlocked(uint32_t irq_no) {
    const uint32_t word = irq_no / 32u;
    const uint32_t bit = 1u << (irq_no % 32u);
    g_irq_mask[word] |= bit;
    intc_sync_enable_word(word);
}

static inline void irq_disable_unlocked(uint32_t irq_no) {
    const uint32_t word = irq_no / 32u;
    const uint32_t bit = 1u << (irq_no % 32u);
    g_irq_mask[word] &= ~bit;
    intc_sync_enable_word(word);
}

static inline uint32_t irq_is_enabled_unlocked(uint32_t irq_no) {
    const uint32_t word = irq_no / 32u;
    const uint32_t bit = 1u << (irq_no % 32u);
    return (g_irq_mask[word] & bit) ? 1u : 0u;
}

static inline void irq_set_priority_unlocked(uint32_t irq_no, uint32_t priority) {
    if (!g_intc_mmio_ready) {
        return;
    }
    intc_write_priority(irq_no, priority);
}

static inline void irq_eoi_unlocked(uint32_t irq_no) {
    if (!g_intc_mmio_ready) {
        return;
    }
    intc_write_eoi(irq_no);
}

static inline void ivt_write_entry(uint32_t irq_no, uintptr_t handler_addr) {
    const uint32_t slot = KERNEL_IVT_BASE + irq_no * KERNEL_IVT_ENTRY_SIZE;
    *(volatile uint32_t *)(uintptr_t)(slot + 0u) = (uint32_t)handler_addr;
    *(volatile uint32_t *)(uintptr_t)(slot + 4u) = 0u;
}

void trap_register(uint32_t irq_no, trap_handler_t handler) {
    if (!irq_no_valid(irq_no)) {
        return;
    }
    spinlock_lock(&g_irq_ctrl_lock);
    g_trap_table[irq_no] = handler ? handler : irq_default;
    spinlock_unlock(&g_irq_ctrl_lock);
}

void irq_enable(uint32_t irq_no) {
    if (!irq_no_valid(irq_no)) {
        return;
    }
    spinlock_lock(&g_irq_ctrl_lock);
    irq_enable_unlocked(irq_no);
    spinlock_unlock(&g_irq_ctrl_lock);
}

void irq_disable(uint32_t irq_no) {
    if (!irq_no_valid(irq_no)) {
        return;
    }
    spinlock_lock(&g_irq_ctrl_lock);
    irq_disable_unlocked(irq_no);
    spinlock_unlock(&g_irq_ctrl_lock);
}

void irq_set_priority(uint32_t irq_no, uint32_t priority) {
    if (!irq_no_valid(irq_no)) {
        return;
    }
    spinlock_lock(&g_irq_ctrl_lock);
    irq_set_priority_unlocked(irq_no, priority);
    spinlock_unlock(&g_irq_ctrl_lock);
}

void irq_eoi(uint32_t irq_no) {
    if (!irq_no_valid(irq_no)) {
        return;
    }
    spinlock_lock(&g_irq_ctrl_lock);
    irq_eoi_unlocked(irq_no);
    spinlock_unlock(&g_irq_ctrl_lock);
}

void trap_init(void) {
    spinlock_init(&g_irq_ctrl_lock);
    g_intc_mmio_ready = intc_support_detect();
    g_trap_ready = 0u;

    spinlock_lock(&g_irq_ctrl_lock);
    for (uint32_t i = 0; i < KERNEL_IVT_SIZE; i++) {
        g_trap_table[i] = irq_default;
        ivt_write_entry(i, (uintptr_t)irq_stub_entry);
    }
    for (uint32_t i = 0; i < (KERNEL_IVT_SIZE / 32u); i++) {
        g_irq_mask[i] = 0u;
    }
    if (g_intc_mmio_ready) {
        for (uint32_t i = 0; i < (KERNEL_IVT_SIZE / 32u); i++) {
            intc_write_enable_word(i, 0u);
        }
        for (uint32_t i = 0; i < KERNEL_IVT_SIZE; i++) {
            intc_write_priority(i, 0u);
        }
    }
    spinlock_unlock(&g_irq_ctrl_lock);

    trap_register(IRQ_DIVIDE_BY_ZERO, irq_divide_by_zero);
    trap_register(IRQ_DISK_COMPLETE, irq_disk_complete);
    trap_register(IRQ_SERIAL, irq_serial);
    trap_register(IRQ_KEYBOARD, irq_keyboard);
    trap_register(IRQ_MOUSE, irq_mouse);
    trap_register(IRQ_TIMER, irq_timer);
    trap_register(IRQ_SYSCALL, irq_syscall);

    irq_set_priority(IRQ_DIVIDE_BY_ZERO, 0xF0u);
    irq_set_priority(IRQ_SYSCALL, 0xE0u);
    irq_set_priority(IRQ_TIMER, 0xC0u);
    irq_set_priority(IRQ_SERIAL, 0xB0u);
    irq_set_priority(IRQ_KEYBOARD, 0xB0u);
    irq_set_priority(IRQ_MOUSE, 0xB0u);
    irq_set_priority(IRQ_DISK_COMPLETE, 0xA0u);

    irq_enable(IRQ_DIVIDE_BY_ZERO);
    irq_enable(IRQ_DISK_COMPLETE);
    irq_enable(IRQ_SERIAL);
    irq_enable(IRQ_KEYBOARD);
    irq_enable(IRQ_MOUSE);
    irq_enable(IRQ_TIMER);
    irq_enable(IRQ_SYSCALL);

    g_trap_ready = 1u;
}

void trap_dispatch(uint32_t irq_no) {
    trap_handler_t handler = irq_default;
    uint32_t enabled = 0u;
    if (!g_trap_ready || irq_no >= KERNEL_IVT_SIZE) {
        return;
    }

    spinlock_lock(&g_irq_ctrl_lock);
    enabled = irq_is_enabled_unlocked(irq_no);
    handler = g_trap_table[irq_no] ? g_trap_table[irq_no] : irq_default;
    spinlock_unlock(&g_irq_ctrl_lock);

    if (!enabled) {
        return;
    }

    handler(irq_no);
}
