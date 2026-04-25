//
// Created by Max Wang on 2025/12/30.
//
#include "../../vm.h"
#include "disk.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "../../panic.h"

#include <unistd.h>

#include "../../memory.h"
#include "../../io.h"
#include "../iommu/iommu_mmio_register.h"

#include "../../interrupt.h"

static int disk_count_to_bytes(uint32_t count, uint64_t *bytes_out) {
    if (!bytes_out || count == 0u) {
        return 0;
    }
    if ((uint64_t)count > (UINT64_MAX / (uint64_t)DISK_SECTOR_SIZE)) {
        return 0;
    }
    *bytes_out = (uint64_t)count * (uint64_t)DISK_SECTOR_SIZE;
    return 1;
}

static int is_valid_dma(VM *vm, uint64_t addr, uint64_t bytes) {
    if (!vm || bytes == 0u) {
        return 0;
    }
    if (addr >= (uint64_t)vm->memory_size) {
        return 0;
    }
    if (bytes > ((uint64_t)vm->memory_size - addr)) {
        return 0;
    }
    return 1;
}

static int is_valid_lba(VM *vm, uint64_t lba, uint32_t count, uint64_t *disk_off_out) {
    uint64_t start;
    uint64_t bytes;
    if (!vm || count == 0u) {
        return 0;
    }
    if ((uint64_t)count > (UINT64_MAX / (uint64_t)DISK_SECTOR_SIZE)) {
        return 0;
    }
    if (lba > (UINT64_MAX / (uint64_t)DISK_SECTOR_SIZE)) {
        return 0;
    }
    start = lba * (uint64_t)DISK_SECTOR_SIZE;
    bytes = (uint64_t)count * (uint64_t)DISK_SECTOR_SIZE;
    if (start >= vm->disk_size_bytes) {
        return 0;
    }
    if (bytes > (vm->disk_size_bytes - start)) {
        return 0;
    }
    if (disk_off_out) {
        *disk_off_out = start;
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

static void* disk_worker(void *arg) {
    VM* vm = arg;
    while (1) {
        int ok = 1;
        int cmd;
        uint64_t lba;
        uint64_t mem_addr;
        uint64_t dma_addr;
        uint32_t count;
        uint64_t bytes = 0u;
        uint64_t disk_off = 0u;

        pthread_mutex_lock(&vm->disk.mutex);

        while (vm->disk.current_cmd == DISK_CMD_NONE && vm->disk.thread_running) {
            pthread_cond_wait(&vm->disk.cond_var, &vm->disk.mutex);
        }

        if (!vm->disk.thread_running) {
            pthread_mutex_unlock(&vm->disk.mutex);
            break;
        }

        cmd = vm->disk.current_cmd;
        lba = vm->disk.lba;
        mem_addr = vm->disk.mem_addr;
        count = vm->disk.count;

        pthread_mutex_unlock(&vm->disk.mutex);

        if (cmd != DISK_CMD_READ && cmd != DISK_CMD_WRITE) {
            fprintf(stderr, "[Disk] Invalid CMD %d\n", cmd);
            ok = 0;
        }

        if (!disk_count_to_bytes(count, &bytes)) {
            fprintf(stderr, "[Disk] Invalid DMA count=%u\n", (unsigned int)count);
            ok = 0;
        }

        dma_addr = mem_addr;
        if (ok && !vm_iommu_translate_dma(vm, IOMMU_DEV_DISK, mem_addr, bytes, &dma_addr)) {
            fprintf(stderr, "[Disk] IOMMU reject iova=0x%llx len=%llu\n",
                    (unsigned long long)mem_addr, (unsigned long long)bytes);
            ok = 0;
        }
        if (ok && !is_valid_dma(vm, dma_addr, bytes)) {
            fprintf(stderr, "[Disk] DMA violation @ Addr 0x%llx, Count %u\n",
                    (unsigned long long)dma_addr, (unsigned int)count);
            ok = 0;
        }

        if (!is_valid_lba(vm, lba, count, &disk_off)) {
            fprintf(stderr, "[Disk] LBA out of range @ LBA %llu, Count %u, DiskBytes %llu\n",
                    (unsigned long long)lba,
                    (unsigned int)count,
                    (unsigned long long)vm->disk_size_bytes);
            ok = 0;
        }

        if (ok) {
            if (disk_off > (uint64_t)LONG_MAX) {
                ok = 0;
            } else if (fseek(vm->disk.fp, (long)disk_off, SEEK_SET) != 0) {
                fprintf(stderr, "[Disk] fseek failed: %s\n", strerror(errno));
                ok = 0;
            }
        }

        if (ok) {
            if (bytes > (uint64_t)SIZE_MAX) {
                ok = 0;
            } else if (cmd == DISK_CMD_READ) {
                uint8_t *buf = malloc((size_t)bytes);
                if (!buf) {
                    fprintf(stderr, "[Disk] OOM during READ DMA\n");
                    ok = 0;
                } else {
                    size_t got = fread(buf, 1, (size_t)bytes, vm->disk.fp);
                    if (got != (size_t)bytes) {
                        fprintf(stderr, "[Disk] READ short I/O got=%zu want=%zu\n", got, (size_t)bytes);
                        ok = 0;
                    } else {
                        vm_shared_lock(vm);
                        memcpy(&vm->memory[(size_t)dma_addr], buf, (size_t)bytes);
                        vm_shared_unlock(vm);
                    }
                    free(buf);
                }
            } else {
                uint8_t *buf = malloc((size_t)bytes);
                if (!buf) {
                    fprintf(stderr, "[Disk] OOM during WRITE DMA\n");
                    ok = 0;
                } else {
                    vm_shared_lock(vm);
                    memcpy(buf, &vm->memory[(size_t)dma_addr], (size_t)bytes);
                    vm_shared_unlock(vm);

                    size_t put = fwrite(buf, 1, (size_t)bytes, vm->disk.fp);
                    if (put != (size_t)bytes) {
                        fprintf(stderr, "[Disk] WRITE short I/O got=%zu want=%zu\n", put, (size_t)bytes);
                        ok = 0;
                    } else if (fflush(vm->disk.fp) != 0) {
                        fprintf(stderr, "[Disk] fflush failed: %s\n", strerror(errno));
                        ok = 0;
                    }
                    free(buf);
                }
            }
        }

        pthread_mutex_lock(&vm->disk.mutex);
        vm->disk.current_cmd = DISK_CMD_NONE;
        vm->disk.op_complete = true;
        vm->disk.status = ok ? DISK_STATUS_FREE : DISK_STATUS_ERROR;
        vm->io[DISK_STATUS] = vm->disk.status;
        pthread_mutex_unlock(&vm->disk.mutex);
    }
    return NULL;
}

void disk_init(VM *vm, const char *path) {
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

void disk_cmd(VM *vm, const int value) {
    pthread_mutex_lock(&vm->disk.mutex);

    if (vm->disk.status == DISK_STATUS_BUSY) {
        pthread_mutex_unlock(&vm->disk.mutex);
        return;
    }

    if (value != DISK_CMD_READ && value != DISK_CMD_WRITE) {
        vm->disk.current_cmd = DISK_CMD_NONE;
        vm->disk.status = DISK_STATUS_ERROR;
        vm->disk.op_complete = true;
        vm->io[DISK_STATUS] = DISK_STATUS_ERROR;
    } else {
        vm->disk.current_cmd = value;
        vm->disk.status = DISK_STATUS_BUSY;
        vm->disk.op_complete = false;
        vm->io[DISK_STATUS] = DISK_STATUS_BUSY;
        pthread_cond_signal(&vm->disk.cond_var);
    }

    pthread_mutex_unlock(&vm->disk.mutex);
}
void disk_tick(VM *vm) {
    pthread_mutex_lock(&vm->disk.mutex);
    if (vm->disk.op_complete) {
        vm->disk.op_complete = false;
        trigger_interrupt(vm, INT_DISK_COMPLETE);
    }
    pthread_mutex_unlock(&vm->disk.mutex);
}
