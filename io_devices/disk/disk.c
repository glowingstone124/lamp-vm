//
// Created by Max Wang on 2025/12/30.
//
#include "../../vm.h"
#include "disk.h"

#include <stdlib.h>

#include "../../panic.h"

#include <unistd.h>

void disk_init(VM *vm, const char* path) {
    printf("cwd: %s\n", getcwd(NULL,0));

    vm->disk.fp = fopen(path, "r+b");
    if (!vm->disk.fp) {
        printf("Disk image not found, creating new disk: %s\n", path);
        vm->disk.fp = fopen(path, "w+b");
        if (!vm->disk.fp) {
            perror("fopen");
            panic("Cannot create disk image", vm);
            return;
        }

        uint8_t *buf = calloc(1, DISK_SIZE);
        if (!buf) {
            panic("Failed to allocate memory for disk init", vm);
            return;
        }
        fwrite(buf, 1, DISK_SIZE, vm->disk.fp);
        fflush(vm->disk.fp);
        free(buf);

        fclose(vm->disk.fp);
        vm->disk.fp = fopen(path, "r+b");
        printf("Disk image created: %s, size %d bytes\n", path, DISK_SIZE);
    }

    vm->disk.lba = 0;
    vm->disk.mem_addr = 0;
    vm->disk.count = 0;
    vm->disk.status = 0;
}

