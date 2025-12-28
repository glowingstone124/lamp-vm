
//
// Created by Max Wang on 2025/12/28.
//

#ifndef VM_VM_H
#define VM_VM_H
#ifdef DEBUG
#define NEXT_INSTRUCTION (
{
    if (vm->ip >= vm->code_size) {
        fprintf(stderr, "IP out of range! \n");
        exit(1);
    }
    vm->code[vm->ip++];
}
)
#else
#define NEXT_INSTRUCTION vm->code[vm->ip++]
#endif
enum {
    OP_ADD = 1,
    OP_SUB,
    OP_MUL,
    OP_PRINT,
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
};
#endif