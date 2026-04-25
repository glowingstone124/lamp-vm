//
// Created by Codex on 2026/01/28.
//
#include "debug.h"

#ifdef VM_DEBUG

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "memory.h"
#include "panic.h"
#include "vm.h"

typedef struct VM_Debug {
#ifdef VM_INSTR_STATS
    uint64_t instr_counts[256];
#endif
    uint32_t breakpoints[VM_DEBUG_MAX_BREAKPOINTS];
    size_t breakpoint_count;
    int step_mode;
    int pause_on_start;
} VM_Debug;

static const char *op_name(uint8_t op) {
    static const char *names[256] = {
        [OP_ADD] = "ADD",
        [OP_SUB] = "SUB",
        [OP_MUL] = "MUL",
        [OP_DIV] = "DIV",
        [OP_HALT] = "HALT",
        [OP_JMP] = "JMP",
        [OP_JZ] = "JZ",
        [OP_PUSH] = "PUSH",
        [OP_POP] = "POP",
        [OP_CALL] = "CALL",
        [OP_RET] = "RET",
        [OP_LOAD] = "LOAD",
        [OP_LOAD32] = "LOAD32",
        [OP_LOADX32] = "LOADX32",
        [OP_STORE] = "STORE",
        [OP_STORE32] = "STORE32",
        [OP_STOREX32] = "STOREX32",
        [OP_CMP] = "CMP",
        [OP_CMPI] = "CMPI",
        [OP_MOV] = "MOV",
        [OP_MOVI] = "MOVI",
        [OP_MEMSET] = "MEMSET",
        [OP_MEMCPY] = "MEMCPY",
        [OP_IN] = "IN",
        [OP_OUT] = "OUT",
        [OP_INT] = "INT",
        [OP_IRET] = "IRET",
        [OP_MOD] = "MOD",
        [OP_AND] = "AND",
        [OP_OR] = "OR",
        [OP_XOR] = "XOR",
        [OP_NOT] = "NOT",
        [OP_SHL] = "SHL",
        [OP_SHR] = "SHR",
        [OP_SAR] = "SAR",
        [OP_JNZ] = "JNZ",
        [OP_JG] = "JG",
        [OP_JGE] = "JGE",
        [OP_JL] = "JL",
        [OP_JLE] = "JLE",
        [OP_JC] = "JC",
        [OP_JNC] = "JNC",
        [OP_FADD] = "FADD",
        [OP_FSUB] = "FSUB",
        [OP_FMUL] = "FMUL",
        [OP_FDIV] = "FDIV",
        [OP_FNEG] = "FNEG",
        [OP_FABS] = "FABS",
        [OP_FSQRT] = "FSQRT",
        [OP_FCMP] = "FCMP",
        [OP_ITOF] = "ITOF",
        [OP_FTOI] = "FTOI",
        [OP_FLOAD32] = "FLOAD32",
        [OP_FSTORE32] = "FSTORE32",
        [OP_INC] = "INC",
        [OP_CAS] = "CAS",
        [OP_XADD] = "XADD",
        [OP_XCHG] = "XCHG",
        [OP_LDAR] = "LDAR",
        [OP_STLR] = "STLR",
        [OP_FENCE] = "FENCE",
        [OP_PAUSE] = "PAUSE",
        [OP_STARTAP] = "STARTAP",
        [OP_IPI] = "IPI",
        [OP_CPUID] = "CPUID",
        [OP_CALLR] = "CALLR",
        [OP_RJMP] = "RJMP",
        [OP_RCALL] = "RCALL",
        [OP_RJZ] = "RJZ",
        [OP_RJNZ] = "RJNZ",
        [OP_LOADX] = "LOADX",
        [OP_LOADX16] = "LOADX16",
        [OP_STOREX] = "STOREX",
        [OP_STOREX16] = "STOREX16",
    };
    return names[op] ? names[op] : "UNKNOWN";
}

static int parse_u32(const char *s, uint32_t *out) {
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (s == end)
        return 0;
    while (*end && isspace((unsigned char)*end))
        end++;
    if (*end != '\0' && *end != ',' && *end != ';')
        return 0;
    *out = (uint32_t)v;
    return 1;
}

