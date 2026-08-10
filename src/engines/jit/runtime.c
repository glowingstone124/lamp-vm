#include "runtime.h"

#include "../engine_internal.h"

void vm_jit_runtime_logic_flags(VM *vm, VCPU *cpu, int32_t result) {
    vm_engine_update_logic_flags(vm, result, cpu);
}

void vm_jit_runtime_add_flags(VM *vm,
                              VCPU *cpu,
                              int32_t a,
                              int32_t b,
                              int32_t result) {
    vm_engine_update_add_flags(vm, a, b, result, cpu);
}

void vm_jit_runtime_sub_flags(VM *vm,
                              VCPU *cpu,
                              int32_t a,
                              int32_t b,
                              int32_t result) {
    vm_engine_update_sub_flags(vm, a, b, result, cpu);
}
