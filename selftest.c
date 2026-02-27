#include <stdint.h>
#include <stdio.h>

#include "selftest.h"
#include "vm.h"
#include "loadbin.h"
#include "interrupt.h"
#include "memory.h"
#include "io_devices/disk/disk.h"

extern const size_t MEM_SIZE;

extern int vm_run_headless(VM *vm, uint64_t timeout_ms);
extern VM *vm_create(size_t memory_size,
                     const uint64_t *program,
                     size_t program_size,
                     const uint8_t *data,
                     size_t data_size,
                     const ProgramLayout *layout,
                     int smp_cores);
extern void vm_destroy(VM *vm);

static int run_selftest_startap_cpuid(void) {
    const vm_addr_t flag_addr = 0x3000;
    const vm_addr_t ap_entry = PROGRAM_BASE + 11 * 8;
    uint64_t program[] = {
        /* BSP */
        INST(OP_MOVI, 1, 0, 0, 1),                    /* r1 = target core */
        INST(OP_MOVI, 2, 0, 0, ap_entry),             /* r2 = ap entry */
        INST(OP_STARTAP, 1, 2, 0, 0),                 /* start AP1 */
        INST(OP_MOVI, 4, 0, 0, flag_addr),            /* r4 = flag addr */
        INST(OP_LOAD32, 3, 4, 0, 0),                  /* r3 = *flag */
        INST(OP_CMPI, 3, 0, 0, 1),                    /* r3 == 1 ? */
        INST(OP_JNZ, 0, 0, 0, PROGRAM_BASE + 4 * 8),  /* loop */
        INST(OP_HALT, 0, 0, 0, 0),
        INST(OP_PAUSE, 0, 0, 0, 0),
        INST(OP_PAUSE, 0, 0, 0, 0),
        INST(OP_PAUSE, 0, 0, 0, 0),
        /* AP entry */
        INST(OP_CPUID, 5, 0, 0, 0),                   /* r5 = core_id */
        INST(OP_MOVI, 6, 0, 0, flag_addr),
        INST(OP_STORE32, 5, 6, 0, 0),                 /* *flag = core_id */
        INST(OP_PAUSE, 0, 0, 0, 0),
        INST(OP_JMP, 0, 0, 0, ap_entry + 3 * 8),
    };
    VM *vm = vm_create(MEM_SIZE, program, sizeof(program) / sizeof(program[0]), NULL, 0, NULL, 2);
    if (!vm)
        return 0;
    disk_init(vm, "./disk.img");
    init_ivt(vm);
    int ok = vm_run_headless(vm, 2000);
    uint32_t v = vm_read32(vm, flag_addr);
    ok = ok && (v == 1);
    vm_destroy(vm);
    return ok;
}

static int run_selftest_ipi(void) {
    const vm_addr_t ready_addr = 0x3010;
    const vm_addr_t ipi_addr = 0x3014;
    const vm_addr_t ap_entry = PROGRAM_BASE + 14 * 8;
    const vm_addr_t isr_entry = PROGRAM_BASE + 20 * 8;
    uint64_t program[] = {
        /* BSP */
        INST(OP_MOVI, 1, 0, 0, 1),
        INST(OP_MOVI, 2, 0, 0, ap_entry),
        INST(OP_STARTAP, 1, 2, 0, 0),
        INST(OP_MOVI, 10, 0, 0, ready_addr),
        INST(OP_LOAD32, 11, 10, 0, 0),
        INST(OP_CMPI, 11, 0, 0, 1),
        INST(OP_JNZ, 0, 0, 0, PROGRAM_BASE + 4 * 8),
        INST(OP_MOVI, 12, 0, 0, 5),                   /* vector=5 */
        INST(OP_IPI, 1, 12, 0, 0),                    /* send IPI to core1 */
        INST(OP_MOVI, 13, 0, 0, ipi_addr),
        INST(OP_LOAD32, 14, 13, 0, 0),
        INST(OP_CMPI, 14, 0, 0, 1),
        INST(OP_JNZ, 0, 0, 0, PROGRAM_BASE + 10 * 8),
        INST(OP_HALT, 0, 0, 0, 0),
        /* AP entry */
        INST(OP_MOVI, 6, 0, 0, ready_addr),
        INST(OP_MOVI, 7, 0, 0, 1),
        INST(OP_STORE32, 7, 6, 0, 0),
        INST(OP_PAUSE, 0, 0, 0, 0),
        INST(OP_JMP, 0, 0, 0, ap_entry + 3 * 8),
        INST(OP_PAUSE, 0, 0, 0, 0),
        /* ISR(vector=5) */
        INST(OP_MOVI, 8, 0, 0, ipi_addr),
        INST(OP_MOVI, 9, 0, 0, 1),
        INST(OP_STORE32, 9, 8, 0, 0),
        INST(OP_IRET, 0, 0, 0, 0),
    };

    VM *vm = vm_create(MEM_SIZE, program, sizeof(program) / sizeof(program[0]), NULL, 0, NULL, 2);
    if (!vm)
        return 0;
    disk_init(vm, "./disk.img");
    init_ivt(vm);
    register_isr(vm, 5, isr_entry);
    int ok = vm_run_headless(vm, 2500);
    uint32_t ipi = vm_read32(vm, ipi_addr);
    ok = ok && (ipi == 1);
    vm_destroy(vm);
    return ok;
}