static void add_breakpoint(VM_Debug *dbg, uint32_t addr) {
    for (size_t i = 0; i < dbg->breakpoint_count; i++) {
        if (dbg->breakpoints[i] == addr)
            return;
    }
    if (dbg->breakpoint_count >= VM_DEBUG_MAX_BREAKPOINTS) {
        printf("[debug] breakpoint list full (max %d)\n", VM_DEBUG_MAX_BREAKPOINTS);
        return;
    }
    dbg->breakpoints[dbg->breakpoint_count++] = addr;
}

static void remove_breakpoint(VM_Debug *dbg, uint32_t addr) {
    for (size_t i = 0; i < dbg->breakpoint_count; i++) {
        if (dbg->breakpoints[i] == addr) {
            dbg->breakpoints[i] = dbg->breakpoints[dbg->breakpoint_count - 1];
            dbg->breakpoint_count--;
            return;
        }
    }
}

static int has_breakpoint(VM_Debug *dbg, uint32_t addr) {
    for (size_t i = 0; i < dbg->breakpoint_count; i++) {
        if (dbg->breakpoints[i] == addr)
            return 1;
    }
    return 0;
}

static void decode_at(VM *vm, uint32_t ip, uint8_t *op, uint8_t *rd, uint8_t *rs1, uint8_t *rs2, int32_t *imm) {
    if (ip + 8 > vm->memory_size) {
        *op = 0;
        *rd = 0;
        *rs1 = 0;
        *rs2 = 0;
        *imm = 0;
        return;
    }
    uint64_t inst = vm_read64(vm, ip);
    *op = (inst >> 56) & 0xFF;
    *rd = (inst >> 48) & 0xFF;
    *rs1 = (inst >> 40) & 0xFF;
    *rs2 = (inst >> 32) & 0xFF;
    *imm = (int32_t)(inst & 0xFFFFFFFF);
}

static void dump_bytes(VM *vm, uint32_t addr, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        if (i % 16 == 0)
            printf("\n0x%08x: ", addr + i);
        printf("%02x ", vm_read8(vm, addr + i));
    }
    printf("\n");
}

static int parse_command_line(char *line, char **cmd, char **arg1, char **arg2) {
    char *p = line;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (!*p)
        return 0;
    *cmd = p;
    while (*p && !isspace((unsigned char)*p))
        p++;
    if (*p)
        *p++ = '\0';
    while (*p && isspace((unsigned char)*p))
        p++;
    *arg1 = (*p) ? p : NULL;
    if (!*p)
        return 1;
    while (*p && !isspace((unsigned char)*p))
        p++;
    if (*p)
        *p++ = '\0';
    while (*p && isspace((unsigned char)*p))
        p++;
    *arg2 = (*p) ? p : NULL;
    if (*arg2) {
        while (*p && !isspace((unsigned char)*p))
            p++;
        *p = '\0';
    }
    return 1;
}

static void print_help(void) {
    printf("[debug] commands: s(step), c(continue), r(regs), m <addr> <len>, b <addr>, d <addr>, l(list), q(quit)\n");
}

static void interactive_wait(VM *vm) {
    VM_Debug *dbg = vm->debug;
    VCPU *cpu = vm_current_cpu(vm);
    if (!cpu)
        return;
    char line[256];
    uint8_t op = 0, rd = 0, rs1 = 0, rs2 = 0;
    int32_t imm = 0;
    decode_at(vm, (uint32_t)cpu->ip, &op, &rd, &rs1, &rs2, &imm);
    printf("\n[debug] pause at IP=0x%08x op=%s rd=%u rs1=%u rs2=%u imm=%d\n",
           (uint32_t)cpu->ip, op_name(op), rd, rs1, rs2, imm);
    print_help();

    while (fgets(line, sizeof(line), stdin)) {
        char *cmd = NULL;
        char *arg1 = NULL;
        char *arg2 = NULL;
        if (!parse_command_line(line, &cmd, &arg1, &arg2)) {
            dbg->step_mode = 1;
            return;
        }

        if (strcmp(cmd, "s") == 0) {
            dbg->step_mode = 1;
            return;
        }
        if (strcmp(cmd, "c") == 0) {
            dbg->step_mode = 0;
            return;
        }
        if (strcmp(cmd, "r") == 0) {
            vm_dump(vm, 0);
            continue;
        }
        if (strcmp(cmd, "m") == 0) {
            if (!arg1 || !arg2) {
                printf("[debug] usage: m <addr> <len>\n");
                continue;
            }
            uint32_t addr = 0;
            uint32_t len = 0;
            if (!parse_u32(arg1, &addr) || !parse_u32(arg2, &len)) {
                printf("[debug] invalid address/length\n");
                continue;
            }
            dump_bytes(vm, addr, len);
            continue;
        }
        if (strcmp(cmd, "b") == 0) {
            if (!arg1) {
                printf("[debug] usage: b <addr>\n");
                continue;
            }
            uint32_t addr = 0;
            if (!parse_u32(arg1, &addr)) {
                printf("[debug] invalid address\n");
                continue;
            }
            add_breakpoint(dbg, addr);
            printf("[debug] breakpoint set at 0x%08x\n", addr);
            continue;
        }
        if (strcmp(cmd, "d") == 0) {
            if (!arg1) {
                printf("[debug] usage: d <addr>\n");
                continue;
            }
            uint32_t addr = 0;
            if (!parse_u32(arg1, &addr)) {
                printf("[debug] invalid address\n");
                continue;
            }
            remove_breakpoint(dbg, addr);
            printf("[debug] breakpoint removed at 0x%08x\n", addr);
            continue;
        }
        if (strcmp(cmd, "l") == 0) {
            printf("[debug] breakpoints (%zu):\n", dbg->breakpoint_count);
            for (size_t i = 0; i < dbg->breakpoint_count; i++) {
                printf("  0x%08x\n", dbg->breakpoints[i]);
            }
            continue;
        }
        if (strcmp(cmd, "q") == 0) {
            atomic_set_vm_halt(vm, 1);;
            return;
        }

        print_help();
    }
}

