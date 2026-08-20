#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../code_memory.h"
#include "../codegen.h"
#include "../../../flags.h"

#if defined(__aarch64__)

enum {
    A64_X0 = 0,
    A64_X1 = 1,
    A64_X2 = 2,
    A64_X3 = 3,
    A64_X4 = 4,
    A64_X8 = 8,
    A64_X9 = 9,
    A64_X10 = 10,
    A64_X11 = 11,
    A64_X12 = 12,
    A64_X13 = 13,
    A64_X14 = 14,
    A64_X16 = 16,
    A64_X19 = 19,
    A64_X20 = 20,
    A64_X21 = 21,
    A64_X22 = 22,
    A64_X29 = 29,
    A64_X30 = 30,
    A64_SP = 31,
};

enum {
    A64_MAX_FIXUPS = VM_JIT_BLOCK_MAX_OPS * 2u
};

typedef enum A64FixupKind {
    A64_FIXUP_B,
    A64_FIXUP_TBZ,
    A64_FIXUP_TBNZ,
    A64_FIXUP_BCOND,
} A64FixupKind;
typedef enum A64FixupTargetKind {
    A64_FIXUP_TARGET_OP,
    A64_FIXUP_TARGET_EPILOGUE,
} A64FixupTargetKind;
typedef struct A64BranchFixup {
    A64FixupKind kind;
    A64FixupTargetKind target_kind;

    size_t word_index;
    uint32_t target_op_index;

    unsigned rt;
    unsigned bit;
    unsigned condition;
} A64BranchFixup;

typedef struct A64Emitter {
    uint32_t words[1024];
    size_t count;

    A64BranchFixup fixups[A64_MAX_FIXUPS];
    size_t fixup_count;

    int failed;
} A64Emitter;


static void a64_emit(A64Emitter *emitter, uint32_t word) {
    if (!emitter || emitter->failed) {
        return;
    }
    if (emitter->count >= sizeof(emitter->words) / sizeof(emitter->words[0])) {
        emitter->failed = 1;
        return;
    }
    emitter->words[emitter->count++] = word;
}