static int run_selftest_relctrl(void) {
    const vm_addr_t flag_addr = 0x3020;
    uint64_t program[] = {
        INST(OP_MOVI, 10, 0, 0, flag_addr), /* r10 = flag addr */
        INST(OP_MOVI, 11, 0, 0, 0),         /* r11 = 0 */
        INST(OP_STORE32, 11, 10, 0, 0),     /* *flag = 0 */
        INST(OP_MOVI, 1, 0, 0, 0),          /* r1 = 0 */
        INST(OP_RJMP, 0, 0, 0, 16),         /* skip next insn */
        INST(OP_MOVI, 1, 0, 0, 111),        /* should not execute */
        INST(OP_RCALL, 0, 0, 0, 104),       /* call fn at idx 19 */
        INST(OP_CMPI, 1, 0, 0, 7),          /* ZF = 1 */
        INST(OP_RJZ, 0, 0, 0, 16),          /* go to idx 10 */
        INST(OP_RJMP, 0, 0, 0, 56),         /* fail */
        INST(OP_CMPI, 1, 0, 0, 8),          /* ZF = 0 */
        INST(OP_RJNZ, 0, 0, 0, 16),         /* go to idx 13 */
        INST(OP_RJMP, 0, 0, 0, 32),         /* fail */
        INST(OP_MOVI, 11, 0, 0, 1),         /* pass: *flag = 1 */
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
        INST(OP_MOVI, 11, 0, 0, 2),         /* fail: *flag = 2 */
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
        INST(OP_MOVI, 1, 0, 0, 7),          /* function body */
        INST(OP_RET, 0, 0, 0, 0),
    };

    VM *vm = vm_create(MEM_SIZE, program, sizeof(program) / sizeof(program[0]), NULL, 0, NULL, 1);
    if (!vm)
        return 0;
    disk_init(vm, "./disk.img");
    init_ivt(vm);
    int ok = vm_run_headless(vm, 1000);
    uint32_t flag = vm_read32(vm, flag_addr);
    ok = ok && (flag == 1);
    vm_destroy(vm);
    return ok;
}

static int run_selftest_zero_branch_flags(void) {
    const vm_addr_t flag_addr = 0x3024;
    const vm_addr_t fail_addr = PROGRAM_BASE + 16 * 8;
    uint64_t program[] = {
        INST(OP_MOVI, 10, 0, 0, flag_addr),  /* r10 = flag addr */
        INST(OP_MOVI, 11, 0, 0, 0),          /* *flag = 0 */
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_MOVI, 2, 0, 0, 123),         /* non-zero rs (must be ignored) */
        INST(OP_MOVI, 1, 0, 0, 7),
        INST(OP_CMPI, 1, 0, 0, 7),           /* ZF = 1 */
        INST(OP_RJZ, 2, 0, 0, 16),           /* go to idx 8 */
        INST(OP_JMP, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 2, 0, 0, 0),           /* zero rs (must be ignored) */
        INST(OP_MOVI, 1, 0, 0, 7),
        INST(OP_CMPI, 1, 0, 0, 8),           /* ZF = 0 */
        INST(OP_RJNZ, 2, 0, 0, 16),          /* go to idx 13 */
        INST(OP_JMP, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 11, 0, 0, 1),          /* pass: *flag = 1 */
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
        INST(OP_MOVI, 11, 0, 0, 2),          /* fail: *flag = 2 */
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
    };

    VM *vm = vm_create(MEM_SIZE, program, sizeof(program) / sizeof(program[0]), NULL, 0, NULL, 1);
    if (!vm)
        return 0;
    disk_init(vm, "./disk.img");
    init_ivt(vm);
    int ok = vm_run_headless(vm, 1000);
    uint32_t flag = vm_read32(vm, flag_addr);
    ok = ok && (flag == 1);
    vm_destroy(vm);
    return ok;
}

