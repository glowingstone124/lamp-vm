//
// Created by Max Wang on 2026/1/3.
//

#ifndef VM_MEMORY_H
#define VM_MEMORY_H
#include <stdint.h>

#include "vm.h"

typedef uint32_t vm_addr_t;

uint8_t vm_read8(VM *vm, vm_addr_t addr);
uint32_t vm_read32(VM *vm, vm_addr_t addr);
uint64_t vm_read64(VM *vm, vm_addr_t addr);

void vm_write8(VM *vm, vm_addr_t addr, uint8_t value);
void vm_write32(VM *vm, vm_addr_t addr, uint32_t value);
void vm_write64(VM *vm, vm_addr_t addr, uint64_t value);
#endif // VM_MEMORY_H