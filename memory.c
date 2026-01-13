//
// Created by Max Wang on 2026/1/3.
//
#include "memory.h"

#include <string.h>

#include "panic.h"
#include "vm.h"

static inline int in_ram(VM *vm, vm_addr_t addr, size_t size) {
    return addr + size <= vm->memory_size;
}

uint8_t vm_read8(VM *vm, vm_addr_t addr) {
    if (!in_ram(vm, addr, 1)) {
        panic(panic_format("READ8 out of bounds: 0x%08x", addr), vm);
        return 0;
    }
    return vm->memory[addr];
}

uint32_t vm_read32(VM *vm, vm_addr_t addr) {
    if (!in_ram(vm, addr, 4)) {
        panic(panic_format("READ32 out of bounds: 0x%08x", addr), vm);
        return 0;
    }

    uint32_t v = 0;
    v |= vm->memory[addr + 0] << 0;
    v |= vm->memory[addr + 1] << 8;
    v |= vm->memory[addr + 2] << 16;
    v |= vm->memory[addr + 3] << 24;
    return v;
}

uint64_t vm_read64(VM *vm, vm_addr_t addr) {
    uint64_t lo = vm_read32(vm, addr);
    uint64_t hi = vm_read32(vm, addr + 4);
    return lo | (hi << 32);
}

void vm_write8(VM *vm, vm_addr_t addr, uint8_t value) {
    // intercept mmio request
    size_t fb_base = FB_BASE(vm->memory_size);
    if (addr >= fb_base && addr < fb_base + FB_SIZE) {
        vm->fb[addr - fb_base] = value;
        return;
    }

    if (!in_ram(vm, addr, 1)) {
        panic(panic_format("WRITE8 out of bounds: 0x%08x", addr), vm);
        return;
    }

    vm->memory[addr] = value;
}

void vm_write32(VM *vm, vm_addr_t addr, uint32_t value) {
    size_t fb_base = FB_BASE(vm->memory_size);
    if (addr >= fb_base && addr + 3 < fb_base + FB_SIZE) {
        size_t pixel_index = (addr - fb_base) / 4;
        vm->fb[pixel_index] = value;

        return;
    }

    if (!in_ram(vm, addr, 4)) {
        panic(panic_format("WRITE32 out of bounds: 0x%08x", addr), vm);
        return;
    }

    vm->memory[addr + 0] = (value >> 0) & 0xFF;
    vm->memory[addr + 1] = (value >> 8) & 0xFF;
    vm->memory[addr + 2] = (value >> 16) & 0xFF;
    vm->memory[addr + 3] = (value >> 24) & 0xFF;
}

void vm_write64(VM *vm, vm_addr_t addr, uint64_t value) {
    if (!in_ram(vm, addr, 8)) {
        panic(panic_format("WRITE64 out of bounds: 0x%08x", addr), vm);
        return;
    }
    vm->memory[addr + 0] = (value >> 0) & 0xFF;
    vm->memory[addr + 1] = (value >> 8) & 0xFF;
    vm->memory[addr + 2] = (value >> 16) & 0xFF;
    vm->memory[addr + 3] = (value >> 24) & 0xFF;
    vm->memory[addr + 4] = (value >> 32) & 0xFF;
    vm->memory[addr + 5] = (value >> 40) & 0xFF;
    vm->memory[addr + 6] = (value >> 48) & 0xFF;
    vm->memory[addr + 7] = (value >> 56) & 0xFF;
}
