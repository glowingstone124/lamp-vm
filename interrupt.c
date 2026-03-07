#include "stack.h"
#include "vm.h"
#include "interrupt.h"
#include "memory.h"
#include "panic.h"

// Created by Max Wang on 2025/12/30.

#define ISR_ARG_REG 31

static inline size_t irq_word_index(int core_id, uint32_t int_no) {
    return (size_t)core_id * (size_t)IRQ_BITMAP_WORDS + (size_t)(int_no >> 6);
}

static inline size_t irq_word_index32(int core_id, uint32_t reg_index32) {
    return (size_t)core_id * (size_t)IRQ_BITMAP_WORDS + (size_t)(reg_index32 >> 1);
}

static inline uint64_t irq_bit_mask(uint32_t int_no) {
    return 1ULL << (int_no & 63);
}

static inline uint32_t irq_chunk_shift32(uint32_t reg_index32) {
    return (reg_index32 & 1u) ? 32u : 0u;
}

static inline int irq_valid_core(const VM *vm, int core_id) {
    return vm && core_id >= 0 && core_id < vm->smp_cores;
}

static inline int irq_valid_reg32(uint32_t reg_index) {
    return reg_index < IRQ_BITMAP_WORDS32;
}

static inline int irq_valid_int_no(uint32_t int_no) {
    return int_no < IVT_SIZE;
}

static uint32_t irq_bitmap_read32(const atomic_uint_fast64_t *bitmap,
                                  const VM *vm,
                                  int core_id,
                                  uint32_t reg_index) {
    if (!bitmap || !irq_valid_core(vm, core_id) || !irq_valid_reg32(reg_index)) {
        return 0u;
    }
    const size_t word_idx = irq_word_index32(core_id, reg_index);
    const uint32_t shift = irq_chunk_shift32(reg_index);
    const uint_fast64_t word = atomic_load(&bitmap[word_idx]);
    return (uint32_t)((word >> shift) & 0xFFFFFFFFu);
}

static inline void isr_push_u32(VM *vm, uint32_t v) {
    isr_push(vm, (uint64_t)v);
}

static inline uint32_t isr_pop_u32(VM *vm) {
    return (uint32_t)isr_pop(vm);
}

uint32_t vm_interrupt_read_pending32(VM *vm, int core_id, uint32_t reg_index) {
    return irq_bitmap_read32(vm ? vm->interrupt_bitmap : NULL, vm, core_id, reg_index);
}

void vm_interrupt_raise_pending32(VM *vm, int core_id, uint32_t reg_index, uint32_t value) {
    if (!vm || !vm->interrupt_bitmap) {
        return;
    }
    if (!irq_valid_core(vm, core_id) || !irq_valid_reg32(reg_index)) {
        return;
    }
    const size_t idx = irq_word_index32(core_id, reg_index);
    const uint32_t shift = irq_chunk_shift32(reg_index);
    const uint_fast64_t mask = ((uint_fast64_t)value) << shift;
    atomic_fetch_or(&vm->interrupt_bitmap[idx], mask);
}

uint32_t vm_interrupt_read_enable32(VM *vm, int core_id, uint32_t reg_index) {
    return irq_bitmap_read32(vm ? vm->interrupt_enable_bitmap : NULL, vm, core_id, reg_index);
}

void vm_interrupt_write_enable32(VM *vm, int core_id, uint32_t reg_index, uint32_t value) {
    if (!vm || !vm->interrupt_enable_bitmap) {
        return;
    }
    if (!irq_valid_core(vm, core_id) || !irq_valid_reg32(reg_index)) {
        return;
    }

    const size_t idx = irq_word_index32(core_id, reg_index);
    const uint32_t shift = irq_chunk_shift32(reg_index);
    const uint_fast64_t clear_mask = ((uint_fast64_t)0xFFFFFFFFu) << shift;
    const uint_fast64_t set_mask = ((uint_fast64_t)value) << shift;

    while (1) {
        uint_fast64_t expected = atomic_load(&vm->interrupt_enable_bitmap[idx]);
        const uint_fast64_t desired = (expected & ~clear_mask) | set_mask;
        if (atomic_compare_exchange_weak(&vm->interrupt_enable_bitmap[idx], &expected, desired)) {
            return;
        }
    }
}

uint32_t vm_interrupt_read_priority(VM *vm, uint32_t int_no) {
    if (!vm || !irq_valid_int_no(int_no)) {
        return 0u;
    }
    return (uint32_t)atomic_load(&vm->interrupt_priority[int_no]);
}

void vm_interrupt_write_priority(VM *vm, uint32_t int_no, uint32_t priority) {
    if (!vm || !irq_valid_int_no(int_no)) {
        return;
    }
    atomic_store(&vm->interrupt_priority[int_no], (unsigned char)(priority & 0xFFu));
}

void vm_interrupt_eoi(VM *vm, int core_id, uint32_t int_no) {
    if (!vm || !vm->interrupt_bitmap) {
        return;
    }
    if (!irq_valid_core(vm, core_id) || !irq_valid_int_no(int_no)) {
        return;
    }
    const size_t idx = irq_word_index(core_id, int_no);
    const uint64_t mask = irq_bit_mask(int_no);
    atomic_fetch_and(&vm->interrupt_bitmap[idx], (uint_fast64_t)(~mask));
}