static int parse_bool_env(const char *name) {
    const char *val = getenv(name);
    if (!val)
        return 0;
    if (strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0 || strcasecmp(val, "yes") == 0)
        return 1;
    return 0;
}

static void load_breakpoints_from_env(VM_Debug *dbg) {
    const char *val = getenv("VM_BREAKPOINTS");
    if (!val || !*val)
        return;

    const char *p = val;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',' || *p == ';'))
            p++;
        if (!*p)
            break;
        char buf[64];
        size_t i = 0;
        while (*p && !isspace((unsigned char)*p) && *p != ',' && *p != ';') {
            if (i + 1 < sizeof(buf))
                buf[i++] = *p;
            p++;
        }
        buf[i] = '\0';
        uint32_t addr = 0;
        if (parse_u32(buf, &addr))
            add_breakpoint(dbg, addr);
    }
}

void vm_debug_init(VM *vm) {
    vm->debug = calloc(1, sizeof(VM_Debug));
    if (!vm->debug) {
        panic("debug alloc failed\n", vm);
        return;
    }
    vm->debug->step_mode = parse_bool_env("VM_DEBUG_STEP") || parse_bool_env("VM_STEP");
    vm->debug->pause_on_start = parse_bool_env("VM_DEBUG_PAUSE");
    load_breakpoints_from_env(vm->debug);
}

void vm_debug_destroy(VM *vm) {
    if (!vm || !vm->debug)
        return;
    free(vm->debug);
    vm->debug = NULL;
}

void vm_debug_pause_if_needed(VM *vm, uint32_t ip) {
    VM_Debug *dbg = vm->debug;
    if (!dbg)
        return;
    if (dbg->pause_on_start) {
        dbg->pause_on_start = 0;
        interactive_wait(vm);
        return;
    }
    if (dbg->step_mode || has_breakpoint(dbg, ip)) {
        interactive_wait(vm);
    }
}

void vm_debug_count_instruction(VM *vm, uint8_t op) {
#ifdef VM_INSTR_STATS
    if (!vm || !vm->debug)
        return;
    vm->debug->instr_counts[op]++;
#else
    (void)vm;
    (void)op;
#endif
}

void vm_debug_print_stats(const VM *vm) {
#ifdef VM_INSTR_STATS
    if (!vm || !vm->debug)
        return;
    printf("\n[debug] instruction statistics:\n");
    uint64_t total = 0;
    for (size_t i = 0; i < 256; i++)
        total += vm->debug->instr_counts[i];
    printf("[debug] total instructions: %llu\n", (unsigned long long)total);
    for (size_t i = 0; i < 256; i++) {
        uint64_t count = vm->debug->instr_counts[i];
        if (count == 0)
            continue;
        printf("  %-8s (%3zu) : %llu\n", op_name((uint8_t)i), i,
               (unsigned long long)count);
    }
#else
    (void)vm;
#endif
}

#endif // VM_DEBUG
