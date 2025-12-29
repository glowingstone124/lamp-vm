#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "fetch.h"
#include "vm.h"
#include "frame.h"
#include "stack.h"
#include "instruction.h"
#include "io.h"
#include "panic.h"
#include "loadbin.h"

void set_zf(VM *vm, int value) {
    if (value == 0) {
        vm->flags |= FLAG_ZF;
    } else {
        vm->flags &= ~FLAG_ZF;
    }
}

void vm_instruction_case(VM *vm) {
    uint8_t op, rd, rs1, rs2;
    int32_t imm;
    FETCH64(vm, op, rd, rs1, rs2, imm);
    vm->execution_times++;
    switch (op) {
        case OP_ADD: {
            vm->regs[rd] = vm->regs[rs1] + vm->regs[rs2];
            set_zf(vm, vm->regs[rd]);
            break;
        }

        case OP_SUB: {
            vm->regs[rd] = vm->regs[rs1] - vm->regs[rs2];
            set_zf(vm, vm->regs[rd]);
            break;
        }
        case OP_MUL: {
            vm->regs[rd] = vm->regs[rs1] * vm->regs[rs2];
            set_zf(vm, vm->regs[rd]);
            break;
        }
        case OP_HALT: {
            vm->halted = 1;
            return;
        }
        case OP_JMP: {
            vm->ip = imm;
            break;
        }
        case OP_JZ: {
            if (vm->flags & FLAG_ZF) {
                vm->ip = imm;
            }
            break;
        }
        case OP_PUSH: {
            DATA_PUSH(vm, vm->regs[rd]);
            break;
        }
        case OP_POP: {
            vm->regs[rd] = DATA_POP(vm);
            set_zf(vm, vm->regs[rd]);
            break;
        }
        case OP_CALL: {
            CALL_PUSH(vm, vm->ip);
            vm->ip = imm;
            break;
        }
        case OP_RET: {
            vm->ip = CALL_POP(vm);
            break;
        }
        case OP_LOAD: {
            int address = rs1 + imm;
            if (address >= 0 && address < MEM_SIZE) {
                vm->regs[rd] = vm->memory[address];
                set_zf(vm, vm->regs[rd]);
            } else {
                panic(
                    panic_format("LOAD out of bounds: %d\n", address),
                    vm
                );
            }
            break;
        }
        case OP_LOAD_IND: {
            int address = vm->regs[rs1] + imm;
            if (address >= 0 && address < MEM_SIZE) {
                vm->regs[rd] = vm->memory[address];
                set_zf(vm, vm->regs[rd]);
            } else {
                panic(panic_format("LOAD_IND out of bounds: %d\n", address),vm);
            }
            break;
        }
        case OP_STORE: {
            int address = rs1 + imm;
            if (address >= 0 && address < MEM_SIZE) {
                vm->memory[address] = vm->regs[rd];
            } else {
                panic(panic_format("STORE out of bounds: %d\n", address),vm);
            }
            break;
        }
        case OP_STORE_IND: {
            int address = vm->regs[rs1] + imm;
            if (address >= 0 && address < MEM_SIZE) {
                vm->memory[address] = vm->regs[rd];
            } else {
                panic(panic_format("STORE_IND out of bounds: %d\n", address),vm);
            }
            break;
        }
        case OP_CMP: {
            int val1 = vm->regs[rd];
            int val2 = (imm != 0) ? imm : vm->regs[rs1];
            set_zf(vm, val1 - val2);
            break;
        }
        case OP_MOV: {
            vm->regs[rd] = vm->regs[rs1];
            set_zf(vm, vm->regs[rd]);
            break;
        }
        case OP_MOVI: {
            vm->regs[rd] = imm;
            set_zf(vm, vm->regs[rd]);
            break;
        }
        case OP_MEMSET: {
            int base = vm->regs[rd];
            int value = vm->regs[rs1];
            for (int i = 0; i < imm; i++) {
                int addr = base + i;
                if (addr >= 0 && addr < MEM_SIZE) {
                    vm->memory[addr] = value;
                } else {
                    panic(panic_format("MEMSET out of bounds; %d\n", addr),vm);
                }
            }
            break;
        }
        case OP_MEMCPY: {
            int dest = vm->regs[rd];
            int src = vm->regs[rs1];
            for (int i = 0; i < imm; i++) {
                int daddr = dest + i;
                int saddr = src + i;
                if (daddr >= 0 && daddr < MEM_SIZE && saddr >= 0 && saddr < MEM_SIZE) {
                    vm->memory[daddr] = vm->memory[saddr];
                } else {
                    panic(panic_format("MEMCPY out of bounds: d=%d, s=%d\n", daddr, saddr),vm);
                }
            }
            break;
        }
        case OP_IN: {
            int addr = rs1;
            if (addr >= 0 && addr < IO_SIZE) {
                vm->regs[rd] = vm->io[addr];
            } else {
                panic(panic_format("IN invalid IO address %d", addr),vm);
            }
            break;
        }
        case OP_OUT: {
            int addr = rs1;
            if (addr >= 0 && addr < IO_SIZE) {
                accept_io(vm, addr, vm->regs[rd]);
            } else {
                panic(panic_format("OUT invalid IO address %d\n", addr),vm);
            }
            break;
        }

        default: {
            panic(panic_format("Unknown opcode %d\n", op),vm);
            return;
        }
    }
}

