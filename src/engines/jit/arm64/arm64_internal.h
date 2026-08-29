#ifndef LAMP_VM_ARM64_INTERNAL_H
#define LAMP_VM_ARM64_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../code_memory.h"
#include "../codegen.h"
#include "../../../flags.h"
#include "../../../vm.h"

#if defined(__aarch64__)

/*
 * ARM64 register allocations.
 *
 * Calling convention / guest mapping:
 *   X0..X4   - Scratch & argument passing
 *   X8..X14  - Scratch & temporary registers
 *   X16      - Helper call target address
 *   X19      - VM* pointer (callee-saved)
 *   X20      - VCPU* pointer (callee-saved)
 *   X21      - Guest instruction counter (callee-saved)
 *   X22      - Callee-saved reserve
 *   X29      - Frame pointer (FP)
 *   X30      - Link register (LR)
 *   SP (31)  - Stack pointer
 */
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

/* ========================================================================= */
/* Emitter & Low-level Instruction Generation (arm64_emitter.c)             */
/* ========================================================================= */

void a64_emit(A64Emitter *emitter, uint32_t word);

void a64_emit_stp_pre(A64Emitter *emitter,
                      unsigned rt,
                      unsigned rt2,
                      unsigned rn,
                      int byte_offset);

void a64_emit_ldp_post(A64Emitter *emitter,
                       unsigned rt,
                       unsigned rt2,
                       unsigned rn,
                       int byte_offset);

void a64_emit_mov_x(A64Emitter *emitter,
                    unsigned rd,
                    unsigned rm);

void a64_emit_mov_w(A64Emitter *emitter,
                    unsigned rd,
                    unsigned rm);

void a64_emit_mov_w_imm(A64Emitter *emitter,
                        unsigned rd,
                        uint32_t value);

void a64_emit_mov_x_imm(A64Emitter *emitter,
                        unsigned rd,
                        uint64_t value);

void a64_emit_ldr_w(A64Emitter *emitter,
                    unsigned rt,
                    unsigned rn,
                    size_t byte_offset);

void a64_emit_str_w(A64Emitter *emitter,
                    unsigned rt,
                    unsigned rn,
                    size_t byte_offset);

void a64_emit_str_x(A64Emitter *emitter,
                    unsigned rt,
                    unsigned rn,
                    size_t byte_offset);

void a64_emit_add_w(A64Emitter *emitter,
                    unsigned rd,
                    unsigned rn,
                    unsigned rm);

void a64_emit_add_w_imm(A64Emitter *emitter,
                        unsigned rd,
                        unsigned rn,
                        uint32_t immediate);

void a64_emit_cmp_w_imm(A64Emitter *emitter,
                        unsigned rn,
                        uint32_t immediate);

void a64_emit_adds_w(A64Emitter *emitter,
                     unsigned rd,
                     unsigned rn,
                     unsigned rm);

void a64_emit_subs_w(A64Emitter *emitter,
                     unsigned rd,
                     unsigned rn,
                     unsigned rm);

void a64_emit_mrs_nzcv(A64Emitter *emitter, unsigned rt);

void a64_emit_lsr_w(A64Emitter *emitter,
                    unsigned rd,
                    unsigned rn,
                    unsigned shift);

void a64_emit_lsl_w(A64Emitter *emitter,
                    unsigned rd,
                    unsigned rn,
                    unsigned shift);

void a64_emit_and_w(A64Emitter *emitter,
                    unsigned rd,
                    unsigned rn,
                    unsigned rm);

void a64_emit_orr_w(A64Emitter *emitter,
                    unsigned rd,
                    unsigned rn,
                    unsigned rm);

void a64_emit_eor_w(A64Emitter *emitter,
                    unsigned rd,
                    unsigned rn,
                    unsigned rm);

void a64_emit_mvn_w(A64Emitter *emitter,
                    unsigned rd,
                    unsigned rm);

void a64_emit_lslv_w(A64Emitter *emitter,
                     unsigned rd,
                     unsigned rn,
                     unsigned rm);