static void a64_emit_stp_pre(A64Emitter *emitter,
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

static void a64_emit_ldp_post(A64Emitter *emitter,
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

static void a64_emit_mov_x(A64Emitter *emitter,
                           unsigned rd,
                           unsigned rm) {
    if (rd > 30u || rm > 30u) {
        emitter->failed = 1;
        return;
    }
    a64_emit(emitter,
             0xAA0003E0u | ((uint32_t) rm << 16u) | (uint32_t) rd);
}

static void a64_emit_mov_w(A64Emitter *emitter,
                           unsigned rd,
                           unsigned rm) {
    if (rd > 30u || rm > 30u) {
        emitter->failed = 1;
        return;
    }
    a64_emit(emitter,
             0x2A0003E0u | ((uint32_t) rm << 16u) | (uint32_t) rd);
}

static void a64_emit_mov_w_imm(A64Emitter *emitter,
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

static void a64_emit_mov_x_imm(A64Emitter *emitter,
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

static void a64_emit_ldr_w(A64Emitter *emitter,
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

static void a64_emit_str_w(A64Emitter *emitter,
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

static void a64_emit_str_x(A64Emitter *emitter,
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

static void a64_emit_add_w(A64Emitter *emitter,
                           unsigned rd,
                           unsigned rn,
                           unsigned rm) {
    a64_emit(emitter,
             0x0B000000u | ((uint32_t) rm << 16u) |
             ((uint32_t) rn << 5u) | (uint32_t) rd);
}

static void a64_emit_add_w_imm(A64Emitter *emitter,
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

static void a64_emit_cmp_w_imm(A64Emitter *emitter,
                               unsigned rn,
                               uint32_t immediate) {
    if (rn > 30u || immediate > 0xFFFu) {
        emitter->failed = 1;
        return;
    }
    a64_emit(emitter,
             0x7100001Fu | (immediate << 10u) | ((uint32_t) rn << 5u));
}

static void a64_emit_adds_w(A64Emitter *emitter,
                            unsigned rd,
                            unsigned rn,
                            unsigned rm) {
    a64_emit(emitter,
             0x2B000000u | ((uint32_t) rm << 16u) |
             ((uint32_t) rn << 5u) | (uint32_t) rd);
}

static void a64_emit_subs_w(A64Emitter *emitter,
                            unsigned rd,
                            unsigned rn,
                            unsigned rm) {
    a64_emit(emitter,
             0x6B000000u | ((uint32_t) rm << 16u) |
             ((uint32_t) rn << 5u) | (uint32_t) rd);
}

static void a64_emit_mrs_nzcv(A64Emitter *emitter, unsigned rt) {
    if (rt > 30u) {
        emitter->failed = 1;
        return;
    }
    a64_emit(emitter, 0xD53B4200u | (uint32_t) rt);
}

static void a64_emit_lsr_w(A64Emitter *emitter,
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

static void a64_emit_lsl_w(A64Emitter *emitter,
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

static void a64_emit_and_w(A64Emitter *emitter,
                           unsigned rd,
                           unsigned rn,
                           unsigned rm) {
    a64_emit(emitter,
             0x0A000000u | ((uint32_t) rm << 16u) |
             ((uint32_t) rn << 5u) | (uint32_t) rd);
}

static void a64_emit_orr_w(A64Emitter *emitter,
                           unsigned rd,
                           unsigned rn,
                           unsigned rm) {
    a64_emit(emitter,
             0x2A000000u | ((uint32_t) rm << 16u) |
             ((uint32_t) rn << 5u) | (uint32_t) rd);
}

static void a64_emit_eor_w(A64Emitter *emitter,
                           unsigned rd,
                           unsigned rn,
                           unsigned rm) {
    a64_emit(emitter,
             0x4A000000u | ((uint32_t) rm << 16u) |
             ((uint32_t) rn << 5u) | (uint32_t) rd);
}

static void a64_emit_mvn_w(A64Emitter *emitter,
                           unsigned rd,
                           unsigned rm) {
    a64_emit(emitter,
             0x2A2003E0u | ((uint32_t)rm << 16u) | (uint32_t)rd);
}

static void a64_emit_lslv_w(A64Emitter *emitter,
                            unsigned rd,
                            unsigned rn,
                            unsigned rm) {
    a64_emit(emitter,
             0x1AC02000u | ((uint32_t)rm << 16u) |
             ((uint32_t)rn << 5u) | (uint32_t)rd);
}

static void a64_emit_lsrv_w(A64Emitter *emitter,
                            unsigned rd,
                            unsigned rn,
                            unsigned rm) {
    a64_emit(emitter,
             0x1AC02400u | ((uint32_t)rm << 16u) |
             ((uint32_t)rn << 5u) | (uint32_t)rd);
}

static void a64_emit_asrv_w(A64Emitter *emitter,
                            unsigned rd,
                            unsigned rn,
                            unsigned rm) {
    a64_emit(emitter,
             0x1AC02800u | ((uint32_t)rm << 16u) |
             ((uint32_t)rn << 5u) | (uint32_t)rd);
}

static void a64_emit_rorv_w(A64Emitter *emitter,
                            unsigned rd,
                            unsigned rn,
                            unsigned rm) {
    a64_emit(emitter,
             0x1AC02C00u | ((uint32_t)rm << 16u) |
             ((uint32_t)rn << 5u) | (uint32_t)rd);
}

static void a64_emit_neg_w(A64Emitter *emitter,
                           unsigned rd,
                           unsigned rm) {
    a64_emit(emitter,
             0x4B0003E0u | ((uint32_t)rm << 16u) | (uint32_t)rd);
}

static void a64_emit_mul_w(A64Emitter *emitter,
                           unsigned rd,
                           unsigned rn,
                           unsigned rm) {
    a64_emit(emitter,
             0x1B007C00u | ((uint32_t) rm << 16u) |
             ((uint32_t) rn << 5u) | (uint32_t) rd);
}

static void a64_emit_blr(A64Emitter *emitter, unsigned rn) {
    if (rn > 30u) {
        emitter->failed = 1;
        return;
    }
    a64_emit(emitter, 0xD63F0000u | ((uint32_t) rn << 5u));
}

static void a64_emit_function_address(A64Emitter *emitter,
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

static void a64_emit_prepare_call(A64Emitter *emitter) {
    a64_emit_mov_x(emitter, A64_X0, A64_X19);
    a64_emit_mov_x(emitter, A64_X1, A64_X20);
}

static void a64_emit_preserved_guest_flags(A64Emitter *emitter) {
    const uint32_t preserve_mask =
            ~(uint32_t) (FLAG_CF | FLAG_ZF | FLAG_SF | FLAG_OF);
    a64_emit_ldr_w(emitter, A64_X12, A64_X20, offsetof(VCPU, flags));
    a64_emit_mov_w_imm(emitter, A64_X13, preserve_mask);
    a64_emit_and_w(emitter, A64_X12, A64_X12, A64_X13);
}

/* NZCV >> 27 maps N/Z to the guest SF/ZF bit positions directly. */
static void a64_emit_logic_flags(A64Emitter *emitter, unsigned result_reg) {
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
static void a64_emit_arithmetic_flags(A64Emitter *emitter, int subtract) {
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

static size_t a64_reg_offset(unsigned guest_reg) {
    return offsetof(VCPU, regs) + (size_t) guest_reg * sizeof(uint32_t);
}

static int a64_guest_reg_valid(unsigned guest_reg) {
    return guest_reg < REG_COUNT;
}

static void a64_emit_guest_position(A64Emitter *emitter,
                                    const VM_DecodedOp *op) {
    const vm_addr_t next_ip = op->ip + (vm_addr_t) sizeof(uint64_t);
    a64_emit_mov_x_imm(emitter, A64_X8, (uint64_t) op->ip);
    a64_emit_str_x(emitter, A64_X8, A64_X20, offsetof(VCPU, last_ip));
    a64_emit_mov_x_imm(emitter, A64_X8, (uint64_t) next_ip);
    a64_emit_str_x(emitter, A64_X8, A64_X20, offsetof(VCPU, ip));
}

static void a64_emit_store_ip(A64Emitter *emitter, vm_addr_t target) {
    a64_emit_mov_x_imm(emitter, A64_X8, (uint64_t) target);
    a64_emit_str_x(emitter, A64_X8, A64_X20, offsetof(VCPU, ip));
}

static void a64_patch_test_branch(A64Emitter *emitter,
                                  size_t branch_index,
                                  unsigned rt,
                                  unsigned bit,
                                  int branch_if_nonzero) {
    size_t distance;
    uint32_t word;
    if (!emitter || branch_index >= emitter->count || rt > 31u || bit > 31u) {
        if (emitter) {
            emitter->failed = 1;
        }
        return;
    }
    distance = emitter->count - branch_index;
    if (distance > 0x1FFFu) {
        emitter->failed = 1;
        return;
    }
    word = branch_if_nonzero ? 0x37000000u : 0x36000000u;
    word |= ((uint32_t) bit & 0x1Fu) << 19u;
    word |= ((uint32_t) distance & 0x3FFFu) << 5u;
    word |= (uint32_t) rt;
    emitter->words[branch_index] = word;
}

static void a64_patch_test_branch_to(A64Emitter *emitter,
                                     size_t branch_index,
                                     size_t target_word,
                                     unsigned rt,
                                     unsigned bit,
                                     int branch_if_nonzero) {
    const int64_t distance = (int64_t) target_word - (int64_t) branch_index;
    uint32_t word;

    if (!emitter || branch_index >= emitter->count || rt > 31u || bit > 31u) {
        if (emitter) {
            emitter->failed = 1;
        }
        return;
    }

    if (distance < -(1ll << 13) || distance >= (1ll << 13)) {
        emitter->failed = 1;
        return;
    }

    word = branch_if_nonzero ? 0x37000000u : 0x36000000u;

    word |= ((uint32_t) bit & 0x1Fu) << 19u;
    word |= ((uint32_t) distance & 0x3FFFu) << 5u;
    word |= (uint32_t) rt;

    emitter->words[branch_index] = word;
}


static void a64_patch_cond_branch_to(A64Emitter *emitter,
                                     size_t branch_index,
                                     size_t target_word,
                                     unsigned condition) {
    const int64_t distance = (int64_t) target_word - (int64_t) branch_index;
    if (!emitter || branch_index >= emitter->count || condition > 15u) {
        if (emitter) {
            emitter->failed = 1;
        }
        return;
    }

    if (distance < -(1ll << 18) || distance >= (1ll << 18)) {
        emitter->failed = 1;
        return;
    }

    emitter->words[branch_index] =
        0x54000000u |
        (((uint32_t)distance & 0x7FFFFu) << 5u) |
        (uint32_t)condition;
}

static void a64_patch_b_to(A64Emitter *emitter,
                           size_t branch_index,
                           size_t target_word) {
    const int64_t distance = (int64_t) target_word - (int64_t) branch_index;
    if (!emitter || branch_index >= emitter->count) {
        if (emitter) {
            emitter->failed = 1;
        }
        return;
    }

    if (distance < -(1ll << 25) || distance >= (1ll << 25)) {
        emitter->failed = 1;
        return;
    }

    emitter->words[branch_index] = 0x14000000u | ((uint32_t) distance & 0x3ffffffu);
}

static void a64_apply_fixups(A64Emitter *emitter,
                             const size_t *op_words,
                             uint32_t op_count,
                             size_t epilogue_word) {
    if (!emitter || !op_words) {
        if (emitter) {
            emitter->failed = 1;
        }
        return;
    }

    for (size_t i = 0u;
         i < emitter->fixup_count && !emitter->failed;
         ++i) {
        const A64BranchFixup *fixup = &emitter->fixups[i];
        size_t target_word;

        switch (fixup->target_kind) {
            case A64_FIXUP_TARGET_OP:
                if (fixup->target_op_index >= op_count) {
                    emitter->failed = 1;
                    return;
                }

                target_word =
                    op_words[fixup->target_op_index];
                break;

            case A64_FIXUP_TARGET_EPILOGUE:
                target_word = epilogue_word;
                break;

            default:
                emitter->failed = 1;
                return;
        }

        switch (fixup->kind) {
            case A64_FIXUP_B:
                a64_patch_b_to(emitter,
                               fixup->word_index,
                               target_word);
                break;

            case A64_FIXUP_TBZ:
                a64_patch_test_branch_to(emitter,
                                         fixup->word_index,
                                         target_word,
                                         fixup->rt,
                                         fixup->bit,
                                         0);
                break;

            case A64_FIXUP_TBNZ:
                a64_patch_test_branch_to(emitter,
                                         fixup->word_index,
                                         target_word,
                                         fixup->rt,
                                         fixup->bit,
                                         1);
                break;

            case A64_FIXUP_BCOND:
                a64_patch_cond_branch_to(emitter,
                                         fixup->word_index,
                                         target_word,
                                         fixup->condition);
                break;

            default:
                emitter->failed = 1;
                return;
        }
    }
}



static void a64_emit_b_to(A64Emitter *emitter, size_t target_index) {
    const int64_t distance =
            (int64_t) target_index - (int64_t) emitter->count;
    if (distance < -(1ll << 25) || distance >= (1ll << 25)) {
        emitter->failed = 1;
        return;
    }
    a64_emit(emitter,
             0x14000000u | ((uint32_t) distance & 0x03FFFFFFu));
}

static int a64_is_branch(uint8_t op) {
    switch (op) {
        case OP_JMP:
        case OP_RJMP:
        case OP_JZ:
        case OP_JNZ:
        case OP_RJZ:
        case OP_RJNZ:
            return 1;

        default:
            return 0;
    }
}

static void a64_emit_conditional_ip(A64Emitter *emitter,
                                    vm_addr_t target,
                                    int take_when_zero_set) {
    size_t skip_index;
    a64_emit_ldr_w(emitter, A64_X8, A64_X20, offsetof(VCPU, flags));
    skip_index = emitter->count;
    a64_emit(emitter, 0u);
    a64_emit_store_ip(emitter, target);
    /* JZ skips the target write when ZF is clear; JNZ skips when it is set. */
    a64_patch_test_branch(emitter,
                          skip_index,
                          A64_X8,
                          3u,
                          take_when_zero_set ? 0 : 1);
}

static void a64_emit_memory_address(A64Emitter *emitter,
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
static void a64_add_op_fixup(A64Emitter *emitter,
                             A64FixupKind kind,
                             size_t word_index,
                             uint32_t target_op_index,
                             unsigned rt,
                             unsigned bit,
                             unsigned condition) {
    if (!emitter || emitter->failed) {
        return;
    }

    if (emitter->fixup_count >=
        sizeof(emitter->fixups) / sizeof(emitter->fixups[0])) {
        emitter->failed = 1;
        return;
    }

    A64BranchFixup *fixup =
        &emitter->fixups[emitter->fixup_count++];

    fixup->kind = kind;
    fixup->target_kind = A64_FIXUP_TARGET_OP;
    fixup->word_index = word_index;
    fixup->target_op_index = target_op_index;
    fixup->rt = rt;
    fixup->bit = bit;
    fixup->condition = condition;
}

static void a64_add_epilogue_fixup(A64Emitter *emitter,
                                   A64FixupKind kind,
                                   size_t word_index,
                                   unsigned condition) {
    if (!emitter || emitter->failed) {
        return;
    }

    if (emitter->fixup_count >=
        sizeof(emitter->fixups) / sizeof(emitter->fixups[0])) {
        emitter->failed = 1;
        return;
    }

    A64BranchFixup *fixup =
        &emitter->fixups[emitter->fixup_count++];

    fixup->kind = kind;
    fixup->target_kind = A64_FIXUP_TARGET_EPILOGUE;
    fixup->word_index = word_index;
    fixup->target_op_index = 0u;
    fixup->rt = 0u;
    fixup->bit = 0u;
    fixup->condition = condition;
}
static vm_addr_t a64_relative_target(const VM_DecodedOp *op) {
    const int64_t target = (int64_t) op->ip + (int64_t) op->imm;
    return (vm_addr_t) target;
}

static int a64_direct_branch_target(const VM_DecodedOp *op,
                                    vm_addr_t *target) {
    if (!op || !target) {
        return 0;
    }
    switch (op->op) {
        case OP_JMP:
        case OP_JZ:
        case OP_JNZ:
            *target = (vm_addr_t) op->imm;
            return 1;
        case OP_RJMP:
        case OP_RJZ:
        case OP_RJNZ:
            *target = a64_relative_target(op);
            return 1;
        default:
            return 0;
    }
}

static int a64_find_block_target(const VmJitBlock *block,
                                 vm_addr_t target,
                                 uint32_t *target_index) {
    if (!block || !target_index) {
        return 0;
    }
    for (uint32_t i = 0u; i < block->count; ++i) {
        if (block->ops[i].ip == target) {
            *target_index = i;
            return 1;
        }
    }
    return 0;
}

static void a64_emit_native_backedge(A64Emitter *emitter,
                                     uint8_t op,
                                     vm_addr_t target_ip,
                                     size_t target_word) {
    size_t condition_fallthrough = SIZE_MAX;

    const int conditional =
        op == OP_JZ ||
        op == OP_JNZ ||
        op == OP_RJZ ||
        op == OP_RJNZ;

    if (conditional) {
        a64_emit_ldr_w(emitter,
                       A64_X8,
                       A64_X20,
                       offsetof(VCPU, flags));

        condition_fallthrough = emitter->count;
        a64_emit(emitter, 0u);
    }
    a64_emit_store_ip(emitter, target_ip);

    a64_emit_cmp_w_imm(emitter,
                       A64_X21,
                       VM_JIT_NATIVE_LOOP_BUDGET);

    const size_t budget_exit = emitter->count;
    a64_emit(emitter, 0u);

    a64_add_epilogue_fixup(emitter,
                           A64_FIXUP_BCOND,
                           budget_exit,
                           2u);

    a64_emit_b_to(emitter, target_word);

    if (conditional) {
        const int take_when_zero_set =
            op == OP_JZ || op == OP_RJZ;

        a64_patch_test_branch(
            emitter,
            condition_fallthrough,
            A64_X8,
            3u,
            take_when_zero_set ? 0 : 1);
    }
}
static int vm_jit_arm64_available(void) {
    return sizeof(void *) == 8u && sizeof(size_t) == 8u;
}

static int vm_jit_arm64_supports_opcode(uint8_t op) {
    switch (op) {
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_MOV:
        case OP_MOVI:
        case OP_INC:
        case OP_ADDI:
        case OP_SUBI:
        case OP_CMP:
        case OP_CMPI:
        case OP_AND:
        case OP_OR:
        case OP_XOR:
        case OP_NOT:
        case OP_SHL:
        case OP_SHR:
        case OP_SAR:
        case OP_ROL:
        case OP_ROR:
        case OP_LOAD:
        case OP_LOAD32:
        case OP_STORE:
        case OP_STORE32:
        case OP_JMP:
        case OP_RJMP:
        case OP_JZ:
        case OP_JNZ:
        case OP_RJZ:
        case OP_RJNZ:
        case OP_FENCE:
        case OP_PAUSE:
        case OP_CPUID:
            return 1;
        default:
            return 0;
    }
}

static int vm_jit_arm64_terminates_block(uint8_t op) {
    switch (op) {
        case OP_STORE:
        case OP_STORE32:
        case OP_JMP:
        case OP_RJMP:
            return 1;
        case OP_JZ:
        case OP_JNZ:
        case OP_RJZ:
        case OP_RJNZ:
            return 0;
        default:
            return 0;
    }
}

static void a64_emit_conditional_exit(A64Emitter *emitter,
                                      vm_addr_t target,
                                      int take_when_zero_set) {
    a64_emit_ldr_w(emitter,
                   A64_X8,
                   A64_X20,
                   offsetof(VCPU, flags));

    const size_t fallthrough_branch = emitter->count;
    a64_emit(emitter, 0u);

    a64_emit_store_ip(emitter, target);

    const size_t exit_branch = emitter->count;
    a64_emit(emitter, 0u);

    a64_add_epilogue_fixup(emitter,
                           A64_FIXUP_B,
                           exit_branch,
                           0u);

    /*
     * JZ:  skip taken path when ZF == 0 -> TBZ
     * JNZ: skip taken path when ZF == 1 -> TBNZ
     */
    a64_patch_test_branch(emitter,
                          fallthrough_branch,
                          A64_X8,
                          3u,
                          take_when_zero_set ? 0 : 1);
}

static void a64_emit_one(A64Emitter *emitter,
                         const VM_DecodedOp *op,
                         const VmJitMemoryOps *memory) {
    a64_emit_guest_position(emitter, op);

    switch (op->op) {
        case OP_MOVI:
            if (!a64_guest_reg_valid(op->rd)) {
                emitter->failed = 1;
                return;
            }
            a64_emit_mov_w_imm(emitter, A64_X8, (uint32_t) op->imm);
            a64_emit_str_w(emitter, A64_X8, A64_X20, a64_reg_offset(op->rd));
            a64_emit_logic_flags(emitter, A64_X8);
            return;
        case OP_MOV:
            if (!a64_guest_reg_valid(op->rd) ||
                !a64_guest_reg_valid(op->rs1)) {
                emitter->failed = 1;
                return;
            }
            a64_emit_ldr_w(emitter, A64_X8, A64_X20,
                           a64_reg_offset(op->rs1));
            a64_emit_str_w(emitter, A64_X8, A64_X20,
                           a64_reg_offset(op->rd));
            a64_emit_logic_flags(emitter, A64_X8);
            return;
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
            if (!a64_guest_reg_valid(op->rd) ||
                !a64_guest_reg_valid(op->rs1) ||
                !a64_guest_reg_valid(op->rs2)) {
                emitter->failed = 1;
                return;
            }
            a64_emit_ldr_w(emitter, A64_X8, A64_X20,
                           a64_reg_offset(op->rs1));
            a64_emit_ldr_w(emitter, A64_X9, A64_X20,
                           a64_reg_offset(op->rs2));
            if (op->op == OP_ADD) {
                a64_emit_adds_w(emitter, A64_X10, A64_X8, A64_X9);
            } else if (op->op == OP_SUB) {
                a64_emit_subs_w(emitter, A64_X10, A64_X8, A64_X9);
            } else {
                a64_emit_mul_w(emitter, A64_X10, A64_X8, A64_X9);
            }
            a64_emit_str_w(emitter, A64_X10, A64_X20,
                           a64_reg_offset(op->rd));
            if (op->op == OP_ADD) {
                a64_emit_arithmetic_flags(emitter, 0);
            } else if (op->op == OP_SUB) {
                a64_emit_arithmetic_flags(emitter, 1);
            } else {
                a64_emit_logic_flags(emitter, A64_X10);
            }
            return;
        case OP_INC:
        case OP_ADDI:
        case OP_SUBI:
            if (!a64_guest_reg_valid(op->rd) ||
                (op->op != OP_INC && !a64_guest_reg_valid(op->rs1))) {
                emitter->failed = 1;
                return;
            }
            a64_emit_ldr_w(emitter, A64_X8, A64_X20,
                           a64_reg_offset(op->op == OP_INC ? op->rd : op->rs1));
            a64_emit_mov_w_imm(emitter,
                               A64_X9,
                               op->op == OP_INC ? 1u : (uint32_t) op->imm);
            if (op->op == OP_SUBI) {
                a64_emit_subs_w(emitter, A64_X10, A64_X8, A64_X9);
            } else {
                a64_emit_adds_w(emitter, A64_X10, A64_X8, A64_X9);
            }
            a64_emit_str_w(emitter, A64_X10, A64_X20,
                           a64_reg_offset(op->rd));
            if (op->op == OP_SUBI) {
                a64_emit_arithmetic_flags(emitter, 1);
            } else {
                a64_emit_arithmetic_flags(emitter, 0);
            }
            return;
        case OP_CMP:
        case OP_CMPI:
            if (!a64_guest_reg_valid(op->rd) ||
                (op->op == OP_CMP && !a64_guest_reg_valid(op->rs1))) {
                emitter->failed = 1;
                return;
            }
            a64_emit_ldr_w(emitter, A64_X8, A64_X20,
                           a64_reg_offset(op->rd));
            if (op->op == OP_CMP) {
                a64_emit_ldr_w(emitter, A64_X9, A64_X20,
                               a64_reg_offset(op->rs1));
            } else {
                a64_emit_mov_w_imm(emitter, A64_X9, (uint32_t) op->imm);
            }
            a64_emit_subs_w(emitter, A64_X10, A64_X8, A64_X9);
            a64_emit_arithmetic_flags(emitter, 1);
            return;
        case OP_AND:
        case OP_OR:
        case OP_XOR:
            if (!a64_guest_reg_valid(op->rd) ||
                !a64_guest_reg_valid(op->rs1) ||
                !a64_guest_reg_valid(op->rs2)) {
                emitter->failed = 1;
                return;
            }
            a64_emit_ldr_w(emitter, A64_X8, A64_X20,
                           a64_reg_offset(op->rs1));
            a64_emit_ldr_w(emitter, A64_X9, A64_X20,
                           a64_reg_offset(op->rs2));
            if (op->op == OP_AND) {
                a64_emit_and_w(emitter, A64_X10, A64_X8, A64_X9);
            } else if (op->op == OP_OR) {
                a64_emit_orr_w(emitter, A64_X10, A64_X8, A64_X9);
            } else {
                a64_emit_eor_w(emitter, A64_X10, A64_X8, A64_X9);
            }
            a64_emit_str_w(emitter, A64_X10, A64_X20,
                           a64_reg_offset(op->rd));
            a64_emit_logic_flags(emitter, A64_X10);
            return;
        case OP_NOT:
            if (!a64_guest_reg_valid(op->rd) ||
                !a64_guest_reg_valid(op->rs1)) {
                emitter->failed = 1;
                return;
            }
            a64_emit_ldr_w(emitter, A64_X8, A64_X20,
                           a64_reg_offset(op->rs1));
            a64_emit_mvn_w(emitter, A64_X10, A64_X8);
            a64_emit_str_w(emitter, A64_X10, A64_X20,
                           a64_reg_offset(op->rd));
            a64_emit_logic_flags(emitter, A64_X10);
            return;
        case OP_SHL:
        case OP_SHR:
        case OP_SAR:
        case OP_ROL:
        case OP_ROR:
            if (!a64_guest_reg_valid(op->rd) ||
                !a64_guest_reg_valid(op->rs1) ||
                !a64_guest_reg_valid(op->rs2)) {
                emitter->failed = 1;
                return;
            }
            a64_emit_ldr_w(emitter, A64_X8, A64_X20,
                           a64_reg_offset(op->rs1));
            a64_emit_ldr_w(emitter, A64_X9, A64_X20,
                           a64_reg_offset(op->rs2));
            switch (op->op) {
                case OP_SHL:
                    a64_emit_lslv_w(emitter, A64_X10, A64_X8, A64_X9);
                    break;
                case OP_SHR:
                    a64_emit_lsrv_w(emitter, A64_X10, A64_X8, A64_X9);
                    break;
                case OP_SAR:
                    a64_emit_asrv_w(emitter, A64_X10, A64_X8, A64_X9);
                    break;
                case OP_ROL:
                    a64_emit_neg_w(emitter, A64_X9, A64_X9);
                    a64_emit_rorv_w(emitter, A64_X10, A64_X8, A64_X9);
                    break;
                case OP_ROR:
                    a64_emit_rorv_w(emitter, A64_X10, A64_X8, A64_X9);
                    break;
                default:
                    emitter->failed = 1;
                    return;
            }
            a64_emit_str_w(emitter, A64_X10, A64_X20,
                           a64_reg_offset(op->rd));
            a64_emit_logic_flags(emitter, A64_X10);
            return;
        case OP_LOAD:
        case OP_LOAD32: {
            if (!a64_guest_reg_valid(op->rd)) {
                emitter->failed = 1;
                return;
            }
            a64_emit_memory_address(emitter, op->rs1, op->imm);
            a64_emit_prepare_call(emitter);
            if (op->op == OP_LOAD) {
                VmJitRead8Fn helper = memory->read8;
                a64_emit_function_address(emitter, A64_X16,
                                          &helper, sizeof(helper));
            } else {
                VmJitRead32Fn helper = memory->read32;
                a64_emit_function_address(emitter, A64_X16,
                                          &helper, sizeof(helper));
            }
            a64_emit_blr(emitter, A64_X16);
            a64_emit_mov_w(emitter, A64_X8, A64_X0);
            a64_emit_str_w(emitter, A64_X8, A64_X20,
                           a64_reg_offset(op->rd));
            a64_emit_logic_flags(emitter, A64_X8);
            return;
        }
        case OP_STORE:
        case OP_STORE32: {
            if (!a64_guest_reg_valid(op->rd)) {
                emitter->failed = 1;
                return;
            }
            a64_emit_memory_address(emitter, op->rs1, op->imm);
            a64_emit_ldr_w(emitter, A64_X3, A64_X20,
                           a64_reg_offset(op->rd));
            a64_emit_prepare_call(emitter);
            if (op->op == OP_STORE) {
                VmJitWrite8Fn helper = memory->write8;
                a64_emit_function_address(emitter, A64_X16,
                                          &helper, sizeof(helper));
            } else {
                VmJitWrite32Fn helper = memory->write32;
                a64_emit_function_address(emitter, A64_X16,
                                          &helper, sizeof(helper));
            }
            a64_emit_blr(emitter, A64_X16);
            return;
        }
        case OP_JMP:
            a64_emit_store_ip(emitter, (vm_addr_t) op->imm);
            return;
        case OP_RJMP:
            a64_emit_store_ip(emitter, a64_relative_target(op));
            return;
        case OP_JZ:
            a64_emit_conditional_ip(emitter, (vm_addr_t) op->imm, 1);
            return;
        case OP_JNZ:
            a64_emit_conditional_ip(emitter, (vm_addr_t) op->imm, 0);
            return;
        case OP_RJZ:
            a64_emit_conditional_ip(emitter, a64_relative_target(op), 1);
            return;
        case OP_RJNZ:
            a64_emit_conditional_ip(emitter, a64_relative_target(op), 0);
            return;
        case OP_FENCE:
            a64_emit(emitter, 0xD5033BBFu); /* dmb ish */
            return;
        case OP_PAUSE:
            a64_emit(emitter, 0xD503203Fu); /* yield */
            return;
        case OP_CPUID:
            if (!a64_guest_reg_valid(op->rd)) {
                emitter->failed = 1;
                return;
            }
            a64_emit_ldr_w(emitter, A64_X8, A64_X20, offsetof(VCPU, core_id));
            a64_emit_str_w(emitter, A64_X8, A64_X20,
                           a64_reg_offset(op->rd));
            a64_emit_logic_flags(emitter, A64_X8);
            return;
        default:
            emitter->failed = 1;
            return;
    }
}

static int vm_jit_arm64_compile(const VmJitBlock *block,
                                const VmJitMemoryOps *memory,
                                VmJitCode *out) {
    A64Emitter emitter;
    size_t op_words[VM_JIT_BLOCK_MAX_OPS];

    if (!block ||
        block->count == 0u ||
        block->count > VM_JIT_BLOCK_MAX_OPS ||
        !memory ||
        !memory->read8 ||
        !memory->read32 ||
        !memory->write8 ||
        !memory->write32 ||
        !out) {
        return 0;
    }

    memset(&emitter, 0, sizeof(emitter));

    /*
     * Prologue.
     *
     * x19 = VM *
     * x20 = VCPU *
     * w21 = number of guest instructions executed in this entry
     */
    a64_emit_stp_pre(&emitter,
                     A64_X19,
                     A64_X20,
                     A64_SP,
                     -16);

    a64_emit_stp_pre(&emitter,
                     A64_X21,
                     A64_X22,
                     A64_SP,
                     -16);

    a64_emit_stp_pre(&emitter,
                     A64_X29,
                     A64_X30,
                     A64_SP,
                     -16);

    a64_emit_mov_x(&emitter, A64_X19, A64_X0);
    a64_emit_mov_x(&emitter, A64_X20, A64_X1);
    a64_emit_mov_w_imm(&emitter, A64_X21, 0u);

    for (uint32_t i = 0u;
         i < block->count && !emitter.failed;
         ++i) {
        const VM_DecodedOp *op = &block->ops[i];

        vm_addr_t target = 0;
        uint32_t target_index = 0u;

        op_words[i] = emitter.count;

        const int is_branch =
            a64_is_branch(op->op);

        int has_direct_target = 0;
        int target_in_block = 0;

        if (is_branch) {
            has_direct_target =
                a64_direct_branch_target(op, &target);

            if (!has_direct_target) {
                emitter.failed = 1;
                break;
            }

            target_in_block =
                a64_find_block_target(block,
                                      target,
                                      &target_index);
        }

        /*
         * Ordinary non-control-flow instruction.
         */
        if (!is_branch) {
            a64_emit_one(&emitter, op, memory);

            a64_emit_add_w_imm(&emitter,
                               A64_X21,
                               A64_X21,
                               1u);
            continue;
        }

        a64_emit_guest_position(&emitter, op);

        a64_emit_add_w_imm(&emitter,
                           A64_X21,
                           A64_X21,
                           1u);

        /*
         * ------------------------------------------------------------
         * 1. Intra-region backedge / self edge
         * ------------------------------------------------------------
         */
        if (target_in_block &&
            target_index <= i) {
            a64_emit_native_backedge(&emitter,
                                     op->op,
                                     target,
                                     op_words[target_index]);
            continue;
        }

        /*
         * ------------------------------------------------------------
         * 2. Intra-region forward edge
         * ------------------------------------------------------------
         */
        if (target_in_block) {
            switch (op->op) {
                case OP_JMP:
                case OP_RJMP: {
                    const size_t branch_word =
                        emitter.count;

                    a64_emit(&emitter, 0u);

                    a64_add_op_fixup(&emitter,
                                     A64_FIXUP_B,
                                     branch_word,
                                     target_index,
                                     0u,
                                     0u,
                                     0u);
                    break;
                }

                case OP_JZ:
                case OP_RJZ:
                case OP_JNZ:
                case OP_RJNZ: {
                    a64_emit_ldr_w(&emitter,
                                   A64_X8,
                                   A64_X20,
                                   offsetof(VCPU, flags));

                    const size_t branch_word =
                        emitter.count;

                    a64_emit(&emitter, 0u);

                    const int jump_when_set =
                        op->op == OP_JZ ||
                        op->op == OP_RJZ;

                    a64_add_op_fixup(
                        &emitter,
                        jump_when_set
                            ? A64_FIXUP_TBNZ
                            : A64_FIXUP_TBZ,
                        branch_word,
                        target_index,
                        A64_X8,
                        3u,
                        0u);
                    break;
                }

                default:
                    emitter.failed = 1;
                    break;
            }

            continue;
        }

        /*
         * ------------------------------------------------------------
         * 3. External edge
         * ------------------------------------------------------------
         *
         * Conditional external edges are side exits.
         *
         * Unconditional branches still terminate region construction,
         * so after storing their target IP we can naturally fall into
         * the common epilogue.
         */
        switch (op->op) {
            case OP_JZ:
            case OP_RJZ:
            case OP_JNZ:
            case OP_RJNZ: {
                const int take_when_zero_set =
                    op->op == OP_JZ ||
                    op->op == OP_RJZ;

                a64_emit_conditional_exit(
                    &emitter,
                    target,
                    take_when_zero_set);
                break;
            }

            case OP_JMP:
            case OP_RJMP:
                a64_emit_store_ip(&emitter, target);
                break;

            default:
                emitter.failed = 1;
                break;
        }
    }

    const size_t epilogue_word = emitter.count;

    a64_apply_fixups(&emitter,
                     op_words,
                     block->count,
                     epilogue_word);

    a64_emit_mov_w(&emitter,
                   A64_X0,
                   A64_X21);

    a64_emit_ldp_post(&emitter,
                      A64_X29,
                      A64_X30,
                      A64_SP,
                      16);

    a64_emit_ldp_post(&emitter,
                      A64_X21,
                      A64_X22,
                      A64_SP,
                      16);

    a64_emit_ldp_post(&emitter,
                      A64_X19,
                      A64_X20,
                      A64_SP,
                      16);

    a64_emit(&emitter, 0xD65F03C0u); /* ret */

    if (emitter.failed) {
        return 0;
    }

    return vm_jit_code_publish(emitter.words,
                               emitter.count,
                               out);
}
#else

static int vm_jit_arm64_available(void) {
    return 0;
}

static int vm_jit_arm64_supports_opcode(uint8_t op) {
    (void) op;
    return 0;
}

static int vm_jit_arm64_terminates_block(uint8_t op) {
    (void) op;
    return 1;
}

static int vm_jit_arm64_compile(const VmJitBlock *block,
                                const VmJitMemoryOps *memory,
                                VmJitCode *out) {
    (void) block;
    (void) memory;
    (void) out;
    return 0;
}

#endif

const VmJitBackend *vm_jit_arm64_backend(void) {
    static const VmJitBackend backend = {
        .name = "arm64-tier0",
        .available = vm_jit_arm64_available,
        .supports_opcode = vm_jit_arm64_supports_opcode,
        .terminates_block = vm_jit_arm64_terminates_block,
        .compile = vm_jit_arm64_compile,
        .memory_ops = vm_jit_arm64_memory_ops,
    };
    return &backend;
}
