#ifndef VM_RUNTIME_H
#define VM_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "loadbin.h"
#include "vm.h"

extern const size_t MEM_SIZE;

VM *vm_create(size_t memory_size,
              const uint64_t *program,
              size_t program_size,
              const uint8_t *data,
              size_t data_size,
              const ProgramLayout *layout,
              int smp_cores);
int vm_run_headless(VM *vm, uint64_t timeout_ms);
void vm_destroy(VM *vm);

#endif
