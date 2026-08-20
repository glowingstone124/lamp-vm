//
// Created by Max Wang on 2026/1/3.
//
#include "memory.h"

#include <string.h>

#include "mmio.h"
#include "panic.h"
#include "vm.h"
#include "io_devices/mmu/mmu_mmio_register.h"

void vm_ram_enable_write_tracking(VM *vm) {
    if (!vm || !vm->ram_page_generations || vm->ram_page_count == 0u) {
        return;
    }
    atomic_store_explicit(&vm->ram_write_tracking_active, true,
                          memory_order_release);
}

uint64_t vm_ram_page_generation_acquire(const VM *vm, uint32_t pa) {
    const size_t page = (size_t)pa >> VM_RAM_PAGE_SHIFT;
    if (!vm || !vm->ram_page_generations || page >= vm->ram_page_count) {
        return UINT64_MAX;
    }
    return atomic_load_explicit(&vm->ram_page_generations[page],
                                memory_order_acquire);
}

void vm_ram_mark_written(VM *vm, uint32_t pa, size_t size) {
    size_t first_page;
    size_t last_page;
    size_t last_byte;
    if (!vm || size == 0u || !vm->ram_page_generations ||
        !atomic_load_explicit(&vm->ram_write_tracking_active,
                              memory_order_acquire) ||
        (size_t)pa >= vm->memory_size || size > vm->memory_size - (size_t)pa) {
        return;
    }
    first_page = (size_t)pa >> VM_RAM_PAGE_SHIFT;
    last_byte = (size_t)pa + size - 1u;
    last_page = last_byte >> VM_RAM_PAGE_SHIFT;
    for (size_t page = first_page; page <= last_page; page++) {
        atomic_fetch_add_explicit(&vm->ram_page_generations[page], 1u,
                                  memory_order_release);
    }
}

#ifdef VM_MEMCHECK
static inline void memcheck_align(VM *vm, vm_addr_t addr, size_t align, const char *op) {
    if ((addr % align) != 0) {
        panic(panic_format("%s unaligned address: 0x%08x", op, addr), vm);
    }
}
#endif