static int run_selftest_callr_unused_fields(void) {
    const vm_addr_t flag_addr = 0x3028;
    const vm_addr_t fail_addr = PROGRAM_BASE + 11 * 8;
    const vm_addr_t fn_addr = PROGRAM_BASE + 14 * 8;
    uint64_t program[] = {
        INST(OP_MOVI, 10, 0, 0, flag_addr),     /* r10 = flag addr */
        INST(OP_MOVI, 11, 0, 0, 0),             /* *flag = 0 */
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_MOVI, 20, 0, 0, fn_addr),       /* r20 = function address */
        INST(OP_MOVI, 1, 0, 0, 0),
        INST(OP_CALLR, 20, 7, 8, 123),          /* rs1/rs2/imm must be ignored */
        INST(OP_CMPI, 1, 0, 0, 42),             /* function result */
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 11, 0, 0, 1),             /* pass */
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
        INST(OP_MOVI, 11, 0, 0, 2),             /* fail */
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
        INST(OP_MOVI, 1, 0, 0, 42),             /* function body */
        INST(OP_RET, 0, 0, 0, 0),
    };

    VM *vm = vm_create(MEM_SIZE, program, sizeof(program) / sizeof(program[0]), NULL, 0, NULL, 1);
    if (!vm)
        return 0;
    disk_init(vm, "./disk.img");
    init_ivt(vm);
    int ok = vm_run_headless(vm, 1000);
    uint32_t flag = vm_read32(vm, flag_addr);
    ok = ok && (flag == 1);
    vm_destroy(vm);
    return ok;
}

static int run_selftest_atomic_conformance(void) {
    const vm_addr_t flag_addr = 0x3040;
    const vm_addr_t word_addr = 0x3044;
    const vm_addr_t fail_addr = PROGRAM_BASE + 46 * 8;
    uint64_t program[] = {
        INST(OP_MOVI, 10, 0, 0, flag_addr),     /* r10 = flag addr */
        INST(OP_MOVI, 11, 0, 0, 0),             /* *flag = 0 */
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_MOVI, 12, 0, 0, word_addr),     /* r12 = atomic word addr */
        INST(OP_MOVI, 1, 0, 0, 10),
        INST(OP_STLR, 1, 12, 77, 0),            /* rs2 ignored */
        INST(OP_LDAR, 2, 12, 88, 0),            /* rs2 ignored */
        INST(OP_CMPI, 2, 0, 0, 10),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 3, 0, 0, 7),              /* addend */
        INST(OP_MOVI, 4, 0, 0, 0),              /* old value result */
        INST(OP_XADD, 4, 12, 3, 0),             /* old=10, mem=17 */
        INST(OP_CMPI, 4, 0, 0, 10),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_LDAR, 5, 12, 0, 0),
        INST(OP_CMPI, 5, 0, 0, 17),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 6, 0, 0, 99),
        INST(OP_MOVI, 7, 0, 0, 0),
        INST(OP_XCHG, 7, 12, 6, 0),             /* old=17, mem=99 */
        INST(OP_CMPI, 7, 0, 0, 17),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_LDAR, 8, 12, 0, 0),
        INST(OP_CMPI, 8, 0, 0, 99),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 9, 0, 0, 99),             /* expected */
        INST(OP_MOVI, 13, 0, 0, 123),           /* desired */
        INST(OP_CAS, 9, 12, 13, 0),             /* success => ZF=1 */
        INST(OP_JNZ, 0, 0, 0, fail_addr),       /* fail if ZF=0 */
        INST(OP_CMPI, 9, 0, 0, 99),             /* old value */
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_LDAR, 14, 12, 0, 0),
        INST(OP_CMPI, 14, 0, 0, 123),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 15, 0, 0, 77),            /* wrong expected */
        INST(OP_MOVI, 16, 0, 0, 55),
        INST(OP_CAS, 15, 12, 16, 0),            /* failure => ZF=0 */
        INST(OP_JZ, 0, 0, 0, fail_addr),        /* fail if ZF=1 */
        INST(OP_CMPI, 15, 0, 0, 123),           /* observed old value */
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_LDAR, 17, 12, 0, 0),
        INST(OP_CMPI, 17, 0, 0, 123),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 11, 0, 0, 1),             /* pass */
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
        INST(OP_MOVI, 11, 0, 0, 2),             /* fail */
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
    };

    VM *vm = vm_create(MEM_SIZE, program, sizeof(program) / sizeof(program[0]), NULL, 0, NULL, 1);
    if (!vm)
        return 0;
    disk_init(vm, "./disk.img");
    init_ivt(vm);
    int ok = vm_run_headless(vm, 1000);
    uint32_t flag = vm_read32(vm, flag_addr);
    uint32_t word = vm_read32(vm, word_addr);
    ok = ok && (flag == 1) && (word == 123u);
    vm_destroy(vm);
    return ok;
}

