#include "arm64_internal.h"

#if defined(__aarch64__)

void a64_emit(A64Emitter *emitter, uint32_t word) {
    if (!emitter || emitter->failed) {
        return;
    }
    if (emitter->count >= sizeof(emitter->words) / sizeof(emitter->words[0])) {
        emitter->failed = 1;
        return;
    }
    emitter->words[emitter->count++] = word;
}

void a64_emit_stp_pre(A64Emitter *emitter,
                      unsigned rt,
                      unsigned rt2,
                      unsigned rn,
                      int byte_offset) {
    const int scaled = byte_offset / 8;
    if ((byte_offset % 8) != 0 || scaled < -64 || scaled > 63 ||
        rt > 31u || rt2 > 31u || rn > 31u) {
        emitter->failed = 1;
        return;
    }
    a64_emit(emitter,
             0xA9800000u |
             (((uint32_t) scaled & 0x7Fu) << 15u) |
             ((uint32_t) rt2 << 10u) |
             ((uint32_t) rn << 5u) |
             (uint32_t) rt);
}

void a64_emit_ldp_post(A64Emitter *emitter,
                       unsigned rt,
                       unsigned rt2,
                       unsigned rn,
                       int byte_offset) {
    const int scaled = byte_offset / 8;
    if ((byte_offset % 8) != 0 || scaled < -64 || scaled > 63 ||
        rt > 31u || rt2 > 31u || rn > 31u) {
        emitter->failed = 1;
        return;
    }
    a64_emit(emitter,
             0xA8C00000u |
             (((uint32_t) scaled & 0x7Fu) << 15u) |
             ((uint32_t) rt2 << 10u) |
             ((uint32_t) rn << 5u) |
             (uint32_t) rt);
}

void a64_emit_mov_x(A64Emitter *emitter,
                    unsigned rd,
                    unsigned rm) {
    if (rd > 30u || rm > 30u) {
        emitter->failed = 1;
        return;
    }
    a64_emit(emitter,
             0xAA0003E0u | ((uint32_t) rm << 16u) | (uint32_t) rd);
}

void a64_emit_mov_w(A64Emitter *emitter,
                    unsigned rd,
                    unsigned rm) {
    if (rd > 30u || rm > 30u) {
        emitter->failed = 1;
        return;
    }
    a64_emit(emitter,
             0x2A0003E0u | ((uint32_t) rm << 16u) | (uint32_t) rd);
}

void a64_emit_mov_w_imm(A64Emitter *emitter,
                        unsigned rd,
                        uint32_t value) {
    if (rd > 30u) {
        emitter->failed = 1;
        return;
    }
    a64_emit(emitter,
             0x52800000u | ((value & 0xFFFFu) << 5u) | (uint32_t) rd);
    if ((value >> 16u) != 0u) {
        a64_emit(emitter,
                 0x72A00000u | (((value >> 16u) & 0xFFFFu) << 5u) |
                 (uint32_t) rd);
    }
}

void a64_emit_mov_x_imm(A64Emitter *emitter,
                        unsigned rd,
                        uint64_t value) {
    if (rd > 30u) {
        emitter->failed = 1;
        return;
    }
    a64_emit(emitter,
             0xD2800000u | ((uint32_t) (value & 0xFFFFu) << 5u) |
             (uint32_t) rd);
    if (((value >> 16u) & 0xFFFFu) != 0u) {
        a64_emit(emitter,
                 0xF2A00000u |
                 ((uint32_t) ((value >> 16u) & 0xFFFFu) << 5u) |
                 (uint32_t) rd);
    }
    if (((value >> 32u) & 0xFFFFu) != 0u) {
        a64_emit(emitter,
                 0xF2C00000u |
                 ((uint32_t) ((value >> 32u) & 0xFFFFu) << 5u) |
                 (uint32_t) rd);
    }
    if (((value >> 48u) & 0xFFFFu) != 0u) {
        a64_emit(emitter,
                 0xF2E00000u |
                 ((uint32_t) ((value >> 48u) & 0xFFFFu) << 5u) |
                 (uint32_t) rd);
    }
}