void vm_enter_interrupt(VM *vm, uint32_t int_no) {
    VCPU *cpu = vm_current_cpu(vm);
    if (!cpu)
        return;
    if (int_no >= IVT_SIZE)
        return;

    if (cpu->in_interrupt)
        return;

    const uint64_t isr_ip = vm_read64(vm, IVT_BASE + int_no * 8);
    if (isr_ip == UINT64_MAX)
        return;

    isr_push(vm, (uint64_t)cpu->ip);
    isr_push(vm, (uint64_t)cpu->flags);

    for (uint32_t i = 0; i < REG_COUNT; i++) {
        isr_push_u32(vm, cpu->regs[i]);
    }

    /*
     * Preserve full pre-interrupt register context (including r31) on ISR stack.
     * Then expose interrupt number in r31 for ISR runtime.
     */
    cpu->regs[ISR_ARG_REG] = int_no;

    cpu->ip = (size_t)(vm_addr_t)isr_ip;
    cpu->in_interrupt = 1;
}

void vm_iret(VM *vm) {
    VCPU *cpu = vm_current_cpu(vm);
    if (!cpu)
        return;
    if (!cpu->in_interrupt)
        return;

    for (int i = (int)REG_COUNT - 1; i >= 0; i--) {
        cpu->regs[i] = isr_pop_u32(vm);
    }

    cpu->flags = (unsigned int)isr_pop(vm);

    cpu->ip = (size_t)(vm_addr_t)isr_pop(vm);

    cpu->in_interrupt = 0;
}

void vm_handle_interrupts(VM *vm) {
    VCPU *cpu = vm_current_cpu(vm);
    if (!cpu)
        return;
    if (cpu->in_interrupt)
        return;
    if (cpu->irq_masked)
        return;
    if (!vm->interrupt_bitmap)
        return;

    const int core_id = cpu->core_id;
    if (!irq_valid_core(vm, core_id))
        return;
    const size_t base = (size_t)core_id * (size_t)IRQ_BITMAP_WORDS;

    uint32_t best_int_no = IVT_SIZE;
    uint8_t best_priority = 0u;
    int found = 0;

    for (uint32_t w = 0; w < IRQ_BITMAP_WORDS; w++) {
        const uint_fast64_t pending_word = atomic_load(&vm->interrupt_bitmap[base + w]);
        const uint_fast64_t enable_word = vm->interrupt_enable_bitmap
            ? atomic_load(&vm->interrupt_enable_bitmap[base + w])
            : (uint_fast64_t)UINT64_MAX;
        uint_fast64_t deliverable = pending_word & enable_word;
        while (deliverable != 0u) {
            const uint32_t bit = (uint32_t)__builtin_ctzll((unsigned long long)deliverable);
            const uint32_t int_no = w * 64u + bit;
            const uint8_t prio = (uint8_t)atomic_load(&vm->interrupt_priority[int_no]);
            if (!found || prio > best_priority || (prio == best_priority && int_no < best_int_no)) {
                found = 1;
                best_int_no = int_no;
                best_priority = prio;
            }
            deliverable &= (deliverable - 1u);
        }
    }

    if (!found) {
        return;
    }

    if (vm->interrupt_enable_bitmap) {
        const size_t idx = irq_word_index(core_id, best_int_no);
        const uint64_t mask = irq_bit_mask(best_int_no);
        const uint_fast64_t enabled = atomic_load(&vm->interrupt_enable_bitmap[idx]);
        if ((enabled & (uint_fast64_t)mask) == 0u) {
            return;
        }
    }

    vm_enter_interrupt(vm, best_int_no);
}

void init_ivt(VM *vm) {
    if (!vm)
        return;
    for (uint32_t i = 0; i < IVT_SIZE; i++) {
        vm_write64(vm, IVT_BASE + i * 8, UINT64_MAX);
        atomic_store(&vm->interrupt_priority[i], (unsigned char)0u);
    }
    for (int c = 0; c < vm->smp_cores; c++) {
        for (uint32_t w = 0; w < IRQ_BITMAP_WORDS; w++) {
            atomic_store(&vm->interrupt_bitmap[(size_t)c * (size_t)IRQ_BITMAP_WORDS + w], 0);
            if (vm->interrupt_enable_bitmap) {
                atomic_store(&vm->interrupt_enable_bitmap[(size_t)c * (size_t)IRQ_BITMAP_WORDS + w],
                             (uint_fast64_t)UINT64_MAX);
            }
        }
    }
    for (int c = 0; c < vm->smp_cores; c++) {
        vm->cpus[c].in_interrupt = 0;
    }
}

void register_isr(VM *vm, uint32_t int_no, uint64_t isr_ip) {
    if (int_no >= IVT_SIZE) {
        panic(panic_format("Invalid interrupt number %u\n", int_no), vm);
        return;
    }
    vm_write64(vm, IVT_BASE + int_no * 8, (uint64_t)(vm_addr_t)isr_ip);
}

void trigger_interrupt(VM *vm, uint32_t int_no) {
    trigger_interrupt_target(vm, BSP_CORE, int_no);
}

void trigger_interrupt_target(VM *vm, int core_id, uint32_t int_no) {
    if (!vm)
        return;
    if (core_id < 0 || core_id >= vm->smp_cores)
        return;
    if (int_no >= IVT_SIZE)
        return;
    if (!vm->interrupt_bitmap)
        return;

    const size_t idx = irq_word_index(core_id, int_no);
    const uint64_t mask = irq_bit_mask(int_no);
    atomic_fetch_or(&vm->interrupt_bitmap[idx], (uint_fast64_t)mask);
}
