#ifndef VM_ENGINES_ENGINE_H
#define VM_ENGINES_ENGINE_H

#include <stdint.h>

#include "../vm.h"

/* Execute one scheduling quantum and return the number of guest instructions. */
uint32_t vm_engine_execute_quantum(VM *vm, VCPU *cpu);

/* Execute exactly one guest instruction. Debuggers use the conservative
 * interpreter path so block-oriented engines cannot run past the requested
 * stop point. */
uint32_t vm_engine_execute_single(VM *vm, VCPU *cpu);

/* Explicit backend entry points are useful for differential self-tests. */
uint32_t vm_engine_execute_classic(VM *vm, VCPU *cpu);
uint32_t vm_engine_execute_cached(VM *vm, VCPU *cpu);
uint32_t vm_engine_execute_threaded(VM *vm, VCPU *cpu);
uint32_t vm_engine_execute_jit(VM *vm, VCPU *cpu);

/* Release engine-owned per-vCPU caches before the VCPU array is freed. */
void vm_engine_destroy_vm(VM *vm);

#endif
