#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <SDL2/SDL_timer.h>
#include <pthread.h>

#include "fetch.h"
#include "vm.h"
#include "io_devices/frame/frame.h"
#include "stack.h"
#include "io.h"
#include "panic.h"
#include "loadbin.h"
#include "interrupt.h"
#include "memory.h"
#include "io_devices/disk/disk.h"
#include "io_devices/vga_display/display.h"

const size_t MEM_SIZE = 1048576 * 4; // 4MB

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
#ifdef DEBUG
    printf("IP=%lu, executing opcode=%d\n", vm->ip, op);
#endif

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
        flush_screen_final();
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
        data_push(vm, (uint8_t)vm->regs[rd]);
        break;
    }
    case OP_POP: {
        vm->regs[rd] = data_pop(vm);
        set_zf(vm, vm->regs[rd]);
        break;
    }

    case OP_MOD:
        if (vm->regs[rs2] != 0)
            vm->regs[rd] = vm->regs[rs1] % vm->regs[rs2];
        else
            vm->regs[rd] = 0;
        break;
    case OP_CALL: {
        call_push(vm, vm->ip);
        vm->ip = imm;
        break;
    }
    case OP_RET: {
        vm->ip = call_pop(vm);
        break;
    }
    case OP_LOAD: {
        const vm_addr_t addr = vm->regs[rs1] + imm;
        vm->regs[rd] = vm_read8(vm, addr);
        set_zf(vm, vm->regs[rd]);
        break;
    }
    case OP_STORE: {
        const vm_addr_t addr = vm->regs[rs1] + imm;
        vm_write8(vm, addr, (uint8_t)vm->regs[rd]);
        break;
    }
    case OP_STORE32: {
        const vm_addr_t addr = vm->regs[rs1] + vm->regs[rs2] + imm;
        vm_write32(vm, addr, (uint32_t)vm->regs[rd]);
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
        const uint32_t base = (uint32_t)vm->regs[rd];
        const uint8_t value = (uint8_t)vm->regs[rs1];
        const uint32_t count = (uint32_t)imm;

        for (uint32_t i = 0; i < count; i++) {
            vm_write8(vm, base + i, value);
        }
        break;
    }
    case OP_MEMCPY: {
        const uint32_t dest = (uint32_t)vm->regs[rd];
        const uint32_t src = (uint32_t)vm->regs[rs1];
        const uint32_t count = (uint32_t)imm;

        for (uint32_t i = 0; i < count; i++) {
            uint8_t v = vm_read8(vm, src + i);
            vm_write8(vm, dest + i, v);
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
        const uint32_t int_no = rd;
        if (int_no >= IVT_SIZE)
            break;

        const vm_addr_t ivt_entry = IVT_BASE + int_no * 8;
        const uint64_t isr_ip = vm_read64(vm, ivt_entry);

        call_push(vm, vm->ip);
        vm->ip = isr_ip;
        vm->in_interrupt = 1;
        break;
    }

    case OP_IRET: {
        vm->ip = call_pop(vm);
        vm->in_interrupt = 0;
        break;
    }
    default: {
        panic(panic_format("Unknown opcode %d\n", op), vm);
        return;
    }
    }
}

void *vm_thread(void *arg) {
    VM *vm = arg;
    while (!vm->halted) {
        if (vm->panic)
            return NULL;
        vm_handle_interrupts(vm);
        vm_instruction_case(vm);
        vm_handle_keyboard(vm);
    }
    return NULL;
}

void display_loop(VM *vm) {
    vga_display_init();
    const int frame_delay = 16; // ~60FPS
    while (!vm->halted) {
        display_poll_events(vm);
        display_update(vm);
        SDL_Delay(frame_delay);
    }
    display_shutdown();
}

void vm_run(VM *vm) {
    enable_raw_mode();

    pthread_t thread_id;
    pthread_create(&thread_id, NULL, vm_thread, vm);

    display_loop(vm);

    pthread_join(thread_id, NULL);
    disable_raw_mode();
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

VM *vm_create(size_t memory_size, const uint64_t *program, size_t program_size) {
    VM *vm = malloc(sizeof(VM));
    if (!vm)
        return NULL;

    memset(vm, 0, sizeof(VM));

    vm->memory_size = memory_size;
    vm->memory = malloc(memory_size);
    if (!vm->memory) {
        free(vm);
        return NULL;
    }

    memset(vm->memory, 0, memory_size);

    if (memory_size < FB_SIZE) {
        panic("Not enough memory for framebuffer\n", vm);
        return vm;
    }

    size_t fb_base = FB_BASE(memory_size);
    if (fb_base + FB_SIZE > memory_size) {
        panic("Framebuffer out of range\n", vm);
        return vm;
    }

    vm->fb = malloc(FB_SIZE);
    printf("vm->fb = %p\n", (void *)vm->fb);
    printf("fb_base = 0x%zx\n", fb_base);
    printf("fb address mod 4 = %zu\n", ((size_t)vm->fb) % 4);

    size_t prog_bytes = program_size * sizeof(uint64_t);
    if (PROGRAM_BASE + prog_bytes > memory_size) {
        panic("Program too large\n", vm);
        return vm;
    }

    memcpy(vm->memory + PROGRAM_BASE, program, prog_bytes);

    vm->ip = PROGRAM_BASE;
    vm->csp = CALL_STACK_SIZE;
    vm->dsp = DATA_STACK_SIZE;

    return vm;
}

void vm_destroy(VM *vm) {
    if (!vm)
        return;
    if (vm->memory)
        free(vm->memory);
    free(vm);
}

int main() {
    /*

     */
    uint64_t program[] = {INST(OP_MOVI, 0, 0, 0, FB_BASE(MEM_SIZE)),
                          INST(OP_MOVI, 1, 0, 0, 0x474A43),
                          INST(OP_MOVI, 2, 0, 0, 0),
                          INST(OP_MOVI, 7, 0, 0, 4),
                          // LOOP_START (index 4)
                          INST(OP_STORE32, 1, 0, 2, 0),
                          INST(OP_ADD, 2, 2, 7, 0),
                          INST(OP_CMP, 2, 0, 0, FB_SIZE),
                          INST(OP_JZ, 0, 0, 0, PROGRAM_BASE + 9 * 8),
                          INST(OP_JMP, 0, 0, 0, PROGRAM_BASE + 4 * 8),

                          // HALT (index 8)
                          INST(OP_HALT, 0, 0, 0, 0)};

    size_t program_size = sizeof(program) / sizeof(program[0]);
    /*
    const char* filename = "program";
    size_t program_size = 0;
    uint64_t* program = load_program(filename, &program_size);
    */
    VM *vm = vm_create(MEM_SIZE, program, program_size);
    disk_init(vm, "./disk.img");
    init_ivt(vm);
    printf("Loaded VM. \n Call Stack size: %d\n Data Stack size: %d \n Memory Size: %lu\n Memory "
           "Head: %p\n",
           CALL_STACK_SIZE,
           DATA_STACK_SIZE,
           MEM_SIZE,
           (void *)vm->memory);
    init_screen();
    vm_run(vm);
#ifdef DBEUG
    vm_dump(vm, 1024);
#endif

    flush_screen_final();
    printf("Execution complete in %lu cycles.\n", vm->execution_times);
    vm_destroy(vm);
    return 0;
}
