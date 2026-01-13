#include "stack.h"
#include "vm.h"
#include "interrupt.h"
#include "memory.h"
#include "panic.h"
//
// Created by Max Wang on 2025/12/30.
//
void vm_handle_interrupts(VM *vm) {
    if (vm->in_interrupt)
        return;

    for (uint32_t i = 0; i < IVT_SIZE; i++) {
        if (!vm->interrupt_flags[i])
            continue;

        vm->interrupt_flags[i] = 0;

        uint64_t isr_ip = vm_read64(vm, IVT_BASE + i * 8);
        if (isr_ip == UINT64_MAX)
            continue;

        call_push(vm, vm->ip);
        vm->ip = isr_ip;
        vm->in_interrupt = 1;
        break;
    }
}

void init_ivt(VM *vm) {
    for (uint32_t i = 0; i < IVT_SIZE; i++) {
        vm_write64(vm, IVT_BASE + i * 8, UINT64_MAX);
        vm->interrupt_flags[i] = 0;
    }
    vm->in_interrupt = 0;
}

void register_isr(VM *vm, uint32_t int_no, uint64_t isr_ip) {
    if (int_no >= IVT_SIZE) {
        panic(panic_format("Invalid interrupt number %u\n", int_no), vm);
        return;
    }

    vm_write64(vm, IVT_BASE + int_no * 8, isr_ip);
}

void trigger_interrupt(VM *vm, uint32_t int_no) {
    if (int_no >= IVT_SIZE)
        return;
    vm->interrupt_flags[int_no] = 1;
}
