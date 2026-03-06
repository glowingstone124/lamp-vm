//
// Created by Max Wang on 2026/1/3.
//

#ifndef VM_MEMORY_H
#define VM_MEMORY_H
#include <stdint.h>
#include <string.h>

#include "mmio.h"
#include "panic.h"
#include "vm.h"

typedef uint32_t vm_addr_t;

uint8_t vm_read8(VM *vm, vm_addr_t addr);
uint32_t vm_read32(VM *vm, vm_addr_t addr);
uint64_t vm_read64(VM *vm, vm_addr_t addr);
uint32_t vm_atomic_load32_acquire(VM *vm, vm_addr_t addr);
void vm_atomic_store32_release(VM *vm, vm_addr_t addr, uint32_t value);
uint32_t vm_atomic_exchange32_seqcst(VM *vm, vm_addr_t addr, uint32_t value);
uint32_t vm_atomic_fetch_add32_seqcst(VM *vm, vm_addr_t addr, uint32_t value);
uint32_t vm_atomic_compare_exchange32_seqcst(VM *vm,
                                             vm_addr_t addr,
                                             uint32_t expected,
                                             uint32_t desired,
                                             int *success);
static inline int in_ram(VM *vm, vm_addr_t addr, size_t size) {
    return addr + size <= vm->memory_size;
}
static inline _Atomic uint32_t *atomic32_ptr_or_panic(VM *vm, vm_addr_t addr, const char *op_name) {
    if ((addr % _Alignof(_Atomic uint32_t)) != 0) {
        panic(panic_format("%s unaligned address: 0x%08x", op_name, addr), vm);
        return NULL;
    }
    if (!in_ram(vm, addr, sizeof(uint32_t))) {
        panic(panic_format("%s out of bounds: 0x%08x", op_name, addr), vm);
        return NULL;
    }
    if (find_mmio(vm, addr) != NULL) {
        panic(panic_format("%s does not support MMIO addr: 0x%08x", op_name, addr), vm);
        return NULL;
    }
    return (_Atomic uint32_t *)(void *)(&vm->memory[addr]);
}

static inline int fb_byte_index(VM *vm, vm_addr_t addr, size_t *out_index) {
    const size_t fb_base = FB_BASE(vm->memory_size);
    if (addr >= fb_base && addr < fb_base + FB_SIZE) {
        *out_index = (size_t)(addr - fb_base);
        return 1;
    }
    if (addr >= FB_LEGACY_BASE && addr < FB_LEGACY_BASE + FB_SIZE) {
        *out_index = (size_t)(addr - FB_LEGACY_BASE);
        return 1;
    }
    return 0;
}

static inline uint32_t load_le32(const void *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap32(v);
#endif
    return v;
}

static inline void store_le32(void *p, uint32_t v) {
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap32(v);
#endif
    memcpy(p, &v, sizeof(v));
}
void vm_write8(VM *vm, vm_addr_t addr, uint8_t value);
void vm_write32(VM *vm, vm_addr_t addr, uint32_t value);
void vm_write64(VM *vm, vm_addr_t addr, uint64_t value);
#endif // VM_MEMORY_H