static int vm_translate_or_panic_cpu(VM *vm,
                                     VCPU *cpu,
                                     vm_addr_t addr,
                                     uint32_t access,
                                     const char *op,
                                     uint32_t *pa_out) {
    uint32_t pa;
    if (!vm_mmu_translate_access_cpu(vm, cpu, addr, access, &pa)) {
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

static int vm_translate_span_or_panic_cpu(VM *vm,
                                      VCPU *cpu,
                                      vm_addr_t addr,
                                      uint32_t len,
                                      uint32_t access,
                                      const char *op,
                                      uint32_t *pa_out) {
    for (uint32_t i = 0u; i < len; i++) {
        if (!vm_translate_or_panic_cpu(vm, cpu, addr + i, access, op, &pa_out[i])) {
            return 0;
        }
    }
    return 1;
}

static int vm_translate_contiguous_span_or_panic_cpu(VM *vm,
                                                 VCPU *cpu,
                                                 vm_addr_t addr,
                                                 uint32_t len,
                                                 uint32_t access,
                                                 const char *op,
                                                 uint32_t *pa_out) {
    uint32_t pa;
    if (!pa_out || len == 0u) {
        return 0;
    }

    /*
     * A short access that stays within one guest page is guaranteed to map to
     * contiguous physical bytes after a single page-table translation.
     */
    if (((addr & 0xFFFu) + len) > 0x1000u) {
        return 0;
    }
    if (!vm_translate_or_panic_cpu(vm, cpu, addr, access, op, &pa)) {
        return 0;
    }
    *pa_out = pa;
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

static uint32_t vm_fb_read32_locked(VM *vm, size_t fb_index) {
    size_t first_row;
    size_t last_row;
    uint32_t value;

    if (fb_index > (size_t)FB_SIZE - sizeof(uint32_t)) {
        panic(panic_format("READ32 crosses framebuffer boundary at byte 0x%zx", fb_index), vm);
        return 0u;
    }
    first_row = vm_fb_row_from_byte_index(fb_index);
    last_row = vm_fb_row_from_byte_index(fb_index + sizeof(uint32_t) - 1u);
    vm_fb_row_lock(vm, first_row);
    if (last_row != first_row) {
        vm_fb_row_lock(vm, last_row);
    }
    value = load_le32((const uint8_t *)vm->fb + fb_index);
    if (last_row != first_row) {
        vm_fb_row_unlock(vm, last_row);
    }
    vm_fb_row_unlock(vm, first_row);
    return value;
}

static void vm_fb_write32_locked(VM *vm, size_t fb_index, uint32_t value) {
    size_t first_row;
    size_t last_row;

    if (fb_index > (size_t)FB_SIZE - sizeof(uint32_t)) {
        panic(panic_format("WRITE32 crosses framebuffer boundary at byte 0x%zx", fb_index), vm);
        return;
    }
    first_row = vm_fb_row_from_byte_index(fb_index);
    last_row = vm_fb_row_from_byte_index(fb_index + sizeof(uint32_t) - 1u);
    vm_fb_row_lock(vm, first_row);
    if (last_row != first_row) {
        vm_fb_row_lock(vm, last_row);
    }
    store_le32((uint8_t *)vm->fb + fb_index, value);
    vm_fb_mark_row_dirty(vm, first_row);
    if (last_row != first_row) {
        vm_fb_mark_row_dirty(vm, last_row);
        vm_fb_row_unlock(vm, last_row);
    }
    vm_fb_row_unlock(vm, first_row);
}

static _Atomic uint32_t *atomic32_ptr_from_va_or_panic(VM *vm,
                                                        VCPU *cpu,
                                                        vm_addr_t addr,
                                                        uint32_t access,
                                                        const char *op_name,
                                                        uint32_t *pa_out) {
    uint32_t pa_base;
    uint32_t pa[4];
    if ((addr % _Alignof(_Atomic uint32_t)) != 0u) {
        panic(panic_format("%s unaligned address: 0x%08x", op_name, addr), vm);
        return NULL;
    }
    if (!vm_translate_contiguous_span_or_panic_cpu(vm, cpu, addr, 4u, access, op_name, &pa_base)) {
        if (!vm_translate_span_or_panic_cpu(vm, cpu, addr, 4u, access, op_name, pa)) {
            return NULL;
        }
        if (!vm_span_is_contiguous(pa, 4u)) {
            panic(panic_format("%s non-contiguous mapping: 0x%08x", op_name, addr), vm);
            return NULL;
        }
        pa_base = pa[0];
    }
    if ((pa_base % _Alignof(_Atomic uint32_t)) != 0u) {
        panic(panic_format("%s unaligned physical address: 0x%08x", op_name, pa_base), vm);
        return NULL;
    }
    if (find_mmio(vm, pa_base) != NULL) {
        panic(panic_format("%s does not support MMIO addr: 0x%08x", op_name, pa_base), vm);
        return NULL;
    }
    if (!in_ram(vm, pa_base, sizeof(uint32_t))) {
        panic(panic_format("%s out of bounds: 0x%08x", op_name, pa_base), vm);
        return NULL;
    }
    if (pa_out) {
        *pa_out = pa_base;
    }
    return (_Atomic uint32_t *)(void *)(&vm->memory[pa_base]);
}

uint8_t vm_read8_cpu(VM *vm, VCPU *cpu, vm_addr_t addr) {
    uint32_t pa;
    size_t fb_index = 0;
    if (!vm_translate_or_panic_cpu(vm, cpu, addr, VM_MMU_ACC_READ, "READ8", &pa)) {
        return 0u;
    }
    if (fb_byte_index(vm, pa, &fb_index)) {
        const size_t row = vm_fb_row_from_byte_index(fb_index);
        vm_fb_row_lock(vm, row);
        uint8_t v = ((uint8_t *) vm->fb)[fb_index];
        vm_fb_row_unlock(vm, row);
        return v;
    }
    if (!in_ram(vm, pa, 1)) {
        panic(panic_format("READ8 out of bounds: 0x%08x", pa), vm);
        return 0;
    }
    return vm->memory[pa];
}

uint8_t vm_read8(VM *vm, vm_addr_t addr) {
    return vm_read8_cpu(vm, vm_current_cpu(vm), addr);
}

uint32_t vm_read32_cpu(VM *vm, VCPU *cpu, vm_addr_t addr) {
    uint32_t pa_base;
    uint32_t pa[4];
#ifdef VM_MEMCHECK
    memcheck_align(vm, addr, 4, "READ32");
#endif
    if (vm_translate_contiguous_span_or_panic_cpu(vm, cpu, addr, 4u, VM_MMU_ACC_READ, "READ32", &pa_base)) {
        size_t fb_index = 0u;
        if (fb_byte_index(vm, pa_base, &fb_index)) {
            return vm_fb_read32_locked(vm, fb_index);
        }
        MMIO_Device *dev = find_mmio(vm, pa_base);
        if (dev) {
            vm_shared_lock(vm);
            uint32_t v = vm_mmio_read32(vm, pa_base);
            vm_shared_unlock(vm);
            return v;
        }
        if (!in_ram(vm, pa_base, 4u)) {
            panic(panic_format("READ32 out of bounds: va=0x%08x pa=0x%08x",
                               addr, pa_base), vm);
            return 0u;
        }
        return load_le32(&vm->memory[pa_base]);
    }
    if (!vm_translate_span_or_panic_cpu(vm, cpu, addr, 4u, VM_MMU_ACC_READ, "READ32", pa)) {
        return 0u;
    }

    if (vm_span_is_contiguous(pa, 4u)) {
        size_t fb_index = 0u;
        if (fb_byte_index(vm, pa[0], &fb_index)) {
            return vm_fb_read32_locked(vm, fb_index);
        }
        MMIO_Device *dev = find_mmio(vm, pa[0]);
        if (dev) {
            vm_shared_lock(vm);
            uint32_t v = vm_mmio_read32(vm, pa[0]);
            vm_shared_unlock(vm);
            return v;
        }
        if (!in_ram(vm, pa[0], 4u)) {
            panic(panic_format("READ32 out of bounds: va=0x%08x pa=0x%08x",
                               addr, pa[0]), vm);
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

uint32_t vm_read32(VM *vm, vm_addr_t addr) {
    return vm_read32_cpu(vm, vm_current_cpu(vm), addr);
}

uint64_t vm_read64_cpu(VM *vm, VCPU *cpu, vm_addr_t addr) {
    uint32_t pa_base;
#ifdef VM_MEMCHECK
    memcheck_align(vm, addr, 8, "READ64");
#endif
    if (vm_translate_contiguous_span_or_panic_cpu(vm, cpu, addr, 8u, VM_MMU_ACC_READ, "READ64", &pa_base)) {
        if (!find_mmio(vm, pa_base)) {
            if (!in_ram(vm, pa_base, 8u)) {
                panic(panic_format("READ64 out of bounds: 0x%08x", pa_base), vm);
                return 0u;
            }
            return load_le64(&vm->memory[pa_base]);
        }
    }
    uint64_t lo = vm_read32_cpu(vm, cpu, addr);
    uint64_t hi = vm_read32_cpu(vm, cpu, addr + 4);
    return lo | (hi << 32);
}

uint64_t vm_read64(VM *vm, vm_addr_t addr) {
    return vm_read64_cpu(vm, vm_current_cpu(vm), addr);
}

uint32_t vm_atomic_load32_acquire_cpu(VM *vm, VCPU *cpu, vm_addr_t addr) {
    _Atomic uint32_t *ptr = atomic32_ptr_from_va_or_panic(
        vm, cpu, addr, VM_MMU_ACC_READ, "LDAR", NULL);
    if (!ptr) {
        return 0;
    }
    return atomic_load_explicit(ptr, memory_order_acquire);
}

void vm_atomic_store32_release_cpu(VM *vm, VCPU *cpu, vm_addr_t addr, uint32_t value) {
    uint32_t pa;
    _Atomic uint32_t *ptr = atomic32_ptr_from_va_or_panic(
        vm, cpu, addr, VM_MMU_ACC_WRITE, "STLR", &pa);
    if (!ptr) {
        return;
    }
    atomic_store_explicit(ptr, value, memory_order_release);
    vm_ram_mark_written(vm, pa, sizeof(value));
}

uint32_t vm_atomic_exchange32_seqcst_cpu(VM *vm, VCPU *cpu, vm_addr_t addr, uint32_t value) {
    uint32_t pa;
    uint32_t observed;
    _Atomic uint32_t *ptr = atomic32_ptr_from_va_or_panic(
        vm, cpu, addr, VM_MMU_ACC_READ | VM_MMU_ACC_WRITE, "XCHG", &pa);
    if (!ptr) {
        return 0;
    }
    observed = atomic_exchange_explicit(ptr, value, memory_order_seq_cst);
    vm_ram_mark_written(vm, pa, sizeof(value));
    return observed;
}

uint32_t vm_atomic_fetch_add32_seqcst_cpu(VM *vm, VCPU *cpu, vm_addr_t addr, uint32_t value) {
    uint32_t pa;
    uint32_t observed;
    _Atomic uint32_t *ptr = atomic32_ptr_from_va_or_panic(
        vm, cpu, addr, VM_MMU_ACC_READ | VM_MMU_ACC_WRITE, "XADD", &pa);
    if (!ptr) {
        return 0;
    }
    observed = atomic_fetch_add_explicit(ptr, value, memory_order_seq_cst);
    vm_ram_mark_written(vm, pa, sizeof(value));
    return observed;
}

uint32_t vm_atomic_compare_exchange32_seqcst_cpu(VM *vm,
                                             VCPU *cpu,
                                             vm_addr_t addr,
                                             uint32_t expected,
                                             uint32_t desired,
                                             int *success) {
    uint32_t pa;
    _Atomic uint32_t *ptr = atomic32_ptr_from_va_or_panic(
        vm, cpu, addr, VM_MMU_ACC_READ | VM_MMU_ACC_WRITE, "CAS", &pa);
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
    if (ok) {
        vm_ram_mark_written(vm, pa, sizeof(desired));
    }
    return observed;
}

uint32_t vm_atomic_load32_acquire(VM *vm, vm_addr_t addr) {
    return vm_atomic_load32_acquire_cpu(vm, vm_current_cpu(vm), addr);
}

void vm_atomic_store32_release(VM *vm, vm_addr_t addr, uint32_t value) {
    vm_atomic_store32_release_cpu(vm, vm_current_cpu(vm), addr, value);
}

uint32_t vm_atomic_exchange32_seqcst(VM *vm, vm_addr_t addr, uint32_t value) {
    return vm_atomic_exchange32_seqcst_cpu(vm, vm_current_cpu(vm), addr, value);
}

uint32_t vm_atomic_fetch_add32_seqcst(VM *vm, vm_addr_t addr, uint32_t value) {
    return vm_atomic_fetch_add32_seqcst_cpu(vm, vm_current_cpu(vm), addr, value);
}

uint32_t vm_atomic_compare_exchange32_seqcst(VM *vm,
                                             vm_addr_t addr,
                                             uint32_t expected,
                                             uint32_t desired,
                                             int *success) {
    return vm_atomic_compare_exchange32_seqcst_cpu(vm, vm_current_cpu(vm), addr,
                                                   expected, desired, success);
}

void vm_write8_cpu(VM *vm, VCPU *cpu, vm_addr_t addr, uint8_t value) {
    uint32_t pa;
    size_t fb_index = 0;
    if (!vm_translate_or_panic_cpu(vm, cpu, addr, VM_MMU_ACC_WRITE, "WRITE8", &pa)) {
        return;
    }
    if (fb_byte_index(vm, pa, &fb_index)) {
        const size_t row = vm_fb_row_from_byte_index(fb_index);
        vm_fb_row_lock(vm, row);
        ((uint8_t *) vm->fb)[fb_index] = value;
        vm_fb_mark_row_dirty(vm, row);
        vm_fb_row_unlock(vm, row);
        return;
    }

    if (!in_ram(vm, pa, 1)) {
        panic(panic_format("WRITE8 out of bounds: 0x%08x", pa), vm);
        return;
    }

    vm->memory[pa] = value;
    vm_ram_mark_written(vm, pa, sizeof(value));
}

void vm_write8(VM *vm, vm_addr_t addr, uint8_t value) {
    vm_write8_cpu(vm, vm_current_cpu(vm), addr, value);
}

void vm_write32_cpu(VM *vm, VCPU *cpu, vm_addr_t addr, uint32_t value) {
    uint32_t pa_base;
    uint32_t pa[4];
#ifdef VM_MEMCHECK
    memcheck_align(vm, addr, 4, "WRITE32");
#endif
    if (vm_translate_contiguous_span_or_panic_cpu(vm, cpu, addr, 4u, VM_MMU_ACC_WRITE, "WRITE32", &pa_base)) {
        size_t fb_index = 0u;
        if (fb_byte_index(vm, pa_base, &fb_index)) {
            vm_fb_write32_locked(vm, fb_index, value);
            return;
        }
        MMIO_Device *dev = find_mmio(vm, pa_base);
        if (dev && dev->write32) {
            vm_shared_lock(vm);
            dev->write32(vm, pa_base, value);
            vm_shared_unlock(vm);
            return;
        }
        if (!in_ram(vm, pa_base, 4u)) {
            panic(panic_format("WRITE32 out of bounds: 0x%08x", pa_base), vm);
            return;
        }
        store_le32(&vm->memory[pa_base], value);
        vm_ram_mark_written(vm, pa_base, sizeof(value));
        return;
    }
    if (!vm_translate_span_or_panic_cpu(vm, cpu, addr, 4u, VM_MMU_ACC_WRITE, "WRITE32", pa)) {
        return;
    }

    if (vm_span_is_contiguous(pa, 4u)) {
        size_t fb_index = 0u;
        if (fb_byte_index(vm, pa[0], &fb_index)) {
            vm_fb_write32_locked(vm, fb_index, value);
            return;
        }
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
        vm_ram_mark_written(vm, pa[0], sizeof(value));
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
        vm_ram_mark_written(vm, pa[i], 1u);
    }
}

void vm_write32(VM *vm, vm_addr_t addr, uint32_t value) {
    vm_write32_cpu(vm, vm_current_cpu(vm), addr, value);
}

void vm_write64_cpu(VM *vm, VCPU *cpu, vm_addr_t addr, uint64_t value) {
    uint32_t pa_base;
#ifdef VM_MEMCHECK
    memcheck_align(vm, addr, 8, "WRITE64");
#endif
    if (vm_translate_contiguous_span_or_panic_cpu(vm, cpu, addr, 8u, VM_MMU_ACC_WRITE, "WRITE64", &pa_base)) {
        if (!find_mmio(vm, pa_base)) {
            if (!in_ram(vm, pa_base, 8u)) {
                panic(panic_format("WRITE64 out of bounds: 0x%08x", pa_base), vm);
                return;
            }
            store_le64(&vm->memory[pa_base], value);
            vm_ram_mark_written(vm, pa_base, sizeof(value));
            return;
        }
    }
    vm_write32_cpu(vm, cpu, addr, (uint32_t)(value & 0xFFFFFFFFu));
    vm_write32_cpu(vm, cpu, addr + 4u, (uint32_t)((value >> 32) & 0xFFFFFFFFu));
}

void vm_write64(VM *vm, vm_addr_t addr, uint64_t value) {
    vm_write64_cpu(vm, vm_current_cpu(vm), addr, value);
}

uint32_t vm_fetch64_exec_cpu_ex(VM *vm,
                                VCPU *cpu,
                                vm_addr_t addr,
                                uint64_t *out_inst,
                                uint32_t *host_pa_out) {
    uint32_t pa_base;
    uint32_t pa[8];
    uint64_t inst = 0u;
    if (host_pa_out) {
        *host_pa_out = UINT32_MAX;
    }
    if (!out_inst) {
        return 0u;
    }

    if (vm_translate_contiguous_span_or_panic_cpu(vm, cpu, addr, 8u, VM_MMU_ACC_EXEC, "IFETCH", &pa_base)) {
        if (find_mmio(vm, pa_base)) {
            panic(panic_format("IFETCH from MMIO: 0x%08x", addr), vm);
            return 0u;
        }
        if (!in_ram(vm, pa_base, 8u)) {
            panic(panic_format("IFETCH out of bounds: 0x%08x", pa_base), vm);
            return 0u;
        }
        memcpy(&inst, &vm->memory[pa_base], sizeof(inst));
        *out_inst = inst;
        if (host_pa_out) {
            *host_pa_out = pa_base;
        }
        return 1u;
    }
    if (!vm_translate_span_or_panic_cpu(vm, cpu, addr, 8u, VM_MMU_ACC_EXEC, "IFETCH", pa)) {
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
        if (host_pa_out) {
            *host_pa_out = pa[0];
        }
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

uint32_t vm_fetch64_exec_cpu(VM *vm, VCPU *cpu, vm_addr_t addr, uint64_t *out_inst) {
    return vm_fetch64_exec_cpu_ex(vm, cpu, addr, out_inst, NULL);
}

uint32_t vm_fetch64_exec(VM *vm, vm_addr_t addr, uint64_t *out_inst) {
    return vm_fetch64_exec_cpu(vm, vm_current_cpu(vm), addr, out_inst);
}