void a64_emit_ldr_w(A64Emitter *emitter,
                    unsigned rt,
                    unsigned rn,
                    size_t byte_offset) {
    if (rt > 30u || rn > 30u || (byte_offset & 3u) != 0u ||
        byte_offset / 4u > 0xFFFu) {
        emitter->failed = 1;
        return;
    }
    a64_emit(emitter,
             0xB9400000u |
             ((uint32_t) (byte_offset / 4u) << 10u) |
             ((uint32_t) rn << 5u) |
             (uint32_t) rt);
}

void a64_emit_str_w(A64Emitter *emitter,
                    unsigned rt,
                    unsigned rn,
                    size_t byte_offset) {
    if (rt > 30u || rn > 30u || (byte_offset & 3u) != 0u ||
        byte_offset / 4u > 0xFFFu) {
        emitter->failed = 1;
        return;
    }
    a64_emit(emitter,
             0xB9000000u |
             ((uint32_t) (byte_offset / 4u) << 10u) |
             ((uint32_t) rn << 5u) |
             (uint32_t) rt);
}

void a64_emit_str_x(A64Emitter *emitter,
                    unsigned rt,
                    unsigned rn,
                    size_t byte_offset) {
    if (rt > 30u || rn > 30u || (byte_offset & 7u) != 0u ||
        byte_offset / 8u > 0xFFFu) {
        emitter->failed = 1;
        return;
    }
    a64_emit(emitter,
             0xF9000000u |
             ((uint32_t) (byte_offset / 8u) << 10u) |
             ((uint32_t) rn << 5u) |
             (uint32_t) rt);
}

void a64_emit_add_w(A64Emitter *emitter,
                    unsigned rd,
                    unsigned rn,
                    unsigned rm) {
    a64_emit(emitter,
             0x0B000000u | ((uint32_t) rm << 16u) |
             ((uint32_t) rn << 5u) | (uint32_t) rd);
}

void a64_emit_add_w_imm(A64Emitter *emitter,
                        unsigned rd,
                        unsigned rn,
                        uint32_t immediate) {
    if (rd > 30u || rn > 30u || immediate > 0xFFFu) {
        emitter->failed = 1;
        return;
    }
    a64_emit(emitter,
             0x11000000u | (immediate << 10u) |
             ((uint32_t) rn << 5u) | (uint32_t) rd);
}

void a64_emit_cmp_w_imm(A64Emitter *emitter,
                        unsigned rn,
                        uint32_t immediate) {
    if (rn > 30u || immediate > 0xFFFu) {
        emitter->failed = 1;
        return;
    }
    a64_emit(emitter,
             0x7100001Fu | (immediate << 10u) | ((uint32_t) rn << 5u));
}

void a64_emit_adds_w(A64Emitter *emitter,
                     unsigned rd,
                     unsigned rn,
                     unsigned rm) {
    a64_emit(emitter,
             0x2B000000u | ((uint32_t) rm << 16u) |
             ((uint32_t) rn << 5u) | (uint32_t) rd);
}

void a64_emit_subs_w(A64Emitter *emitter,
                     unsigned rd,
                     unsigned rn,
                     unsigned rm) {
    a64_emit(emitter,
             0x6B000000u | ((uint32_t) rm << 16u) |
             ((uint32_t) rn << 5u) | (uint32_t) rd);
}

void a64_emit_mrs_nzcv(A64Emitter *emitter, unsigned rt) {
    if (rt > 30u) {
        emitter->failed = 1;
        return;
    }
    a64_emit(emitter, 0xD53B4200u | (uint32_t) rt);
}

void a64_emit_lsr_w(A64Emitter *emitter,
                    unsigned rd,
                    unsigned rn,
                    unsigned shift) {
    if (rd > 30u || rn > 30u || shift > 31u) {
        emitter->failed = 1;
        return;
    }
    a64_emit(emitter,
             0x53000000u | ((uint32_t) shift << 16u) | (31u << 10u) |
             ((uint32_t) rn << 5u) | (uint32_t) rd);
}

void a64_emit_lsl_w(A64Emitter *emitter,
                    unsigned rd,
                    unsigned rn,
                    unsigned shift) {
    unsigned immr;
    unsigned imms;
    if (rd > 30u || rn > 30u || shift > 31u) {
        emitter->failed = 1;
        return;
    }
    immr = (32u - shift) & 31u;
    imms = 31u - shift;
    a64_emit(emitter,
             0x53000000u | ((uint32_t) immr << 16u) |
             ((uint32_t) imms << 10u) |
             ((uint32_t) rn << 5u) | (uint32_t) rd);
}

