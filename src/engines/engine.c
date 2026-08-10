#include "engine.h"

#include "jit/jit.h"

uint32_t vm_engine_execute_quantum(VM *vm, VCPU *cpu) {
    switch (vm->execution_engine) {
        case VM_ENGINE_CACHED:
            return vm_engine_execute_cached(vm, cpu);
        case VM_ENGINE_THREADED:
            return vm_engine_execute_threaded(vm, cpu);
        case VM_ENGINE_JIT:
            return vm_engine_execute_jit(vm, cpu);
        case VM_ENGINE_CLASSIC:
        default:
            return vm_engine_execute_classic(vm, cpu);
    }
}

uint32_t vm_engine_execute_single(VM *vm, VCPU *cpu) {
    return vm_engine_execute_classic(vm, cpu);
}

void vm_engine_destroy_vm(VM *vm) {
    if (!vm || !vm->cpus) {
        return;
    }
    for (int core = 0; core < vm->smp_cores; core++) {
        vm_jit_destroy_cpu(&vm->cpus[core]);
    }
}
