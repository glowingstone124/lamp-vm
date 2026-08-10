#ifndef VM_ENGINES_ENGINE_INTERNAL_H
#define VM_ENGINES_ENGINE_INTERNAL_H

#include <stdatomic.h>
#include <stdint.h>

#include "engine.h"
#include "../flags.h"
#include "../panic.h"

void vm_engine_execute_decoded(VM *vm,
                               VCPU *cpu,
                               const VM_DecodedOp *decoded);

static inline int32_t vm_engine_add_wrap32(int32_t a, int32_t b) {
    return (int32_t)((uint32_t)a + (uint32_t)b);
}

static inline int32_t vm_engine_sub_wrap32(int32_t a, int32_t b) {
    return (int32_t)((uint32_t)a - (uint32_t)b);
}

static inline int32_t vm_engine_mul_wrap32(int32_t a, int32_t b) {
    return (int32_t)((uint32_t)a * (uint32_t)b);
}

static inline void vm_engine_update_zf_sf(VM *vm,
                                          int32_t result,
                                          VCPU *cpu) {
    (void)vm;
    if (!cpu) {
        return;
    }
    if (result == 0) {
        cpu->flags |= FLAG_ZF;
    } else {
        cpu->flags &= ~FLAG_ZF;
    }
    if (result < 0) {
        cpu->flags |= FLAG_SF;
    } else {
        cpu->flags &= ~FLAG_SF;
    }
}

static inline void vm_engine_update_add_flags(VM *vm,
                                               int32_t a,
                                               int32_t b,
                                               int32_t result,
                                               VCPU *cpu) {
    if (!cpu) {
        return;
    }
    if ((uint32_t)a + (uint32_t)b < (uint32_t)a) {
        cpu->flags |= FLAG_CF;
    } else {
        cpu->flags &= ~FLAG_CF;
    }
    if (((~((uint32_t)a ^ (uint32_t)b)) &
         ((uint32_t)a ^ (uint32_t)result) & 0x80000000u) != 0u) {
        cpu->flags |= FLAG_OF;
    } else {
        cpu->flags &= ~FLAG_OF;
    }
    vm_engine_update_zf_sf(vm, result, cpu);
}

static inline void vm_engine_update_sub_flags(VM *vm,
                                               int32_t a,
                                               int32_t b,
                                               int32_t result,
                                               VCPU *cpu) {
    if (!cpu) {
        return;
    }
    if ((uint32_t)a < (uint32_t)b) {
        cpu->flags |= FLAG_CF;
    } else {
        cpu->flags &= ~FLAG_CF;
    }
    if ((((uint32_t)a ^ (uint32_t)b) &
         ((uint32_t)a ^ (uint32_t)result) & 0x80000000u) != 0u) {
        cpu->flags |= FLAG_OF;
    } else {
        cpu->flags &= ~FLAG_OF;
    }
    vm_engine_update_zf_sf(vm, result, cpu);
}

static inline void vm_engine_clear_cf_of(VM *vm, VCPU *cpu) {
    (void)vm;
    if (cpu) {
        cpu->flags &= ~(FLAG_CF | FLAG_OF);
    }
}

static inline void vm_engine_update_logic_flags(VM *vm,
                                                 int32_t result,
                                                 VCPU *cpu) {
    vm_engine_clear_cf_of(vm, cpu);
    vm_engine_update_zf_sf(vm, result, cpu);
}

static inline void vm_engine_set_cas_flags(VM *vm,
                                            int success,
                                            VCPU *cpu) {
    (void)vm;
    if (!cpu) {
        return;
    }
    cpu->flags &= ~(FLAG_ZF | FLAG_SF | FLAG_CF | FLAG_OF);
    if (success) {
        cpu->flags |= FLAG_ZF;
    }
}

static inline void vm_engine_ensure_atomic_aligned_or_panic(
    VM *vm,
    vm_addr_t addr,
    const char *op_name) {
    if ((addr & 0x3u) != 0u) {
        panic(panic_format("%s unaligned address: 0x%08x", op_name, addr), vm);
    }
}

static inline void vm_engine_ensure_halfword_aligned_or_panic(
    VM *vm,
    vm_addr_t addr,
    const char *op_name) {
    if ((addr & 0x1u) != 0u) {
        panic(panic_format("%s unaligned address: 0x%08x", op_name, addr), vm);
    }
}

static inline vm_addr_t vm_engine_rel_target_from_last_ip(
    const VCPU *cpu,
    int32_t imm) {
    const int64_t base = (int64_t)(vm_addr_t)cpu->last_ip;
    const int64_t target = base + (int64_t)imm;
    return (vm_addr_t)target;
}

static inline uint32_t vm_engine_rotl32(uint32_t value, uint32_t shift) {
    shift &= 31u;
    if (shift == 0u) {
        return value;
    }
    return (value << shift) | (value >> (32u - shift));
}

static inline uint32_t vm_engine_rotr32(uint32_t value, uint32_t shift) {
    shift &= 31u;
    if (shift == 0u) {
        return value;
    }
    return (value >> shift) | (value << (32u - shift));
}

static inline void vm_engine_host_cpu_relax(void) {
#if defined(__aarch64__)
    __asm__ volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("pause" ::: "memory");
#else
    atomic_signal_fence(memory_order_seq_cst);
#endif
}

#endif
