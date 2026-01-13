#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "vm.h"
//
// Created by Max Wang on 2025/12/29.
//
const char *panic_format(const char *fmt, ...) {
    static char buffer[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    return buffer;
}

void panic(const char *msg, VM *vm) {
    printf("VM panic detected. %s\n @ %lu clock cycle, IP = %lu", msg, vm->execution_times, vm->ip);
    printf("Creating VM dump...");
    vm_dump(vm, DUMP_MEM_SEEK_LEN);
    if (vm)
        vm->panic = 1;
    exit(1);
}