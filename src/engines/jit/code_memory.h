#ifndef VM_ENGINES_JIT_CODE_MEMORY_H
#define VM_ENGINES_JIT_CODE_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#include "codegen.h"

typedef struct VmJitCodeArena {
    void *mapping;
    size_t mapping_size;
    size_t slot_size;
    size_t slot_count;
    size_t page_size;
} VmJitCodeArena;

int vm_jit_code_arena_init(VmJitCodeArena *arena,
                           size_t slot_count,
                           size_t slot_size);
void vm_jit_code_arena_destroy(VmJitCodeArena *arena);
int vm_jit_code_assign_slot(const VmJitCodeArena *arena,
                            size_t slot_index,
                            VmJitCode *code);

int vm_jit_code_publish(const uint32_t *words,
                        size_t word_count,
                        VmJitCode *out);
void vm_jit_code_destroy(VmJitCode *code);

#endif
