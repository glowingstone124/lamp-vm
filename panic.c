#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "vm.h"
//
// Created by Max Wang on 2025/12/29.
//
const char *panic_format(const char *fmt, ...) {
    static char buffer[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    return buffer;
}

static uint64_t calculate_cycles(const VM *vm) {
    uint64_t cycles = 0;
    for (int i = 0; i < vm->smp_cores; i++) {
        cycles += atomic_load_explicit(&vm->cpus[i].execution_times, memory_order_relaxed);
    }
    return cycles;
}

void panic(const char *msg, VM *vm) {
    VCPU *cpu = vm_current_cpu(vm);
    uint64_t cycles = vm ? calculate_cycles(vm) : 0;
    size_t ip_now = cpu ? cpu->ip : 0;
    size_t last_ip = cpu ? cpu->last_ip : 0;
    printf("VM panic detected. %s\n @ %lu clock cycle, IP = %lu",
           msg, (unsigned long)cycles, (unsigned long)ip_now);
    if (vm && vm->memory) {
        const size_t window = 6;
        size_t start = last_ip >= window * 8 ? last_ip - window * 8 : 0;
        size_t end = last_ip + 8 <= vm->memory_size ? last_ip + 8 : vm->memory_size;
        printf("\nLast inst window:\n");
        for (size_t ip = start; ip + 8 <= end; ip += 8) {
            const uint8_t *p = vm->memory + ip;
            uint64_t inst = (uint64_t)p[0]
                | ((uint64_t)p[1] << 8)
                | ((uint64_t)p[2] << 16)
                | ((uint64_t)p[3] << 24)
                | ((uint64_t)p[4] << 32)
                | ((uint64_t)p[5] << 40)
                | ((uint64_t)p[6] << 48)
                | ((uint64_t)p[7] << 56);
            uint8_t op = (inst >> 56) & 0xFF;
            uint8_t rd = (inst >> 48) & 0xFF;
            uint8_t rs1 = (inst >> 40) & 0xFF;
            uint8_t rs2 = (inst >> 32) & 0xFF;
            int32_t imm = (int32_t)(inst & 0xFFFFFFFF);
            printf("  0x%zx: op=%u rd=%u rs1=%u rs2=%u imm=%d (0x%08x)%s\n",
                   ip, op, rd, rs1, rs2, imm, (uint32_t)imm,
                   ip == last_ip ? "  <--" : "");
        }
    }
    printf("Creating VM dump...");
    vm_dump(vm, DUMP_MEM_SEEK_LEN);
    if (vm)
        atomic_set_vm_panic(vm, 1);;
    exit(1);
}
