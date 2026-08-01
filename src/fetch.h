//
// Created by Max Wang on 2025/12/28.
//
#ifndef VM_FETCH_H
#define VM_FETCH_H

#include <string.h>

#include "memory.h"
#include "panic.h"
#include "vm.h"

static inline void vm_decode_inst_cached(VCPU *cpu,
                                         vm_addr_t ip,
                                         uint64_t inst,
                                         uint8_t *op,
                                         uint8_t *rd,
                                         uint8_t *rs1,
                                         uint8_t *rs2,
                                         int32_t *imm) {
    VM_DecodeCacheEntry *entry = &cpu->decode_cache[(ip >> 3u) & (VM_DECODE_CACHE_ENTRIES - 1u)];
    if (entry->valid != 0u && entry->ip == ip && entry->inst == inst) {
        *op = entry->op;
        *rd = entry->rd;
        *rs1 = entry->rs1;
        *rs2 = entry->rs2;
        *imm = entry->imm;
        return;
    }

    entry->valid = 1u;
    entry->ip = ip;
    entry->inst = inst;
    entry->op = (uint8_t)((inst >> 56) & 0xFFu);
    entry->rd = (uint8_t)((inst >> 48) & 0xFFu);
    entry->rs1 = (uint8_t)((inst >> 40) & 0xFFu);
    entry->rs2 = (uint8_t)((inst >> 32) & 0xFFu);
    entry->imm = (int32_t)(inst & 0xFFFFFFFFu);
    entry->translated = 0u;

    *op = entry->op;
    *rd = entry->rd;
    *rs1 = entry->rs1;
    *rs2 = entry->rs2;
    *imm = entry->imm;
}

static inline int vm_fetch64_decode_translated(VM *vm,
                                                VCPU *cpu,
                                                uint8_t *op,
                                                uint8_t *rd,
                                                uint8_t *rs1,
                                                uint8_t *rs2,
                                                int32_t *imm) {
    vm_addr_t ip;
    uint64_t inst;
    uint64_t mmu_epoch_before;
    uint64_t mmu_epoch_after;
    uint32_t mmio_epoch_before;
    uint32_t mmio_epoch_after;
    uint32_t host_pa = UINT32_MAX;
    VM_DecodeCacheEntry *entry;

    if (!cpu) {
        panic("No active CPU context\n", vm);
        return 0;
    }

    ip = (vm_addr_t)cpu->ip;
    entry = &cpu->decode_cache[(ip >> 3u) &
                               (VM_DECODE_CACHE_ENTRIES - 1u)];
    mmu_epoch_before = atomic_load_explicit(&cpu->mmu_epoch,
                                             memory_order_acquire);
    mmio_epoch_before = (uint32_t)atomic_load_explicit(&vm->mmio_epoch,
                                                       memory_order_acquire);

    if (entry->valid != 0u && entry->translated != 0u &&
        entry->ip == ip && entry->mmu_epoch == mmu_epoch_before &&
        entry->mmio_epoch == mmio_epoch_before &&
        entry->host_pa < vm->memory_size &&
        sizeof(inst) <= vm->memory_size - entry->host_pa) {
        const uint32_t cached_host_pa = entry->host_pa;
        memcpy(&inst, &vm->memory[cached_host_pa], sizeof(inst));
        vm_decode_inst_cached(cpu, ip, inst, op, rd, rs1, rs2, imm);
        entry = &cpu->decode_cache[(ip >> 3u) &
                                   (VM_DECODE_CACHE_ENTRIES - 1u)];
        entry->host_pa = cached_host_pa;
        entry->mmu_epoch = mmu_epoch_before;
        entry->mmio_epoch = mmio_epoch_before;
        entry->translated = 1u;
        cpu->last_ip = ip;
        cpu->ip = (size_t)(ip + 8u);
        return 1;
    }

    if (!vm_fetch64_exec_cpu_ex(vm, cpu, ip, &inst, &host_pa)) {
        return 0;
    }
    mmu_epoch_after = atomic_load_explicit(&cpu->mmu_epoch,
                                            memory_order_acquire);
    mmio_epoch_after = (uint32_t)atomic_load_explicit(&vm->mmio_epoch,
                                                      memory_order_acquire);
    cpu->last_ip = ip;
    cpu->ip = (size_t)(ip + 8u);
    vm_decode_inst_cached(cpu, ip, inst, op, rd, rs1, rs2, imm);

    entry = &cpu->decode_cache[(ip >> 3u) &
                               (VM_DECODE_CACHE_ENTRIES - 1u)];
    if (host_pa != UINT32_MAX &&
        mmu_epoch_before == mmu_epoch_after &&
        mmio_epoch_before == mmio_epoch_after) {
        entry->host_pa = host_pa;
        entry->mmu_epoch = mmu_epoch_after;
        entry->mmio_epoch = mmio_epoch_after;
        entry->translated = 1u;
    }
    return 1;
}

#endif // VM_FETCH_H
