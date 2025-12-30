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

void disk_read(VM *vm) {
    fseek(vm->disk.fp,
               vm->disk.lba * DISK_SECTOR_SIZE,
               SEEK_SET);
    fread(&vm->memory[vm->disk.mem_addr],
          DISK_SECTOR_SIZE,
          vm->disk.count,
          vm->disk.fp
    );
}

void disk_write(const VM *vm) {
    fseek(vm->disk.fp,
              vm->disk.lba * DISK_SECTOR_SIZE,
              SEEK_SET);
    fwrite(&vm->memory[vm->disk.mem_addr],
           DISK_SECTOR_SIZE,
           vm->disk.count,
           vm->disk.fp);
    fflush(vm->disk.fp);
    const int fd = fileno(vm->disk.fp);
    if (fd >= 0) {
        fsync(fd);
    }
}

void disk_cmd(VM *vm, const int value) {
    disk_set_busy(vm);
    if (value == DISK_CMD_READ) {
        disk_read(vm);
    } else if (value == DISK_CMD_WRITE) {
        disk_write(vm);
    }
    disk_set_free(vm);
}

