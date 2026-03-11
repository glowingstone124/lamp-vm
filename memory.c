//
// Created by Max Wang on 2026/1/3.
//
#include "memory.h"

#include <string.h>

#include "mmio.h"
#include "panic.h"
#include "vm.h"
#include "io_devices/mmu/mmu_mmio_register.h"

#ifdef VM_MEMCHECK
static inline void memcheck_align(VM *vm, vm_addr_t addr, size_t align, const char *op) {
    if ((addr % align) != 0) {
        panic(panic_format("%s unaligned address: 0x%08x", op, addr), vm);
    }
}
#endif

static int vm_translate_or_panic(VM *vm, vm_addr_t addr, uint32_t access, const char *op, uint32_t *pa_out) {
    uint32_t pa;
    if (!vm_mmu_translate_access(vm, addr, access, &pa)) {
        panic(panic_format("%s page fault va: 0x%08x status=0x%08x info=0x%08x root_lo=0x%08x",
                           op,
                           addr,
                           vm ? vm_mmu_fault_status(vm) : 0u,
                           vm ? vm_mmu_fault_info(vm) : 0u,
                           vm ? vm_mmu_root_lo(vm) : 0u),
              vm);
        return 0;
    }
    *pa_out = pa;
    return 1;
}

static int vm_translate_span_or_panic(VM *vm,
                                      vm_addr_t addr,
                                      uint32_t len,
                                      uint32_t access,
                                      const char *op,
                                      uint32_t *pa_out) {
    for (uint32_t i = 0u; i < len; i++) {
        if (!vm_translate_or_panic(vm, addr + i, access, op, &pa_out[i])) {
            return 0;
        }
    }
    return 1;
}

static int vm_span_is_contiguous(const uint32_t *pa, uint32_t len) {
    if (!pa || len == 0u) {
        return 0;
    }
    for (uint32_t i = 1u; i < len; i++) {
        if (pa[i] != pa[0] + i) {
            return 0;
        }
    }
    return 1;
}

static _Atomic uint32_t *atomic32_ptr_from_va_or_panic(VM *vm,
                                                        vm_addr_t addr,
                                                        uint32_t access,
                                                        const char *op_name) {
    uint32_t pa[4];
    if ((addr % _Alignof(_Atomic uint32_t)) != 0u) {
        panic(panic_format("%s unaligned address: 0x%08x", op_name, addr), vm);
        return NULL;
    }
    if (!vm_translate_span_or_panic(vm, addr, 4u, access, op_name, pa)) {
        return NULL;
    }
    if (!vm_span_is_contiguous(pa, 4u)) {
        panic(panic_format("%s non-contiguous mapping: 0x%08x", op_name, addr), vm);
        return NULL;
    }
    if ((pa[0] % _Alignof(_Atomic uint32_t)) != 0u) {
        panic(panic_format("%s unaligned physical address: 0x%08x", op_name, pa[0]), vm);
        return NULL;
    }
    if (find_mmio(vm, pa[0]) != NULL) {
        panic(panic_format("%s does not support MMIO addr: 0x%08x", op_name, pa[0]), vm);
        return NULL;
    }
    if (!in_ram(vm, pa[0], sizeof(uint32_t))) {
        panic(panic_format("%s out of bounds: 0x%08x", op_name, pa[0]), vm);
        return NULL;
    }
    return (_Atomic uint32_t *)(void *)(&vm->memory[pa[0]]);
}

uint8_t vm_read8(VM *vm, vm_addr_t addr) {
    uint32_t pa;
    size_t fb_index = 0;
    if (!vm_translate_or_panic(vm, addr, VM_MMU_ACC_READ, "READ8", &pa)) {
        return 0u;
    }
    if (fb_byte_index(vm, pa, &fb_index)) {
        vm_shared_lock(vm);
        uint8_t v = ((uint8_t *) vm->fb)[fb_index];
        vm_shared_unlock(vm);
        return v;
    }
    if (!in_ram(vm, pa, 1)) {
        panic(panic_format("READ8 out of bounds: 0x%08x", pa), vm);
        return 0;
    }
    return vm->memory[pa];
}

uint32_t vm_read32(VM *vm, vm_addr_t addr) {
    uint32_t pa[4];
#ifdef VM_MEMCHECK
    memcheck_align(vm, addr, 4, "READ32");
#endif
    if (!vm_translate_span_or_panic(vm, addr, 4u, VM_MMU_ACC_READ, "READ32", pa)) {
        return 0u;
    }

    if (vm_span_is_contiguous(pa, 4u)) {
        MMIO_Device *dev = find_mmio(vm, pa[0]);
        if (dev) {
            vm_shared_lock(vm);
            uint32_t v = vm_mmio_read32(vm, pa[0]);
            vm_shared_unlock(vm);
            return v;
        }
        if (!in_ram(vm, pa[0], 4u)) {
            panic(panic_format("READ32 out of bounds: 0x%08x", pa[0]), vm);
            return 0u;
        }
        return load_le32(&vm->memory[pa[0]]);
    }

    uint32_t v = 0u;
    for (uint32_t i = 0u; i < 4u; i++) {
        if (find_mmio(vm, pa[i])) {
            panic(panic_format("READ32 split MMIO unsupported: 0x%08x", addr), vm);
            return 0u;
        }
        if (!in_ram(vm, pa[i], 1u)) {
            panic(panic_format("READ32 out of bounds: 0x%08x", pa[i]), vm);
            return 0u;
        }
        v |= ((uint32_t)vm->memory[pa[i]]) << (i * 8u);
    }
    return v;
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
    _Atomic uint32_t *ptr = atomic32_ptr_from_va_or_panic(vm, addr, VM_MMU_ACC_READ, "LDAR");
    if (!ptr) {
        return 0;
    }
    return atomic_load_explicit(ptr, memory_order_acquire);
}