void a64_emit_and_w(A64Emitter *emitter,
                    unsigned rd,
                    unsigned rn,
                    unsigned rm) {
    a64_emit(emitter,
             0x0A000000u | ((uint32_t) rm << 16u) |
             ((uint32_t) rn << 5u) | (uint32_t) rd);
}

void a64_emit_orr_w(A64Emitter *emitter,
                    unsigned rd,
                    unsigned rn,
                    unsigned rm) {
    a64_emit(emitter,
             0x2A000000u | ((uint32_t) rm << 16u) |
             ((uint32_t) rn << 5u) | (uint32_t) rd);
}

void a64_emit_eor_w(A64Emitter *emitter,
                    unsigned rd,
                    unsigned rn,
                    unsigned rm) {
    a64_emit(emitter,
             0x4A000000u | ((uint32_t) rm << 16u) |
             ((uint32_t) rn << 5u) | (uint32_t) rd);
}

void a64_emit_mvn_w(A64Emitter *emitter,
                    unsigned rd,
                    unsigned rm) {
    a64_emit(emitter,
             0x2A2003E0u | ((uint32_t)rm << 16u) | (uint32_t)rd);
}

void a64_emit_lslv_w(A64Emitter *emitter,
                     unsigned rd,
                     unsigned rn,
                     unsigned rm) {
    a64_emit(emitter,
             0x1AC02000u | ((uint32_t)rm << 16u) |
             ((uint32_t)rn << 5u) | (uint32_t)rd);
}

void a64_emit_lsrv_w(A64Emitter *emitter,
                     unsigned rd,
                     unsigned rn,
                     unsigned rm) {
    a64_emit(emitter,
             0x1AC02400u | ((uint32_t)rm << 16u) |
             ((uint32_t)rn << 5u) | (uint32_t)rd);
}

void a64_emit_asrv_w(A64Emitter *emitter,
                     unsigned rd,
                     unsigned rn,
                     unsigned rm) {
    a64_emit(emitter,
             0x1AC02800u | ((uint32_t)rm << 16u) |
             ((uint32_t)rn << 5u) | (uint32_t)rd);
}

void a64_emit_rorv_w(A64Emitter *emitter,
                     unsigned rd,
                     unsigned rn,
                     unsigned rm) {
    a64_emit(emitter,
             0x1AC02C00u | ((uint32_t)rm << 16u) |
             ((uint32_t)rn << 5u) | (uint32_t)rd);
}

void a64_emit_neg_w(A64Emitter *emitter,
                    unsigned rd,
                    unsigned rm) {
    a64_emit(emitter,
             0x4B0003E0u | ((uint32_t)rm << 16u) | (uint32_t)rd);
}

void a64_emit_mul_w(A64Emitter *emitter,
                    unsigned rd,
                    unsigned rn,
                    unsigned rm) {
    a64_emit(emitter,
             0x1B007C00u | ((uint32_t) rm << 16u) |
             ((uint32_t) rn << 5u) | (uint32_t) rd);
}

void a64_emit_blr(A64Emitter *emitter, unsigned rn) {
    if (rn > 30u) {
        emitter->failed = 1;
        return;
    }
    a64_emit(emitter, 0xD63F0000u | ((uint32_t) rn << 5u));
}

void a64_emit_function_address(A64Emitter *emitter,
                               unsigned rd,
                               const void *representation,
                               size_t representation_size) {
    uint64_t address = 0u;
    if (!representation || representation_size > sizeof(address)) {
        emitter->failed = 1;
        return;
    }
    memcpy(&address, representation, representation_size);
    a64_emit_mov_x_imm(emitter, rd, address);
}

void a64_emit_prepare_call(A64Emitter *emitter) {
    a64_emit_mov_x(emitter, A64_X0, A64_X19);
    a64_emit_mov_x(emitter, A64_X1, A64_X20);
}

void a64_emit_preserved_guest_flags(A64Emitter *emitter) {
    const uint32_t preserve_mask =
            ~(uint32_t) (FLAG_CF | FLAG_ZF | FLAG_SF | FLAG_OF);
    a64_emit_ldr_w(emitter, A64_X12, A64_X20, offsetof(VCPU, flags));
    a64_emit_mov_w_imm(emitter, A64_X13, preserve_mask);
    a64_emit_and_w(emitter, A64_X12, A64_X12, A64_X13);
}

