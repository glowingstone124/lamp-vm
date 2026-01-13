//
// Created by Max Wang on 2025/12/30.
//

#ifndef VM_DISK_H
#define VM_DISK_H
#define DISK_SIZE (1024 * 1024)
#define DISK_SECTOR_SIZE 512
#define DISK_CMD_READ 1
#define DISK_CMD_WRITE 2

static inline void disk_set_busy(VM *vm) { vm->disk.status = 1; }

static inline void disk_set_free(VM *vm) { vm->disk.status = 0; }

void disk_init(VM *vm, const char *path);
void disk_cmd(VM *vm, int value);
#endif // VM_DISK_H