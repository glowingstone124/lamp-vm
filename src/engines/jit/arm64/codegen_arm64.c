#include "arm64_internal.h"

#if defined(__aarch64__)

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
