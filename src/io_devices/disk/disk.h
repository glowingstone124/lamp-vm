//
// Created by Max Wang on 2025/12/30.
//

#ifndef VM_DISK_H
#define VM_DISK_H
#define DISK_SIZE (1024 * 1024 * 512)
#define DISK_SECTOR_SIZE 512
#define DISK_STATUS_FREE 0
#define DISK_STATUS_BUSY 1
#define DISK_STATUS_ERROR 2
#define DISK_CMD_NONE 0
#define DISK_CMD_READ 1
#define DISK_CMD_WRITE 2

void disk_init(VM *vm, const char *path);
void disk_cmd(VM *vm, int value);
int disk_read_status(VM *vm);
void disk_close(VM *vm);
#endif // VM_DISK_H
