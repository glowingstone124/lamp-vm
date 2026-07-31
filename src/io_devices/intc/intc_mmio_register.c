#include "intc_mmio_register.h"
#include "../../runtime_log.h"

#include <stdio.h>

#include "../../interrupt.h"
#include "../../io.h"

static inline int intc_current_core_id(VM *vm) {
    VCPU *cpu = vm_current_cpu(vm);
    if (!cpu) {
        return 0;
    }
    if (cpu->core_id < 0 || cpu->core_id >= vm->smp_cores) {
        return 0;
    }
    return cpu->core_id;
}

static inline int intc_is_u32_aligned(uint32_t offset) {
    return (offset & 0x3u) == 0u;
}

static uint32_t intc_read32(VM *vm, uint32_t addr) {
    const uint32_t offset = addr - INTC_BASE;
    const int core_id = intc_current_core_id(vm);

    if (offset >= INTC_REG_PENDING &&
        offset < INTC_REG_PENDING + IRQ_BITMAP_WORDS32 * 4u &&
        intc_is_u32_aligned(offset)) {
        const uint32_t reg = (offset - INTC_REG_PENDING) / 4u;
        return vm_interrupt_read_pending32(vm, core_id, reg);
    }

    if (offset >= INTC_REG_ENABLE &&
        offset < INTC_REG_ENABLE + IRQ_BITMAP_WORDS32 * 4u &&
        intc_is_u32_aligned(offset)) {
        const uint32_t reg = (offset - INTC_REG_ENABLE) / 4u;
        return vm_interrupt_read_enable32(vm, core_id, reg);
    }

    if (offset >= INTC_REG_PRIORITY &&
        offset < INTC_REG_PRIORITY + IVT_SIZE * 4u &&
        intc_is_u32_aligned(offset)) {
        const uint32_t int_no = (offset - INTC_REG_PRIORITY) / 4u;
        return vm_interrupt_read_priority(vm, int_no);
    }

    if (offset == INTC_REG_EOI) {
        return 0u;
    }

    fprintf(stderr, "Unknown INTC MMIO register offset: 0x%08x\n", offset);
    return 0u;
}

static void intc_write32(VM *vm, uint32_t addr, uint32_t value) {
    const uint32_t offset = addr - INTC_BASE;
    const int core_id = intc_current_core_id(vm);

    if (offset >= INTC_REG_PENDING &&
        offset < INTC_REG_PENDING + IRQ_BITMAP_WORDS32 * 4u &&
        intc_is_u32_aligned(offset)) {
        const uint32_t reg = (offset - INTC_REG_PENDING) / 4u;
        vm_interrupt_raise_pending32(vm, core_id, reg, value);
        return;
    }

    if (offset >= INTC_REG_ENABLE &&
        offset < INTC_REG_ENABLE + IRQ_BITMAP_WORDS32 * 4u &&
        intc_is_u32_aligned(offset)) {
        const uint32_t reg = (offset - INTC_REG_ENABLE) / 4u;
        vm_interrupt_write_enable32(vm, core_id, reg, value);
        return;
    }

    if (offset >= INTC_REG_PRIORITY &&
        offset < INTC_REG_PRIORITY + IVT_SIZE * 4u &&
        intc_is_u32_aligned(offset)) {
        const uint32_t int_no = (offset - INTC_REG_PRIORITY) / 4u;
        vm_interrupt_write_priority(vm, int_no, value);
        return;
    }

    if (offset == INTC_REG_EOI) {
        vm_interrupt_eoi(vm, core_id, value);
        if (value == INT_KEYBOARD || value == INT_MOUSE) {
            /* 8042 IRQs are level-like. Re-evaluate the output-buffer front
             * after clearing the latched pending bit so an event arriving at
             * the end of the handler cannot be lost. */
            vm_ps2_reassert_irq(vm);
        }
        return;
    }

    fprintf(stderr, "Unknown INTC MMIO register offset: 0x%08x\n", offset);
    atomic_set_vm_panic(vm, 1);
}

void register_intc_mmio(VM *vm) {
    static MMIO_Device intc_dev;
    intc_dev.start = INTC_BASE;
    intc_dev.end = INTC_BASE + INTC_MMIO_SIZE - 1u;
    intc_dev.read32 = intc_read32;
    intc_dev.write32 = intc_write32;

    if (vm->mmio_count < MAX_MMIO_DEVICES) {
        vm->mmio_devices[vm->mmio_count++] = &intc_dev;
        VM_RUNTIME_LOG("Registered VM Interrupt Controller to MMIO ID %d\n", vm->mmio_count);
    }
}