void vm_run(VM *vm) {
    while (!vm->halted && vm->ip < vm->code_size) {
        if (vm->panic) {
            printf("VM panic detected.\n");
            return;
        }
        vm_instruction_case(vm);
    }
}

void vm_dump(VM *vm, int mem_preview) {
    printf("VM dump:\n");
    printf("Registers:\n");
    for (int i = 0; i < REG_COUNT; i++) {
        printf("r%d = %d\n", i, vm->regs[i]);
    }

    printf("Call Stack (top -> bottom):\n");
    for (int i = vm->csp; i < CALL_STACK_SIZE; i++) {
        printf("[%d] = %d\n", i, vm->call_stack[i]);
    }
    if (vm->csp == CALL_STACK_SIZE) {
        printf("<empty>\n");
    }
    printf("Data Stack (top -> bottom):\n");
    for (int i = vm->dsp; i < DATA_STACK_SIZE; i++) {
        printf("[%d] = %d\n", i, vm->data_stack[i]);
    }
    if (vm->dsp == DATA_STACK_SIZE) {
        printf("<empty>\n");
    }
    printf("Memory (first %d cells):\n", mem_preview);
    for (int i = 0; i < mem_preview && i < MEM_SIZE; i++) {
        printf("[%d] = %d\n", i, vm->memory[i]);
    }
    printf("IP = %d\n", vm->ip);
    printf("ZF = %d\n", (vm->flags & FLAG_ZF) != 0);
}

int main() {
    /*
     * r0 = 0
     * r1 = 0
     * :loop_start
     * cmp r1, 5
     * if r1 == 5 jmp end
     * r2 = memory[r1]
     * r0 += r2
     * r3 = 1
     * r1 += 1
     * jmp loop_start
     * :end
     * print r0
     * halt
     */
    uint64_t program[] = {
        INST(OP_MOVI, 0, 0, 0, 0x07),
        INST(OP_OUT, 0, SCREEN_ATTRIBUTE, 0, 0),
        INST(OP_MOVI, 0, 0, 0, 'H'),
        INST(OP_OUT, 0, SCREEN, 0, 0),
        INST(OP_MOVI, 0, 0, 0, 'e'),
        INST(OP_OUT, 0, SCREEN, 0, 0),
        INST(OP_MOVI, 0, 0, 0, 'l'),
        INST(OP_OUT, 0, SCREEN, 0, 0),
        INST(OP_MOVI, 0, 0, 0, 'l'),
        INST(OP_OUT, 0, SCREEN, 0, 0),
        INST(OP_MOVI, 0, 0, 0, 'o'),
        INST(OP_OUT, 0, SCREEN, 0, 0),
        INST(OP_MOVI, 0, 0, 0, ' '),
        INST(OP_OUT, 0, SCREEN, 0, 0),
        INST(OP_MOVI, 0, 0, 0, 'W'),
        INST(OP_OUT, 0, SCREEN, 0, 0),
        INST(OP_MOVI, 0, 0, 0, 'o'),
        INST(OP_OUT, 0, SCREEN, 0, 0),
        INST(OP_MOVI, 0, 0, 0, 'r'),
        INST(OP_OUT, 0, SCREEN, 0, 0),
        INST(OP_MOVI, 0, 0, 0, 'l'),
        INST(OP_OUT, 0, SCREEN, 0, 0),
        INST(OP_MOVI, 0, 0, 0, 'd'),
        INST(OP_OUT, 0, SCREEN, 0, 0),
        INST(OP_MOVI, 0, 0, 0, '\n'),
        INST(OP_OUT, 0, SCREEN, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0)
    };


    VM vm = {0};
    vm.code = program;
    vm.code_size = sizeof(program) / sizeof(program[0]);
    vm.ip = 0;
    vm.flags = 0;
    vm.dsp = DATA_STACK_SIZE;
    vm.csp = CALL_STACK_SIZE;
    vm.execution_times = 0;
    printf(
        "Loaded VM. \n code length: %d\n Call Stack size: %d\n Data Stack size: %d \n Memory Size: %d\n Memory Head: %p\n",
        vm.code_size,
        CALL_STACK_SIZE,DATA_STACK_SIZE, MEM_SIZE, &vm.memory[0]);

    for (int i = 0; i < 5; i++) {
        vm.memory[i] = i + 1;
    }
    init_screen();
    vm_run(&vm);
    //vm_dump(&vm, 16);
    printf("Execution complete in %d cycles.\n", vm.execution_times);
    return 0;
}
