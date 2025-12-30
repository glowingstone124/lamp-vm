#include "vm.h"
#include "io.h"

#include <unistd.h>

#include "io_devices/disk/disk.h"
#include "io_devices/frame/frame.h"

void accept_io(VM *vm, int addr, int value) {
    if (addr < 0 || addr >= IO_SIZE) return;

    switch (addr) {
        case SCREEN: {
            char c = (char) value;
            char attr = vm->io[SCREEN_ATTRIBUTE];
            put_char_with_attr(c, attr);
            break;
        }
        case SCREEN_ATTRIBUTE: {
            vm->io[SCREEN_ATTRIBUTE] = value & 0xFF;
            break;
        }
        case DISK_CMD: {
            disk_set_busy(vm);
            if (value == DISK_CMD_READ) {
                fseek(vm->disk.fp,
                      vm->disk.lba * DISK_SECTOR_SIZE,
                      SEEK_SET);
                fread(&vm->memory[vm->disk.mem_addr],
                      DISK_SECTOR_SIZE,
                      vm->disk.count,
                      vm->disk.fp
                );
            } else if (value == DISK_CMD_WRITE) {
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
            disk_set_free(vm);
            break;
        }
        case DISK_LBA:
            vm->disk.lba = value;
            break;
        case DISK_MEM:
            vm->disk.mem_addr = value;
            break;
        case DISK_COUNT:
            vm->disk.count = value;
            break;

        default: {
            vm->io[addr] = value;
        }
    }
}
