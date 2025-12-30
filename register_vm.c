#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fetch.h"
#include "vm.h"
#include "io_devices/frame/frame.h"
#include "stack.h"
#include "instruction.h"
#include "io.h"
#include "panic.h"
#include "loadbin.h"
#include "interrupt.h"
#include "io_devices/disk/disk.h"

const size_t MEM_SIZE = 1048576; //4MB

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
            const int address = rs1 + imm;
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
            const int address = vm->regs[rs1] + imm;
            if (address >= 0 && address < MEM_SIZE) {
                vm->regs[rd] = vm->memory[address];
                set_zf(vm, vm->regs[rd]);
            } else {
                panic(panic_format("LOAD_IND out of bounds: %d\n", address), vm);
            }
            break;
        }
        case OP_STORE: {
            const int address = rs1 + imm;
            if (address >= 0 && address < MEM_SIZE) {
                vm->memory[address] = vm->regs[rd];
            } else {
                panic(panic_format("STORE out of bounds: %d\n", address), vm);
            }
            break;
        }
        case OP_STORE_IND: {
            const int address = vm->regs[rs1] + imm;
            if (address >= 0 && address < MEM_SIZE) {
                vm->memory[address] = vm->regs[rd];
            } else {
                panic(panic_format("STORE_IND out of bounds: %d\n", address), vm);
            }
            break;
        }
        case OP_CMP: {
            const int val1 = vm->regs[rd];
            const int val2 = (imm != 0) ? imm : vm->regs[rs1];
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
            const int base = vm->regs[rd];
            const int value = vm->regs[rs1];
            for (int i = 0; i < imm; i++) {
                const int addr = base + i;
                if (addr >= 0 && addr < MEM_SIZE) {
                    vm->memory[addr] = value;
                } else {
                    panic(panic_format("MEMSET out of bounds; %d\n", addr), vm);
                }
            }
            break;
        }
        case OP_MEMCPY: {
            const int dest = vm->regs[rd];
            const int src = vm->regs[rs1];
            for (int i = 0; i < imm; i++) {
                const int daddr = dest + i;
                const int saddr = src + i;
                if (daddr >= 0 && daddr < MEM_SIZE && saddr >= 0 && saddr < MEM_SIZE) {
                    vm->memory[daddr] = vm->memory[saddr];
                } else {
                    panic(panic_format("MEMCPY out of bounds: d=%d, s=%d\n", daddr, saddr), vm);
                }
            }
            break;
        }
        case OP_IN: {
            const int addr = rs1;
            if (addr >= 0 && addr < IO_SIZE) {
                vm->regs[rd] = vm->io[addr];
            } else {
                panic(panic_format("IN invalid IO address %d", addr), vm);
            }
            break;
        }
        case OP_OUT: {
            const int addr = rs1;
            if (addr >= 0 && addr < IO_SIZE) {
                accept_io(vm, addr, vm->regs[rd]);
            } else {
                panic(panic_format("OUT invalid IO address %d\n", addr), vm);
            }
            break;
        }
        case OP_INT: {
            const int int_no = rd;
            if (int_no < 0 || int_no >= IVT_SIZE) break;

            const int isr_ip = vm->memory[IVT_BASE + int_no];
            if (isr_ip >= 0) {
                CALL_PUSH(vm, vm->ip);
                vm->ip = isr_ip;
                vm->in_interrupt = 1;
            }
            break;
        }

        case OP_IRET: {
            vm->ip = CALL_POP(vm);
            vm->in_interrupt = 0;
            break;
        }
        default: {
            panic(panic_format("Unknown opcode %d\n", op), vm);
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
        vm_handle_interrupts(vm);
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
    printf("IP = %lu\n", vm->ip);
    printf("ZF = %d\n", (vm->flags & FLAG_ZF) != 0);
}

VM *vm_create(size_t memory_size, uint64_t* program, size_t program_size) {
    VM *vm = malloc(sizeof(VM));
    if (!vm) return NULL;

    vm->ip = 0;
    vm->execution_times = 0;
    vm->code = program;
    vm->code_size = program_size;
    vm->halted = 0;
    vm->panic = 0;
    vm->flags = 0;
    vm->dsp = 0;
    vm->csp = 0;
    vm->in_interrupt = 0;
    vm->memory_size = memory_size;

    vm->memory = malloc(memory_size * sizeof(int));
    if (!vm->memory) {
        free(vm);
        return NULL;
    }
    memset(vm->memory, 0, memory_size * sizeof(int));

    return vm;
}


void vm_destroy(VM *vm) {
    if (!vm) return;
    if (vm->memory) free(vm->memory);
    free(vm);
}

