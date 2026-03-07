//
// Created by Max Wang on 2025/12/30.
//
#include "../../vm.h"
#include "disk.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "../../panic.h"

#include <unistd.h>

#include "../../memory.h"
#include "../../io.h"

#include "../../interrupt.h"

static int is_valid_dma(VM *vm, size_t addr, size_t count) {
    if (addr + (count * DISK_SECTOR_SIZE) > vm->memory_size) {
        return 0;
    }
    return 1;
}

static uint64_t disk_detect_size_bytes(FILE *fp) {
    if (!fp) {
        return DISK_SIZE;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        return DISK_SIZE;
    }
    long end = ftell(fp);
    if (end < 0) {
        return DISK_SIZE;
    }
    return (uint64_t)(unsigned long)end;
}

void* disk_worker(void *arg) {
    VM* vm = arg;
    while (1) {
        pthread_mutex_lock(&vm->disk.mutex);

        while (vm->disk.current_cmd == DISK_CMD_NONE && vm->disk.thread_running) {
            pthread_cond_wait(&vm->disk.cond_var, &vm->disk.mutex);
        }

        if (!vm->disk.thread_running) {
            pthread_mutex_unlock(&vm->disk.mutex);
            break;
        }

        int cmd = vm->disk.current_cmd;
        uint64_t lba = vm->disk.lba;
        uint64_t mem_addr = vm->disk.mem_addr;
        uint32_t count = vm->disk.count;

        pthread_mutex_unlock(&vm->disk.mutex);

        if (!is_valid_dma(vm, mem_addr, count)) {
            fprintf(stderr, "[Disk] DMA Violation @ Addr 0x%lx, Count %d\n", mem_addr, count);
        } else {
            fseek(vm->disk.fp, lba * DISK_SECTOR_SIZE, SEEK_SET);
            size_t bytes = (size_t)count * DISK_SECTOR_SIZE;

            if (cmd == DISK_CMD_READ) {
                uint8_t *buf = malloc(bytes);
                if (!buf) {
                    fprintf(stderr, "[Disk] OOM during READ DMA\n");
                } else {
                    fread(buf, DISK_SECTOR_SIZE, count, vm->disk.fp);
                    vm_shared_lock(vm);
                    memcpy(&vm->memory[mem_addr], buf, bytes);
                    vm_shared_unlock(vm);
                    free(buf);
                }
            } else if (cmd ==DISK_CMD_WRITE) {
                uint8_t *buf = malloc(bytes);
                if (!buf) {
                    fprintf(stderr, "[Disk] OOM during WRITE DMA\n");
                } else {
                    vm_shared_lock(vm);
                    memcpy(buf, &vm->memory[mem_addr], bytes);
                    vm_shared_unlock(vm);
                    fwrite(buf, DISK_SECTOR_SIZE, count, vm->disk.fp);
                    fflush(vm->disk.fp);
                    free(buf);
                }
            }
        }

        pthread_mutex_lock(&vm->disk.mutex);
        vm->disk.current_cmd = DISK_CMD_NONE;
        vm->disk.op_complete = true;
        vm->io[DISK_STATUS] = DISK_STATUS_FREE;
        pthread_mutex_unlock(&vm->disk.mutex);
    }
    return NULL;
}

