#ifndef VM_ENGINES_JIT_JIT_H
#define VM_ENGINES_JIT_JIT_H

#include <stdint.h>

#include "../../vm.h"

uint32_t vm_engine_execute_jit(VM *vm, VCPU *cpu);
void vm_jit_destroy_cpu(VCPU *cpu);

#endif