int main() {
    /*
     *Disk
     */
    uint64_t program[] = {
        INST(OP_MOVI, 0, 0, 0, 0),
        INST(OP_MOVI, 1, 0, 0, 0),
        INST(OP_MOVI, 3, 0, 0, 28),

        INST(OP_MOVI, 2, 0, 0, 'H'),
        INST(OP_STORE_IND, 2, 0, 1, 0),
        INST(OP_MOVI, 2, 0, 0, 'e'),
        INST(OP_STORE_IND, 2, 0, 1, 1),
        INST(OP_MOVI, 2, 0, 0, 'l'),
        INST(OP_STORE_IND, 2, 0, 1, 2),
        INST(OP_MOVI, 2, 0, 0, 'l'),
        INST(OP_STORE_IND, 2, 0, 1, 3),
        INST(OP_MOVI, 2, 0, 0, 'o'),
        INST(OP_STORE_IND, 2, 0, 1, 4),
        INST(OP_MOVI, 2, 0, 0, ' '),
        INST(OP_STORE_IND, 2, 0, 1, 5),
        INST(OP_MOVI, 2, 0, 0, 'W'),
        INST(OP_STORE_IND, 2, 0, 1, 6),
        INST(OP_MOVI, 2, 0, 0, 'o'),
        INST(OP_STORE_IND, 2, 0, 1, 7),
        INST(OP_MOVI, 2, 0, 0, 'r'),
        INST(OP_STORE_IND, 2, 0, 1, 8),
        INST(OP_MOVI, 2, 0, 0, 'l'),
        INST(OP_STORE_IND, 2, 0, 1, 9),
        INST(OP_MOVI, 2, 0, 0, 'd'),
        INST(OP_STORE_IND, 2, 0, 1, 10),
        INST(OP_MOVI, 2, 0, 0, '!'),
        INST(OP_STORE_IND, 2, 0, 1, 11),
        INST(OP_MOVI, 2, 0, 0, ' '),
        INST(OP_STORE_IND, 2, 0, 1, 12),
        INST(OP_MOVI, 2, 0, 0, 'T'),
        INST(OP_STORE_IND, 2, 0, 1, 13),
        INST(OP_MOVI, 2, 0, 0, 'h'),
        INST(OP_STORE_IND, 2, 0, 1, 14),
        INST(OP_MOVI, 2, 0, 0, 'i'),
        INST(OP_STORE_IND, 2, 0, 1, 15),
        INST(OP_MOVI, 2, 0, 0, 's'),
        INST(OP_STORE_IND, 2, 0, 1, 16),
        INST(OP_MOVI, 2, 0, 0, ' '),
        INST(OP_STORE_IND, 2, 0, 1, 17),
        INST(OP_MOVI, 2, 0, 0, 'i'),
        INST(OP_STORE_IND, 2, 0, 1, 18),
        INST(OP_MOVI, 2, 0, 0, 's'),
        INST(OP_STORE_IND, 2, 0, 1, 19),
        INST(OP_MOVI, 2, 0, 0, ' '),
        INST(OP_STORE_IND, 2, 0, 1, 20),
        INST(OP_MOVI, 2, 0, 0, 'V'),
        INST(OP_STORE_IND, 2, 0, 1, 21),
        INST(OP_MOVI, 2, 0, 0, 'M'),
        INST(OP_STORE_IND, 2, 0, 1, 22),
        INST(OP_MOVI, 2, 0, 0, ' '),
        INST(OP_STORE_IND, 2, 0, 1, 23),
        INST(OP_MOVI, 2, 0, 0, 't'),
        INST(OP_STORE_IND, 2, 0, 1, 24),
        INST(OP_MOVI, 2, 0, 0, 'e'),
        INST(OP_STORE_IND, 2, 0, 1, 25),
        INST(OP_MOVI, 2, 0, 0, 's'),
        INST(OP_STORE_IND, 2, 0, 1, 26),
        INST(OP_MOVI, 2, 0, 0, 't'),
        INST(OP_STORE_IND, 2, 0, 1, 27),
        INST(OP_MOVI, 0, 0, 0, 0),
        INST(OP_OUT, 0, DISK_MEM, 0, 0),
        INST(OP_MOVI, 0, 0, 0, 0),
        INST(OP_OUT, 0, DISK_LBA, 0, 0),
        INST(OP_MOVI, 0, 0, 0, 1),
        INST(OP_OUT, 0, DISK_COUNT, 0, 0),

        INST(OP_MOVI, 0, 0, 0, DISK_CMD_WRITE),
        INST(OP_OUT, 0, DISK_CMD, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0)
    };
    size_t program_size = sizeof(program) / sizeof(program[0]);
    VM *vm = vm_create(MEM_SIZE, program, program_size);
    disk_init(vm, "./disk.img");
    init_ivt(vm);
    printf(
        "Loaded VM. \n code length: %lu\n Call Stack size: %d\n Data Stack size: %d \n Memory Size: %lu\n Memory Head: %p\n",
        vm->code_size,
        CALL_STACK_SIZE,DATA_STACK_SIZE,MEM_SIZE, (void*)vm->memory);

    for (int i = 0; i < 5; i++) {
        vm->memory[i] = i + 1;
    }
    init_screen();
    vm_run(vm);
    //vm_dump(vm, 16);
    printf("Execution complete in %lu cycles.\n", vm->execution_times);
    vm_destroy(vm);
    return 0;
}