static int run_selftest_div0_interrupt_conformance(void) {
    const vm_addr_t flag_addr = 0x3050;
    const vm_addr_t vector_addr = 0x3054;
    const vm_addr_t count_addr = 0x3058;
    const vm_addr_t fail_addr = PROGRAM_BASE + 19 * 8;
    const vm_addr_t isr_entry = PROGRAM_BASE + 22 * 8;
    uint64_t program[] = {
        INST(OP_MOVI, 10, 0, 0, flag_addr),              /* *flag = 0 */
        INST(OP_MOVI, 11, 0, 0, 0),
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_MOVI, 12, 0, 0, vector_addr),            /* *vector = 0 */
        INST(OP_STORE32, 11, 12, 0, 0),
        INST(OP_MOVI, 13, 0, 0, count_addr),             /* *count = 0 */
        INST(OP_STORE32, 11, 13, 0, 0),
        INST(OP_MOVI, 1, 0, 0, 42),
        INST(OP_MOVI, 2, 0, 0, 0),
        INST(OP_DIV, 3, 1, 2, 0),                        /* triggers INT_DIVIDE_BY_ZERO */
        INST(OP_LOAD32, 4, 12, 0, 0),                    /* vector captured by ISR */
        INST(OP_CMPI, 4, 0, 0, INT_DIVIDE_BY_ZERO),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_LOAD32, 5, 13, 0, 0),                    /* ISR count */
        INST(OP_CMPI, 5, 0, 0, 1),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 11, 0, 0, 1),                      /* pass */
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
        INST(OP_MOVI, 11, 0, 0, 2),                      /* fail */
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
        INST(OP_MOVI, 20, 0, 0, vector_addr),            /* ISR: store r31 */
        INST(OP_STORE32, 31, 20, 0, 0),
        INST(OP_MOVI, 21, 0, 0, count_addr),             /* ISR: count++ */
        INST(OP_LOAD32, 22, 21, 0, 0),
        INST(OP_INC, 22, 0, 0, 0),
        INST(OP_STORE32, 22, 21, 0, 0),
        INST(OP_IRET, 0, 0, 0, 0),
    };

    VM *vm = vm_create(MEM_SIZE, program, sizeof(program) / sizeof(program[0]), NULL, 0, NULL, 1);
    if (!vm)
        return 0;
    disk_init(vm, "./disk.img");
    init_ivt(vm);
    register_isr(vm, INT_DIVIDE_BY_ZERO, isr_entry);
    int ok = vm_run_headless(vm, 1000);
    uint32_t flag = vm_read32(vm, flag_addr);
    uint32_t vector = vm_read32(vm, vector_addr);
    uint32_t count = vm_read32(vm, count_addr);
    ok = ok && (flag == 1u) && (vector == (uint32_t)INT_DIVIDE_BY_ZERO) && (count == 1u);
    vm_destroy(vm);
    return ok;
}

