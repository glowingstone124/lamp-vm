//
// Created by Max Wang on 2026/1/18.
//

#include "mmio.h"

#include "panic.h"

uint32_t vm_mmio_read32(VM* vm, uint32_t addr) {
    MMIO_Device *dev = find_mmio(vm,addr);
    if (!dev || !dev->read32) {
        panic(panic_format("READ32 invalid MMIO at 0x%08x", addr),vm);
        return 0;
    }
    return dev->read32(vm,addr);
}

void vm_mmio_write32(VM *vm, uint32_t addr, uint32_t val) {
    MMIO_Device *dev = find_mmio(vm,addr);
    if (!dev || !dev->write32) {
        panic(panic_format("WRITE32 invalid MMIO at 0x%08x", addr),vm);
        return;
    }
    dev->write32(vm,addr,val);
}
