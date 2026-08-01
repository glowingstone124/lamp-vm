#include "engine.h"

uint32_t vm_engine_execute_quantum(VM *vm, VCPU *cpu) {
    switch (vm->execution_engine) {
        case VM_ENGINE_CACHED:
            return vm_engine_execute_cached(vm, cpu);
        case VM_ENGINE_THREADED:
            return vm_engine_execute_threaded(vm, cpu);
        case VM_ENGINE_CLASSIC:
        default:
            return vm_engine_execute_classic(vm, cpu);
    }
}
