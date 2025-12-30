//
// Created by Max Wang on 2025/12/30.
//
#include "../../vm.h"
#include "disk.h"

#include "../../panic.h"

void disk_init(VM *vm, const char* path) {
    vm->disk.fp = fopen(path, "r+b");
    if (!vm->disk.fp) {
        vm->disk.fp = fopen(path, "w+b");
        if (!vm->disk.fp) {
            panic("Cannot open disk image", vm);
            return;
        }
        fseek(vm->disk.fp, DISK_SIZE - 1, SEEK_SET);
        fputc(0, vm->disk.fp);
        fflush(vm->disk.fp);
    }

    vm->disk.lba = 0;
    vm->disk.mem_addr = 0;
    vm->disk.count = 0;
    vm->disk.status = 0;
}
