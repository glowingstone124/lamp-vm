//
// Created by Max Wang on 2025/12/28.
//

#ifndef VM_INSTRUCTION_H
#define VM_INSTRUCTION_H
static inline uint64_t INST(uint8_t op, uint8_t rd, uint8_t rs1, uint8_t rs2, uint32_t imm) {
    return ((uint64_t)op << 56 | (uint64_t)rd << 48 | (uint64_t)rs1 << 40 | (uint64_t)rs2 << 32) |
        imm;
}
#endif // VM_INSTRUCTION_H