static int run_selftest_load16_signext_conformance(void) {
    const vm_addr_t flag_addr = 0x3060;
    const vm_addr_t data_addr = 0x3068;
    const vm_addr_t fail_addr = PROGRAM_BASE + 25 * 8;
    uint64_t program[] = {
        INST(OP_MOVI, 10, 0, 0, flag_addr),
        INST(OP_MOVI, 11, 0, 0, 0),
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_MOVI, 12, 0, 0, data_addr),
        INST(OP_MOVI, 1, 0, 0, 0xABCD),
        INST(OP_STORE16, 1, 12, 0, 0),
        INST(OP_LOAD16, 2, 12, 0, 0),
        INST(OP_CMPI, 2, 0, 0, 0xABCD),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_LOAD, 3, 12, 0, 1),                 /* high byte should be 0xAB */
        INST(OP_CMPI, 3, 0, 0, 0xAB),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 4, 0, 0, 0x80),
        INST(OP_STORE, 4, 12, 0, 4),
        INST(OP_LOADS8, 5, 12, 0, 4),
        INST(OP_CMPI, 5, 0, 0, -128),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 6, 0, 0, 0x8001),
        INST(OP_STORE16, 6, 12, 0, 6),
        INST(OP_LOADS16, 7, 12, 0, 6),
        INST(OP_CMPI, 7, 0, 0, -32767),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 11, 0, 0, 1),
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
        INST(OP_MOVI, 11, 0, 0, 2),
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
    };

    VM *vm = vm_create(MEM_SIZE, program, sizeof(program) / sizeof(program[0]), NULL, 0, NULL, 1);
    if (!vm)
        return 0;
    disk_init(vm, "./disk.img");
    init_ivt(vm);
    int ok = vm_run_headless(vm, 1000);
    uint32_t flag = vm_read32(vm, flag_addr);
    uint32_t raw = vm_read32(vm, data_addr);
    ok = ok && (flag == 1u) && ((raw & 0xFFFFu) == 0xABCDu);
    vm_destroy(vm);
    return ok;
}

static int run_selftest_relcond_extended_conformance(void) {
    const vm_addr_t flag_addr = 0x3070;
    const vm_addr_t fail_addr = PROGRAM_BASE + 26 * 8;
    uint64_t program[] = {
        INST(OP_MOVI, 10, 0, 0, flag_addr),
        INST(OP_MOVI, 11, 0, 0, 0),
        INST(OP_STORE32, 11, 10, 0, 0),

        INST(OP_MOVI, 1, 0, 0, 5),
        INST(OP_CMPI, 1, 0, 0, 3),                   /* > */
        INST(OP_RJG, 0, 0, 0, 16),
        INST(OP_JMP, 0, 0, 0, fail_addr),

        INST(OP_CMPI, 1, 0, 0, 5),                   /* >= */
        INST(OP_RJGE, 0, 0, 0, 16),
        INST(OP_JMP, 0, 0, 0, fail_addr),

        INST(OP_CMPI, 1, 0, 0, 6),                   /* < */
        INST(OP_RJL, 0, 0, 0, 16),
        INST(OP_JMP, 0, 0, 0, fail_addr),

        INST(OP_CMPI, 1, 0, 0, 5),                   /* <= */
        INST(OP_RJLE, 0, 0, 0, 16),
        INST(OP_JMP, 0, 0, 0, fail_addr),

        INST(OP_CMPI, 1, 0, 0, 6),                   /* CF=1 */
        INST(OP_RJC, 0, 0, 0, 16),
        INST(OP_JMP, 0, 0, 0, fail_addr),

        INST(OP_MOVI, 1, 0, 0, 6),
        INST(OP_CMPI, 1, 0, 0, 5),                   /* CF=0 */
        INST(OP_RJNC, 0, 0, 0, 16),
        INST(OP_JMP, 0, 0, 0, fail_addr),

        INST(OP_MOVI, 11, 0, 0, 1),
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
        INST(OP_MOVI, 11, 0, 0, 2),
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
    };

    VM *vm = vm_create(MEM_SIZE, program, sizeof(program) / sizeof(program[0]), NULL, 0, NULL, 1);
    if (!vm)
        return 0;
    disk_init(vm, "./disk.img");
    init_ivt(vm);
    int ok = vm_run_headless(vm, 1000);
    uint32_t flag = vm_read32(vm, flag_addr);
    ok = ok && (flag == 1u);
    vm_destroy(vm);
    return ok;
}

