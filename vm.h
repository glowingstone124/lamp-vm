
//
// Created by Max Wang on 2025/12/28.
//

#ifndef VM_VM_H
#define VM_VM_H
#include <stdint.h>
#include <stdio.h>
static inline uint64_t INST(uint8_t op, uint8_t rd, uint8_t rs1, uint8_t rs2, uint32_t imm) {
    return ((uint64_t)op << 56 | (uint64_t)rd << 48 | (uint64_t)rs1 << 40 | (uint64_t)rs2 << 32) |
        imm;
}

#define FB_WIDTH 640
#define FB_HEIGHT 480
#define FB_BPP 4
#define FB_SIZE (FB_WIDTH * FB_HEIGHT * FB_BPP)

#define FLAG_ZF 1
#define IO_SIZE 256

#define REG_COUNT 8
#define DUMP_MEM_SEEK_LEN 16

#define IVT_SIZE 256
#define IVT_ENTRY_SIZE 8
#define CALL_STACK_SIZE 256
#define DATA_STACK_SIZE 256

#define IVT_BASE 0x0000
#define CALL_STACK_BASE (IVT_BASE + IVT_SIZE * IVT_ENTRY_SIZE)
#define DATA_STACK_BASE (CALL_STACK_BASE + CALL_STACK_SIZE * 8)
#define PROGRAM_BASE (DATA_STACK_BASE + DATA_STACK_SIZE * 8)
#define FB_BASE(addr_space_size) ((addr_space_size) - FB_SIZE)
typedef uint32_t vm_addr_t;

typedef struct {
    FILE *fp;
    int lba;
    int mem_addr;
    int count;
    int status;
} Disk;

typedef struct {
    int regs[REG_COUNT];
    uint64_t *code;
    size_t ip;
    size_t execution_times;
    int halted;
    int panic;
    unsigned int flags;

    int data_stack[DATA_STACK_SIZE];
    int dsp;

    int call_stack[CALL_STACK_SIZE];
    int csp;

    uint8_t *memory;
    size_t memory_size;
    /*
     * framebuffer targets to a segment in our memory
     * [fb_base, fb_base + FB_SIZE)
     */
    uint32_t *fb;

    int io[IO_SIZE];

    Disk disk;

    int interrupt_flags[IVT_SIZE];
    int in_interrupt;
} VM;
enum {
    OP_ADD = 1,
    OP_SUB,
    OP_MUL,
    OP_HALT,
    OP_JMP,
    OP_JZ,
    OP_PUSH,
    OP_POP,
    OP_CALL,
    OP_RET,
    OP_LOAD,
    OP_STORE,
    OP_STORE32,
    OP_CMP,
    OP_MOV,
    OP_MOVI,
    OP_MEMSET,
    OP_MEMCPY,
    OP_IN,
    OP_OUT,
    OP_INT,
    OP_IRET,
    OP_MOD
};
void vm_dump(VM *vm, int mem_preview);
#endif