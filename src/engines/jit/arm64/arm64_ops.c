#include "arm64_internal.h"

#if defined(__aarch64__)

void a64_emit_one(A64Emitter *emitter,
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

#endif /* __aarch64__ */
