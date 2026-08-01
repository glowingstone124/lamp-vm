//
// Created by Max Wang on 2025/12/28.
//

#ifndef VM_STACK_H
#define VM_STACK_H
#include "memory.h"
#include "panic.h"

static inline void data_push_cpu(VM *vm, VCPU *cpu, uint32_t val) {
    if (!cpu)
        return;
    if (cpu->dsp == 0) {
        panic("Data stack overflow", vm);
        return;
    }
    cpu->dsp--;
    vm_write32_cpu(vm, cpu, cpu->data_stack_base + (vm_addr_t)(cpu->dsp * 4), val);
}

static inline void data_push(VM *vm, uint32_t val) {
    data_push_cpu(vm, vm_current_cpu(vm), val);
}

static inline uint32_t data_pop_cpu(VM *vm, VCPU *cpu) {
    if (!cpu)
        return 0;
    if (cpu->dsp >= DATA_STACK_SIZE) {
        panic("Data stack underflow", vm);
        return 0;
    }
    uint32_t val = vm_read32_cpu(vm, cpu, cpu->data_stack_base + (vm_addr_t)(cpu->dsp * 4));
    cpu->dsp++;
    return val;
}

static inline uint32_t data_pop(VM *vm) {
    return data_pop_cpu(vm, vm_current_cpu(vm));
}

static inline void call_push_cpu(VM *vm, VCPU *cpu, uint64_t val) {
    if (!cpu)
        return;
    if (cpu->csp == 0) {
        panic("Call stack overflow", vm);
        return;
    }
    cpu->csp--;
    vm_write64_cpu(vm, cpu, cpu->call_stack_base + (vm_addr_t)(cpu->csp * 8), val);
}

static inline void call_push(VM *vm, uint64_t val) {
    call_push_cpu(vm, vm_current_cpu(vm), val);
}

static inline uint64_t call_pop_cpu(VM *vm, VCPU *cpu) {
    if (!cpu)
        return 0;
    if (cpu->csp >= CALL_STACK_SIZE) {
        panic("Call stack underflow", vm);
        return 0;
    }
    uint64_t val = vm_read64_cpu(vm, cpu, cpu->call_stack_base + (vm_addr_t)(cpu->csp * 8));
    cpu->csp++;
    return val;
}

static inline uint64_t call_pop(VM *vm) {
    return call_pop_cpu(vm, vm_current_cpu(vm));
}

static inline void isr_push_cpu(VM *vm, VCPU *cpu, uint64_t val) {
    if (!cpu)
        return;
    if (cpu->isp == 0) {
        panic("Interrupt stack overflow", vm);
        return;
    }
    cpu->isp--;
    vm_write64_cpu(vm, cpu, cpu->isr_stack_base + (vm_addr_t)(cpu->isp * 8), val);
}

static inline void isr_push(VM *vm, uint64_t val) {
    isr_push_cpu(vm, vm_current_cpu(vm), val);
}

static inline uint64_t isr_pop_cpu(VM *vm, VCPU *cpu) {
    if (!cpu)
        return 0;
    if (cpu->isp >= ISR_STACK_SIZE) {
        panic("Interrupt stack underflow", vm);
        return 0;
    }
    uint64_t val = vm_read64_cpu(vm, cpu, cpu->isr_stack_base + (vm_addr_t)(cpu->isp * 8));
    cpu->isp++;
    return val;
}

static inline uint64_t isr_pop(VM *vm) {
    return isr_pop_cpu(vm, vm_current_cpu(vm));
}

#endif // VM_STACK_H
