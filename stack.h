//
// Created by Max Wang on 2025/12/28.
//

#ifndef VM_STACK_H
#define VM_STACK_H
#include "memory.h"
#include "panic.h"

static inline void data_push(VM *vm, uint8_t val) {
    if (vm->dsp == 0) {
        panic("Data stack overflow", vm);
        return;
    }
    vm->dsp--;
    vm->memory[DATA_STACK_BASE + vm->dsp] = val;
}

static inline uint8_t data_pop(VM *vm) {
    if (vm->dsp >= DATA_STACK_SIZE) {
        panic("Data stack underflow", vm);
        return 0;
    }
    uint8_t val = vm->memory[DATA_STACK_BASE + vm->dsp];
    vm->dsp++;
    return val;
}

static inline void call_push(VM *vm, uint64_t val) {
    if (vm->csp == 0) {
        panic("Call stack overflow", vm);
        return;
    }
    vm->csp--;
    vm_write64(vm, CALL_STACK_BASE + vm->csp * 8, val);
}

static inline uint64_t call_pop(VM *vm) {
    if (vm->csp >= CALL_STACK_SIZE) {
        panic("Call stack underflow", vm);
        return 0;
    }
    uint64_t val = vm_read64(vm, CALL_STACK_BASE + vm->csp * 8);
    vm->csp++;
    return val;
}

#endif // VM_STACK_H