void disk_init(VM *vm, const char *path) {
    printf("cwd: %s\n", getcwd(NULL, 0));

    vm->disk.fp = fopen(path, "r+b");
    if (!vm->disk.fp) {
        printf("[Disk] Creating new image: %s\n", path);
        vm->disk.fp = fopen(path, "w+b");
        if (!vm->disk.fp) {
            perror("fopen");
            panic("Cannot create disk image", vm);
            return;
        }
        if (ftruncate(fileno(vm->disk.fp), DISK_SIZE) != 0) {
            panic("ftruncate faild",vm);
        }
        fclose(vm->disk.fp);
        vm->disk.fp = fopen(path, "r+b");
    }
    vm->disk.lba = 0;
    vm->disk.mem_addr = 0;
    vm->disk.count = 0;
    vm->disk_size_bytes = disk_detect_size_bytes(vm->disk.fp);
    vm->disk.status = DISK_STATUS_FREE;
    vm->disk.current_cmd = DISK_CMD_NONE;
    vm->disk.op_complete = false;
    vm->disk.thread_running = true;
    vm->io[DISK_STATUS] = DISK_STATUS_FREE;

    pthread_mutex_init(&vm->disk.mutex, NULL);
    pthread_cond_init(&vm->disk.cond_var, NULL);

    if (pthread_create(&vm->disk.worker_thread, NULL, disk_worker, vm) != 0) {
        panic("Failed to create disk worker", vm);
    }

    printf("[Disk] Created disk worker thread. Image: %s\n", path);
}

void disk_close(VM *vm) {
    pthread_mutex_lock(&vm->disk.mutex);
    vm->disk.thread_running = false;
    pthread_cond_signal(&vm->disk.cond_var);
    pthread_mutex_unlock(&vm->disk.mutex);

    pthread_join(vm->disk.worker_thread, NULL);
    pthread_mutex_destroy(&vm->disk.mutex);
    pthread_cond_destroy(&vm->disk.cond_var);

    if (vm->disk.fp) fclose(vm->disk.fp);
}

void disk_read(VM *vm) {
    size_t addr = vm->disk.mem_addr;
    size_t bytes = (size_t)vm->disk.count * DISK_SECTOR_SIZE;
    fseek(vm->disk.fp, vm->disk.lba * DISK_SECTOR_SIZE, SEEK_SET);

    if (bytes == 0) {
        return;
    }
    uint8_t *buf = malloc(bytes);
    if (!buf) {
        fprintf(stderr, "[Disk] OOM during legacy disk_read\n");
        return;
    }
    fread(buf, 1, bytes, vm->disk.fp);

    for (size_t i = 0; i < bytes; i++) {
        vm_write8(vm, addr + i, buf[i]);
    }
    free(buf);
}

void disk_write(const VM *vm) {
    size_t addr = vm->disk.mem_addr;
    fseek(vm->disk.fp, vm->disk.lba * DISK_SECTOR_SIZE, SEEK_SET);

    uint8_t buf[DISK_SECTOR_SIZE * vm->disk.count];
    for (size_t i = 0; i < vm->disk.count * DISK_SECTOR_SIZE; i++) {
        buf[i] = vm_read8((VM *)vm, addr + i);
    }

    fwrite(buf, 1, vm->disk.count * DISK_SECTOR_SIZE, vm->disk.fp);
    fflush(vm->disk.fp);
    int fd = fileno(vm->disk.fp);
    if (fd >= 0) fsync(fd);
}

void disk_cmd(VM *vm, const int value) {
    pthread_mutex_lock(&vm->disk.mutex);

    if (vm->disk.status == DISK_STATUS_BUSY) {
        pthread_mutex_unlock(&vm->disk.mutex);
        return;
    }

    vm->disk.current_cmd = value;
    vm->disk.status = DISK_STATUS_BUSY;
    vm->disk.op_complete = false;
    vm->io[DISK_STATUS] = DISK_STATUS_BUSY;

    pthread_cond_signal(&vm->disk.cond_var);
    pthread_mutex_unlock(&vm->disk.mutex);
}
void disk_tick(VM *vm) {
    if (vm->disk.status == DISK_STATUS_FREE) return;

    pthread_mutex_lock(&vm->disk.mutex);

    if (vm->disk.op_complete) {
        vm->disk.status = DISK_STATUS_FREE;
        vm->disk.op_complete = false;
        vm->io[DISK_STATUS] = DISK_STATUS_FREE;

        trigger_interrupt(vm, INT_DISK_COMPLETE);
    }

    pthread_mutex_unlock(&vm->disk.mutex);
}
