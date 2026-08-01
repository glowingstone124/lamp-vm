#ifndef VM_ENGINES_ENGINE_H
#define VM_ENGINES_ENGINE_H

#include <stdint.h>

#include "../vm.h"

/* Execute one scheduling quantum and return the number of guest instructions. */
uint32_t vm_engine_execute_quantum(VM *vm, VCPU *cpu);

/* Explicit backend entry points are useful for differential self-tests. */
uint32_t vm_engine_execute_classic(VM *vm, VCPU *cpu);
uint32_t vm_engine_execute_cached(VM *vm, VCPU *cpu);
uint32_t vm_engine_execute_threaded(VM *vm, VCPU *cpu);

#endif
