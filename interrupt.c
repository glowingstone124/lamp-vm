#include "stack.h"
#include "vm.h"
#include "interrupt.h"

#include "panic.h"
//
// Created by Max Wang on 2025/12/30.
//
void vm_handle_interrupts(VM *vm) {
    if (vm->in_interrupt) return;

    for (int i = 0; i < IVT_SIZE; i++) {
        if (vm->interrupt_flags[i]) {
            vm->interrupt_flags[i] = 0;
            int isr_ip = vm->memory[IVT_BASE + i];
            if (isr_ip >= 0) {
                CALL_PUSH(vm, vm->ip);
                vm->ip = isr_ip;
                vm->in_interrupt = 1;
                break;
            }
        }
    }
}

void init_ivt(VM *vm) {
    for (int i = 0; i < IVT_SIZE; i++) {
        vm->memory[IVT_BASE + i] = -1;
        vm->interrupt_flags[i] = 0;
    }
    vm->in_interrupt = 0;
}

void register_isr(VM *vm, int int_no, int isr_ip) {
    if (int_no < 0 || int_no >= IVT_SIZE) {
        panic(panic_format("Invalid interrupt number %d\n", int_no), vm);
        return;
    }
    vm->memory[IVT_BASE + int_no] = isr_ip;
}