/* NZCV >> 27 maps N/Z to the guest SF/ZF bit positions directly. */
void a64_emit_logic_flags(A64Emitter *emitter, unsigned result_reg) {
    a64_emit_cmp_w_imm(emitter, result_reg, 0u);
    a64_emit_mrs_nzcv(emitter, A64_X11);
    a64_emit_lsr_w(emitter, A64_X11, A64_X11, 27u);
    a64_emit_preserved_guest_flags(emitter);
    a64_emit_mov_w_imm(emitter, A64_X13, FLAG_ZF | FLAG_SF);
    a64_emit_and_w(emitter, A64_X14, A64_X11, A64_X13);
    a64_emit_orr_w(emitter, A64_X12, A64_X12, A64_X14);
    a64_emit_str_w(emitter, A64_X12, A64_X20, offsetof(VCPU, flags));
}

/*
 * After ADDS/SUBS, NZCV >> 27 is N:4 Z:3 C:2 V:1.  Guest subtraction CF
 * denotes borrow, so it is the inverse of AArch64 C.
 */
void a64_emit_arithmetic_flags(A64Emitter *emitter, int subtract) {
    a64_emit_mrs_nzcv(emitter, A64_X11);
    a64_emit_lsr_w(emitter, A64_X11, A64_X11, 27u);
    a64_emit_preserved_guest_flags(emitter);

    a64_emit_mov_w_imm(emitter, A64_X13, FLAG_ZF | FLAG_SF);
    a64_emit_and_w(emitter, A64_X14, A64_X11, A64_X13);
    a64_emit_orr_w(emitter, A64_X12, A64_X12, A64_X14);

    a64_emit_mov_w_imm(emitter, A64_X13, 4u);
    a64_emit_and_w(emitter, A64_X14, A64_X11, A64_X13);
    a64_emit_lsr_w(emitter, A64_X14, A64_X14, 2u);
    if (subtract) {
        a64_emit_mov_w_imm(emitter, A64_X13, 1u);
        a64_emit_eor_w(emitter, A64_X14, A64_X14, A64_X13);
    }
    a64_emit_orr_w(emitter, A64_X12, A64_X12, A64_X14);

    a64_emit_mov_w_imm(emitter, A64_X13, 2u);
    a64_emit_and_w(emitter, A64_X14, A64_X11, A64_X13);
    a64_emit_lsl_w(emitter, A64_X14, A64_X14, 4u);
    a64_emit_orr_w(emitter, A64_X12, A64_X12, A64_X14);
    a64_emit_str_w(emitter, A64_X12, A64_X20, offsetof(VCPU, flags));
}

size_t a64_reg_offset(unsigned guest_reg) {
    return offsetof(VCPU, regs) + (size_t) guest_reg * sizeof(uint32_t);
}

int a64_guest_reg_valid(unsigned guest_reg) {
    return guest_reg < REG_COUNT;
}

void a64_emit_guest_position(A64Emitter *emitter,
                             const VM_DecodedOp *op) {
    const vm_addr_t next_ip = op->ip + (vm_addr_t) sizeof(uint64_t);
    a64_emit_mov_x_imm(emitter, A64_X8, (uint64_t) op->ip);
    a64_emit_str_x(emitter, A64_X8, A64_X20, offsetof(VCPU, last_ip));
    a64_emit_mov_x_imm(emitter, A64_X8, (uint64_t) next_ip);
    a64_emit_str_x(emitter, A64_X8, A64_X20, offsetof(VCPU, ip));
}

void a64_emit_store_ip(A64Emitter *emitter, vm_addr_t target) {
    a64_emit_mov_x_imm(emitter, A64_X8, (uint64_t) target);
    a64_emit_str_x(emitter, A64_X8, A64_X20, offsetof(VCPU, ip));
}

void a64_emit_memory_address(A64Emitter *emitter,
                             unsigned base_guest_reg,
                             int32_t immediate) {
    if (!a64_guest_reg_valid(base_guest_reg)) {
        emitter->failed = 1;
        return;
    }
    a64_emit_ldr_w(emitter,
                   A64_X8,
                   A64_X20,
                   a64_reg_offset(base_guest_reg));
    a64_emit_mov_w_imm(emitter, A64_X9, (uint32_t) immediate);
    a64_emit_add_w(emitter, A64_X2, A64_X8, A64_X9);
}

#endif /* __aarch64__ */
