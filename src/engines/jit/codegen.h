#ifndef VM_ENGINES_JIT_CODEGEN_H
#define VM_ENGINES_JIT_CODEGEN_H

#include <stddef.h>
#include <stdint.h>

#include "../../vm.h"

#define VM_JIT_BLOCK_MAX_OPS VM_THREADED_BLOCK_MAX_OPS
#define VM_JIT_NATIVE_LOOP_BUDGET 256u
#define VM_JIT_ENTRY_MAX_INSTRUCTIONS \
    (VM_JIT_NATIVE_LOOP_BUDGET + VM_JIT_BLOCK_MAX_OPS)

typedef uint32_t (*VmJitEntryFn)(VM *vm, VCPU *cpu);
typedef uint8_t (*VmJitRead8Fn)(VM *vm, VCPU *cpu, vm_addr_t addr);
typedef uint32_t (*VmJitRead32Fn)(VM *vm, VCPU *cpu, vm_addr_t addr);
typedef void (*VmJitWrite8Fn)(VM *vm,
                              VCPU *cpu,
                              vm_addr_t addr,
                              uint8_t value);
typedef void (*VmJitWrite32Fn)(VM *vm,
                               VCPU *cpu,
                               vm_addr_t addr,
                               uint32_t value);

/*
 * Guest-memory behavior belongs to the target backend.  Tier 0 uses the
 * complete CPU-aware C helpers through this table.  A later backend revision
 * can emit a guarded RAM/TLB fast path while retaining these exact slow exits.
 */
typedef struct VmJitMemoryOps {
    VmJitRead8Fn read8;
    VmJitRead32Fn read32;
    VmJitWrite8Fn write8;
    VmJitWrite32Fn write32;
} VmJitMemoryOps;

typedef struct VmJitBlock {
    uint64_t mmu_epoch;
    uint64_t code_page_generation;
    vm_addr_t start_ip;
    uint32_t host_pa;
    uint32_t mmio_epoch;
    uint16_t count;
    uint8_t reserved[3];
    uint64_t raw[VM_JIT_BLOCK_MAX_OPS];
    VM_DecodedOp ops[VM_JIT_BLOCK_MAX_OPS];
} VmJitBlock;

typedef struct VmJitCode {
    void *mapping;
    size_t mapping_size;
    size_t code_size;
    VmJitEntryFn entry;
    uint8_t arena_backed;
} VmJitCode;

typedef struct VmJitBackend {
    const char *name;
    int (*available)(void);
    int (*supports_opcode)(uint8_t op);
    int (*terminates_block)(uint8_t op);
    int (*compile)(const VmJitBlock *block,
                   const VmJitMemoryOps *memory,
                   VmJitCode *out);
    const VmJitMemoryOps *(*memory_ops)(void);
} VmJitBackend;

const VmJitBackend *vm_jit_codegen_host_backend(void);

/* Both target paths are always present in the abstract dispatcher. */
const VmJitBackend *vm_jit_arm64_backend(void);
const VmJitBackend *vm_jit_x86_64_backend(void);
const VmJitMemoryOps *vm_jit_arm64_memory_ops(void);
const VmJitMemoryOps *vm_jit_x86_64_memory_ops(void);

#endif