static int run_selftest_inti_imm_conformance(void) {
    const vm_addr_t flag_addr = 0x3080;
    const vm_addr_t vector_addr = 0x3084;
    const vm_addr_t count_addr = 0x3088;
    const vm_addr_t fail_addr = PROGRAM_BASE + 17 * 8;
    const vm_addr_t isr_entry = PROGRAM_BASE + 20 * 8;
    uint64_t program[] = {
        INST(OP_MOVI, 10, 0, 0, flag_addr),
        INST(OP_MOVI, 11, 0, 0, 0),
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_MOVI, 12, 0, 0, vector_addr),
        INST(OP_STORE32, 11, 12, 0, 0),
        INST(OP_MOVI, 13, 0, 0, count_addr),
        INST(OP_STORE32, 11, 13, 0, 0),
        INST(OP_INTI, 7, 8, 9, 9),                   /* rd/rs1/rs2 ignored */
        INST(OP_LOAD32, 1, 12, 0, 0),
        INST(OP_CMPI, 1, 0, 0, 9),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_LOAD32, 2, 13, 0, 0),
        INST(OP_CMPI, 2, 0, 0, 1),
        INST(OP_JNZ, 0, 0, 0, fail_addr),
        INST(OP_MOVI, 11, 0, 0, 1),
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
        INST(OP_MOVI, 11, 0, 0, 2),
        INST(OP_STORE32, 11, 10, 0, 0),
        INST(OP_HALT, 0, 0, 0, 0),
        INST(OP_MOVI, 20, 0, 0, vector_addr),        /* ISR: record vector */
        INST(OP_STORE32, 31, 20, 0, 0),
        INST(OP_MOVI, 21, 0, 0, count_addr),
        INST(OP_LOAD32, 22, 21, 0, 0),
        INST(OP_INC, 22, 0, 0, 0),
        INST(OP_STORE32, 22, 21, 0, 0),
        INST(OP_IRET, 0, 0, 0, 0),
    };

    VM *vm = vm_create(MEM_SIZE, program, sizeof(program) / sizeof(program[0]), NULL, 0, NULL, 1);
    if (!vm)
        return 0;
    disk_init(vm, "./disk.img");
    init_ivt(vm);
    register_isr(vm, 9, isr_entry);
    int ok = vm_run_headless(vm, 1000);
    uint32_t flag = vm_read32(vm, flag_addr);
    uint32_t vector = vm_read32(vm, vector_addr);
    uint32_t count = vm_read32(vm, count_addr);
    ok = ok && (flag == 1u) && (vector == 9u) && (count == 1u);
    vm_destroy(vm);
    return ok;
}

int run_selftests(void) {
    int ok1 = run_selftest_startap_cpuid();
    int ok2 = run_selftest_ipi();
    int ok3 = run_selftest_relctrl();
    int ok4 = run_selftest_zero_branch_flags();
    int ok5 = run_selftest_callr_unused_fields();
    int ok6 = run_selftest_atomic_conformance();
    int ok7 = run_selftest_div0_interrupt_conformance();
    int ok8 = run_selftest_load16_signext_conformance();
    int ok9 = run_selftest_relcond_extended_conformance();
    int ok10 = run_selftest_inti_imm_conformance();

    printf("[selftest] startap_cpuid: %s\n", ok1 ? "PASS" : "FAIL");
    printf("[selftest] ipi: %s\n", ok2 ? "PASS" : "FAIL");
    printf("[selftest] relctrl: %s\n", ok3 ? "PASS" : "FAIL");
    printf("[selftest] zero_branch_flags: %s\n", ok4 ? "PASS" : "FAIL");
    printf("[selftest] callr_unused_fields: %s\n", ok5 ? "PASS" : "FAIL");
    printf("[selftest] atomic_conformance: %s\n", ok6 ? "PASS" : "FAIL");
    printf("[selftest] div0_interrupt_conformance: %s\n", ok7 ? "PASS" : "FAIL");
    printf("[selftest] load16_signext_conformance: %s\n", ok8 ? "PASS" : "FAIL");
    printf("[selftest] relcond_extended_conformance: %s\n", ok9 ? "PASS" : "FAIL");
    printf("[selftest] inti_imm_conformance: %s\n", ok10 ? "PASS" : "FAIL");
    return (ok1 && ok2 && ok3 && ok4 && ok5 && ok6 && ok7 && ok8 && ok9 && ok10) ? 0 : 1;
}
