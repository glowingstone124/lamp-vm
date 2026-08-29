#include "arm64_internal.h"

#if defined(__aarch64__)

void a64_patch_test_branch(A64Emitter *emitter,
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

void a64_patch_test_branch_to(A64Emitter *emitter,
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

void a64_patch_cond_branch_to(A64Emitter *emitter,
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

void a64_patch_b_to(A64Emitter *emitter,
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

void a64_apply_fixups(A64Emitter *emitter,
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

void a64_emit_b_to(A64Emitter *emitter, size_t target_index) {
    const int64_t distance =
            (int64_t) target_index - (int64_t) emitter->count;
    if (distance < -(1ll << 25) || distance >= (1ll << 25)) {
        emitter->failed = 1;
        return;
    }
    a64_emit(emitter,
             0x14000000u | ((uint32_t) distance & 0x03FFFFFFu));
}

int a64_is_branch(uint8_t op) {
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

void a64_emit_conditional_ip(A64Emitter *emitter,
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

void a64_add_op_fixup(A64Emitter *emitter,
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

void a64_add_epilogue_fixup(A64Emitter *emitter,
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

vm_addr_t a64_relative_target(const VM_DecodedOp *op) {
    const int64_t target = (int64_t) op->ip + (int64_t) op->imm;
    return (vm_addr_t) target;
}

int a64_direct_branch_target(const VM_DecodedOp *op,
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

int a64_find_block_target(const VmJitBlock *block,
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

void a64_emit_native_backedge(A64Emitter *emitter,
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

void a64_emit_conditional_exit(A64Emitter *emitter,
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

#endif /* __aarch64__ */
