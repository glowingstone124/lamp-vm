//
// Created by Max Wang on 2026/1/26.
//

#ifndef VM_FLOAT_H
#define VM_FLOAT_H
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "flags.h"
#include "vm.h"

static inline float reg_as_f32(int32_t r) {
    float f;
    uint32_t u = (uint32_t)r;
    memcpy(&f, &u, sizeof(f));
    return f;
}

static inline int32_t f32_as_reg(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return (int32_t)u;
}

static inline bool f32_is_nan(float f) {
    return isnan(f);
}
static inline void clear_all_flags(VM *vm) {
    VCPU *cpu = vm_current_cpu(vm);
    if (!cpu)
        return;
    cpu->flags &= ~(FLAG_ZF | FLAG_SF | FLAG_CF | FLAG_OF);
}
static inline void update_fcmp_flags(VM *vm, float a , float b) {
    VCPU *cpu = vm_current_cpu(vm);
    if (!cpu)
        return;
    clear_all_flags(vm);

    if (f32_is_nan(a) || f32_is_nan(b)) {
        cpu->flags |= FLAG_OF;
        return;
    }
    if (a == b) cpu->flags |= FLAG_ZF;
    if (a < b) cpu->flags |= FLAG_SF;
    if (a > b) cpu->flags |= FLAG_CF;
}
#endif //VM_FLOAT_H
