//
// Created by Max Wang on 2026/1/3.
//
#include "memory.h"

#include <string.h>

#include "mmio.h"
#include "panic.h"
#include "vm.h"

#ifdef VM_MEMCHECK
static inline void memcheck_align(VM *vm, vm_addr_t addr, size_t align, const char *op) {
    if ((addr % align) != 0) {
        panic(panic_format("%s unaligned address: 0x%08x", op, addr), vm);
    }
}
#endif

uint8_t vm_read8(VM *vm, vm_addr_t addr) {
    size_t fb_index = 0;
    if (fb_byte_index(vm, addr, &fb_index)) {
        vm_shared_lock(vm);
        uint8_t v = ((uint8_t *) vm->fb)[fb_index];
        vm_shared_unlock(vm);
        return v;
    }
    if (!in_ram(vm, addr, 1)) {
        panic(panic_format("READ8 out of bounds: 0x%08x", addr), vm);
        return 0;
    }
    return vm->memory[addr];
}

uint32_t vm_read32(VM *vm, vm_addr_t addr) {
#ifdef VM_MEMCHECK
    memcheck_align(vm, addr, 4, "READ32");
#endif
    MMIO_Device *dev = find_mmio(vm, addr);
    if (dev) {
        vm_shared_lock(vm);
        uint32_t v = vm_mmio_read32(vm, addr);
        vm_shared_unlock(vm);
        return v;
    }

    if (!in_ram(vm, addr, 4)) {
        panic(panic_format("READ32 out of bounds: 0x%08x", addr), vm);
        return 0;
    }
    return load_le32(&vm->memory[addr]);
}

uint64_t vm_read64(VM *vm, vm_addr_t addr) {
#ifdef VM_MEMCHECK
    memcheck_align(vm, addr, 8, "READ64");
#endif
    uint64_t lo = vm_read32(vm, addr);
    uint64_t hi = vm_read32(vm, addr + 4);
    return lo | (hi << 32);
}

uint32_t vm_atomic_load32_acquire(VM *vm, vm_addr_t addr) {
    _Atomic uint32_t *ptr = atomic32_ptr_or_panic(vm, addr, "LDAR");
    if (!ptr) {
        return 0;
    }
    return atomic_load_explicit(ptr, memory_order_acquire);
}

void vm_atomic_store32_release(VM *vm, vm_addr_t addr, uint32_t value) {
    _Atomic uint32_t *ptr = atomic32_ptr_or_panic(vm, addr, "STLR");
    if (!ptr) {
        return;
    }
    atomic_store_explicit(ptr, value, memory_order_release);
}

uint32_t vm_atomic_exchange32_seqcst(VM *vm, vm_addr_t addr, uint32_t value) {
    _Atomic uint32_t *ptr = atomic32_ptr_or_panic(vm, addr, "XCHG");
    if (!ptr) {
        return 0;
    }
    return atomic_exchange_explicit(ptr, value, memory_order_seq_cst);
}

uint32_t vm_atomic_fetch_add32_seqcst(VM *vm, vm_addr_t addr, uint32_t value) {
    _Atomic uint32_t *ptr = atomic32_ptr_or_panic(vm, addr, "XADD");
    if (!ptr) {
        return 0;
    }
    return atomic_fetch_add_explicit(ptr, value, memory_order_seq_cst);
}

uint32_t vm_atomic_compare_exchange32_seqcst(VM *vm,
                                             vm_addr_t addr,
                                             uint32_t expected,
                                             uint32_t desired,
                                             int *success) {
    _Atomic uint32_t *ptr = atomic32_ptr_or_panic(vm, addr, "CAS");
    if (!ptr) {
        if (success) {
            *success = 0;
        }
        return 0;
    }
    uint32_t observed = expected;
    int ok = atomic_compare_exchange_strong_explicit(ptr,
                                                     &observed,
                                                     desired,
                                                     memory_order_seq_cst,
                                                     memory_order_seq_cst);
    if (success) {
        *success = ok ? 1 : 0;
    }
    return observed;
}

void vm_write8(VM *vm, vm_addr_t addr, uint8_t value) {
    size_t fb_index = 0;
    if (fb_byte_index(vm, addr, &fb_index)) {
        vm_shared_lock(vm);
        ((uint8_t *) vm->fb)[fb_index] = value;
        vm_shared_unlock(vm);
        return;
    }

    if (!in_ram(vm, addr, 1)) {
        panic(panic_format("WRITE8 out of bounds: 0x%08x", addr), vm);
        return;
    }

    vm->memory[addr] = value;
}

void vm_write32(VM *vm, vm_addr_t addr, uint32_t value) {
#ifdef VM_MEMCHECK
    memcheck_align(vm, addr, 4, "WRITE32");
#endif
    MMIO_Device *dev = find_mmio(vm, addr);
    if (dev && dev->write32) {
        vm_shared_lock(vm);
        dev->write32(vm, addr, value);
        vm_shared_unlock(vm);
        return;
    }
    if (!in_ram(vm, addr, 4)) {
        panic(panic_format("WRITE32 out of bounds: 0x%08x", addr), vm);
        return;
    }
    store_le32(&vm->memory[addr], value);
}

void vm_write64(VM *vm, vm_addr_t addr, uint64_t value) {
#ifdef VM_MEMCHECK
    memcheck_align(vm, addr, 8, "WRITE64");
#endif
    if (!in_ram(vm, addr, 8)) {
        panic(panic_format("WRITE64 out of bounds: 0x%08x", addr), vm);
        return;
    }
    vm->memory[addr + 0] = (uint8_t)((value >> 0) & 0xFF);
    vm->memory[addr + 1] = (uint8_t)((value >> 8) & 0xFF);
    vm->memory[addr + 2] = (uint8_t)((value >> 16) & 0xFF);
    vm->memory[addr + 3] = (uint8_t)((value >> 24) & 0xFF);
    vm->memory[addr + 4] = (uint8_t)((value >> 32) & 0xFF);
    vm->memory[addr + 5] = (uint8_t)((value >> 40) & 0xFF);
    vm->memory[addr + 6] = (uint8_t)((value >> 48) & 0xFF);
    vm->memory[addr + 7] = (uint8_t)((value >> 56) & 0xFF);
}
