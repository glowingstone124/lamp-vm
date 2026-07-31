//
// Created by Max Wang on 2026/1/18.
//

#ifndef VM_MMIO_H
#define VM_MMIO_H
#include "vm.h"

static inline MMIO_Device *find_mmio(VM *vm, uint32_t addr) {
    static _Thread_local VM *cached_vm;
    static _Thread_local MMIO_Device *cached_dev;
    static _Thread_local uint32_t cached_start;
    static _Thread_local uint32_t cached_end;

    if (vm == cached_vm && cached_dev && addr >= cached_start && addr <= cached_end) {
        return cached_dev;
    }

    if (vm->mmio_page_map_ready != 0u) {
        const uint32_t page = addr >> VM_MMIO_PAGE_SHIFT;
        const uint8_t mask = (uint8_t)(1u << (page & 7u));
        if ((vm->mmio_page_map[page >> 3u] & mask) == 0u) {
            return NULL;
        }
    }

    for (int i = 0; i < vm->mmio_count; i++) {
        MMIO_Device *dev = vm->mmio_devices[i];
        if (addr >= dev->start && addr <= dev->end) {
            cached_vm = vm;
            cached_dev = dev;
            cached_start = dev->start;
            cached_end = dev->end;
            return dev;
        }
    }
    cached_vm = vm;
    cached_dev = NULL;
    cached_start = 0;
    cached_end = 0;
    return NULL;
}


uint32_t vm_mmio_read32(VM* vm, uint32_t addr);
void vm_mmio_write32(VM* vm, uint32_t addr, uint32_t val);
#endif //VM_MMIO_H
