
//
// Created by Max Wang on 2025/12/28.
//

#ifndef VM_VM_H
#define VM_VM_H
#include <stdint.h>
#include <stdio.h>

#define REG_COUNT 8
#define DUMP_MEM_SEEK_LEN 16
#define MEM_SIZE 1024
#define DATA_STACK_SIZE 256
#define CALL_STACK_SIZE 256
#define FLAG_ZF 1
#define IO_SIZE 256
#define IVT_BASE 0x0
#define IVT_SIZE 256
#define IVT_ENTRY_SIZE 1

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
    int ip;
    int execution_times;
    int code_size;
    int halted;
    int panic;
    unsigned int flags;

    int data_stack[DATA_STACK_SIZE];
    int dsp;

    int call_stack[CALL_STACK_SIZE];
    int csp;

    int memory[MEM_SIZE];

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
    OP_LOAD_IND,
    OP_STORE_IND,
    OP_CMP,
    OP_MOV,
    OP_MOVI,
    OP_MEMSET,
    OP_MEMCPY,
    OP_IN,
    OP_OUT,
    OP_INT,
    OP_IRET
};
void vm_dump(VM *vm, int mem_preview);
#endif