void a64_emit_lsrv_w(A64Emitter *emitter,
                     unsigned rd,
                     unsigned rn,
                     unsigned rm);

void a64_emit_asrv_w(A64Emitter *emitter,
                     unsigned rd,
                     unsigned rn,
                     unsigned rm);

void a64_emit_rorv_w(A64Emitter *emitter,
                     unsigned rd,
                     unsigned rn,
                     unsigned rm);

void a64_emit_neg_w(A64Emitter *emitter,
                    unsigned rd,
                    unsigned rm);

void a64_emit_mul_w(A64Emitter *emitter,
                    unsigned rd,
                    unsigned rn,
                    unsigned rm);

void a64_emit_blr(A64Emitter *emitter, unsigned rn);

void a64_emit_function_address(A64Emitter *emitter,
                               unsigned rd,
                               const void *representation,
                               size_t representation_size);

void a64_emit_prepare_call(A64Emitter *emitter);

void a64_emit_preserved_guest_flags(A64Emitter *emitter);

void a64_emit_logic_flags(A64Emitter *emitter, unsigned result_reg);

void a64_emit_arithmetic_flags(A64Emitter *emitter, int subtract);

size_t a64_reg_offset(unsigned guest_reg);

int a64_guest_reg_valid(unsigned guest_reg);

void a64_emit_guest_position(A64Emitter *emitter,
                             const VM_DecodedOp *op);

void a64_emit_store_ip(A64Emitter *emitter, vm_addr_t target);

void a64_emit_memory_address(A64Emitter *emitter,
                             unsigned base_guest_reg,
                             int32_t immediate);

/* ========================================================================= */
/* Branching, Fixups & Control Flow (arm64_branch.c)                        */
/* ========================================================================= */

void a64_patch_test_branch(A64Emitter *emitter,
                           size_t branch_index,
                           unsigned rt,
                           unsigned bit,
                           int branch_if_nonzero);

void a64_patch_test_branch_to(A64Emitter *emitter,
                              size_t branch_index,
                              size_t target_word,
                              unsigned rt,
                              unsigned bit,
                              int branch_if_nonzero);

void a64_patch_cond_branch_to(A64Emitter *emitter,
                              size_t branch_index,
                              size_t target_word,
                              unsigned condition);

void a64_patch_b_to(A64Emitter *emitter,
                    size_t branch_index,
                    size_t target_word);

void a64_apply_fixups(A64Emitter *emitter,
                      const size_t *op_words,
                      uint32_t op_count,
                      size_t epilogue_word);

void a64_emit_b_to(A64Emitter *emitter, size_t target_index);

int a64_is_branch(uint8_t op);

void a64_emit_conditional_ip(A64Emitter *emitter,
                             vm_addr_t target,
                             int take_when_zero_set);

void a64_add_op_fixup(A64Emitter *emitter,
                      A64FixupKind kind,
                      size_t word_index,
                      uint32_t target_op_index,
                      unsigned rt,
                      unsigned bit,
                      unsigned condition);

void a64_add_epilogue_fixup(A64Emitter *emitter,
                            A64FixupKind kind,
                            size_t word_index,
                            unsigned condition);

vm_addr_t a64_relative_target(const VM_DecodedOp *op);

int a64_direct_branch_target(const VM_DecodedOp *op,
                             vm_addr_t *target);

int a64_find_block_target(const VmJitBlock *block,
                          vm_addr_t target,
                          uint32_t *target_index);

void a64_emit_native_backedge(A64Emitter *emitter,
                              uint8_t op,
                              vm_addr_t target_ip,
                              size_t target_word);

void a64_emit_conditional_exit(A64Emitter *emitter,
                               vm_addr_t target,
                               int take_when_zero_set);

/* ========================================================================= */
/* Opcode Lowering (arm64_ops.c)                                            */
/* ========================================================================= */

void a64_emit_one(A64Emitter *emitter,
                  const VM_DecodedOp *op,
                  const VmJitMemoryOps *memory);

#endif /* __aarch64__ */

#endif /* LAMP_VM_ARM64_INTERNAL_H */