void vm_atomic_store32_release(VM *vm, vm_addr_t addr, uint32_t value) {
    _Atomic uint32_t *ptr = atomic32_ptr_from_va_or_panic(vm, addr, VM_MMU_ACC_WRITE, "STLR");
    if (!ptr) {
        return;
    }
    atomic_store_explicit(ptr, value, memory_order_release);
}

uint32_t vm_atomic_exchange32_seqcst(VM *vm, vm_addr_t addr, uint32_t value) {
    _Atomic uint32_t *ptr = atomic32_ptr_from_va_or_panic(vm, addr, VM_MMU_ACC_READ | VM_MMU_ACC_WRITE, "XCHG");
    if (!ptr) {
        return 0;
    }
    return atomic_exchange_explicit(ptr, value, memory_order_seq_cst);
}

uint32_t vm_atomic_fetch_add32_seqcst(VM *vm, vm_addr_t addr, uint32_t value) {
    _Atomic uint32_t *ptr = atomic32_ptr_from_va_or_panic(vm, addr, VM_MMU_ACC_READ | VM_MMU_ACC_WRITE, "XADD");
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
    _Atomic uint32_t *ptr = atomic32_ptr_from_va_or_panic(vm, addr, VM_MMU_ACC_READ | VM_MMU_ACC_WRITE, "CAS");
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
    uint32_t pa;
    size_t fb_index = 0;
    if (!vm_translate_or_panic(vm, addr, VM_MMU_ACC_WRITE, "WRITE8", &pa)) {
        return;
    }
    if (fb_byte_index(vm, pa, &fb_index)) {
        vm_shared_lock(vm);
        ((uint8_t *) vm->fb)[fb_index] = value;
        vm_shared_unlock(vm);
        return;
    }

    if (!in_ram(vm, pa, 1)) {
        panic(panic_format("WRITE8 out of bounds: 0x%08x", pa), vm);
        return;
    }

    vm->memory[pa] = value;
}

void vm_write32(VM *vm, vm_addr_t addr, uint32_t value) {
    uint32_t pa[4];
#ifdef VM_MEMCHECK
    memcheck_align(vm, addr, 4, "WRITE32");
#endif
    if (!vm_translate_span_or_panic(vm, addr, 4u, VM_MMU_ACC_WRITE, "WRITE32", pa)) {
        return;
    }

    if (vm_span_is_contiguous(pa, 4u)) {
        MMIO_Device *dev = find_mmio(vm, pa[0]);
        if (dev && dev->write32) {
            vm_shared_lock(vm);
            dev->write32(vm, pa[0], value);
            vm_shared_unlock(vm);
            return;
        }
        if (!in_ram(vm, pa[0], 4u)) {
            panic(panic_format("WRITE32 out of bounds: 0x%08x", pa[0]), vm);
            return;
        }
        store_le32(&vm->memory[pa[0]], value);
        return;
    }

    for (uint32_t i = 0u; i < 4u; i++) {
        if (find_mmio(vm, pa[i])) {
            panic(panic_format("WRITE32 split MMIO unsupported: 0x%08x", addr), vm);
            return;
        }
        if (!in_ram(vm, pa[i], 1u)) {
            panic(panic_format("WRITE32 out of bounds: 0x%08x", pa[i]), vm);
            return;
        }
        vm->memory[pa[i]] = (uint8_t)((value >> (i * 8u)) & 0xFFu);
    }
}

void vm_write64(VM *vm, vm_addr_t addr, uint64_t value) {
#ifdef VM_MEMCHECK
    memcheck_align(vm, addr, 8, "WRITE64");
#endif
    vm_write32(vm, addr, (uint32_t)(value & 0xFFFFFFFFu));
    vm_write32(vm, addr + 4u, (uint32_t)((value >> 32) & 0xFFFFFFFFu));
}

uint32_t vm_fetch64_exec(VM *vm, vm_addr_t addr, uint64_t *out_inst) {
    uint32_t pa[8];
    uint64_t inst = 0u;
    if (!out_inst) {
        return 0u;
    }
    if (!vm_translate_span_or_panic(vm, addr, 8u, VM_MMU_ACC_EXEC, "IFETCH", pa)) {
        return 0u;
    }

    if (vm_span_is_contiguous(pa, 8u)) {
        if (find_mmio(vm, pa[0])) {
            panic(panic_format("IFETCH from MMIO: 0x%08x", addr), vm);
            return 0u;
        }
        if (!in_ram(vm, pa[0], 8u)) {
            panic(panic_format("IFETCH out of bounds: 0x%08x", pa[0]), vm);
            return 0u;
        }
        memcpy(&inst, &vm->memory[pa[0]], sizeof(inst));
        *out_inst = inst;
        return 1u;
    }

    for (uint32_t i = 0u; i < 8u; i++) {
        if (find_mmio(vm, pa[i])) {
            panic(panic_format("IFETCH split MMIO unsupported: 0x%08x", addr), vm);
            return 0u;
        }
        if (!in_ram(vm, pa[i], 1u)) {
            panic(panic_format("IFETCH out of bounds: 0x%08x", pa[i]), vm);
            return 0u;
        }
        inst |= ((uint64_t)vm->memory[pa[i]]) << (i * 8u);
    }
    *out_inst = inst;
    return 1u;
}
