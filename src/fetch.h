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

    *op = entry->op;
    *rd = entry->rd;
    *rs1 = entry->rs1;
    *rs2 = entry->rs2;
    *imm = entry->imm;
}

#define FETCH64(vm, op, rd, rs1, rs2, imm)                                                         \
    do {                                                                                           \
        VCPU *cpu = vm_current_cpu((vm));                                                          \
        if (!cpu) {                                                                                \
            panic("No active CPU context\n", (vm));                                                \
            return;                                                                                \
        }                                                                                          \
        const vm_addr_t ip = (vm_addr_t)cpu->ip;                                                   \
        cpu->last_ip = ip;                                                                         \
        uint64_t inst = 0;                                                                         \
        if (!vm_fetch64_exec((vm), ip, &inst)) {                                                   \
            return;                                                                                \
        }                                                                                          \
        cpu->ip = (size_t)(ip + 8u);                                                               \
        vm_decode_inst_cached(cpu, ip, inst, &(op), &(rd), &(rs1), &(rs2), &(imm));                \
    } while (0)

#endif // VM_FETCH_H
