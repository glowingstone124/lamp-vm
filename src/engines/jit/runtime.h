#ifndef VM_ENGINES_JIT_RUNTIME_H
#define VM_ENGINES_JIT_RUNTIME_H

#include <stdint.h>

#include "../../vm.h"

void vm_jit_runtime_logic_flags(VM *vm, VCPU *cpu, int32_t result);
void vm_jit_runtime_add_flags(VM *vm,
                              VCPU *cpu,
                              int32_t a,
                              int32_t b,
                              int32_t result);
void vm_jit_runtime_sub_flags(VM *vm,
                              VCPU *cpu,
                              int32_t a,
                              int32_t b,
                              int32_t result);

#endif
