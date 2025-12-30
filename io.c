#include "vm.h"
#include "io.h"
#include "io_devices/disk/disk.h"
#include "io_devices/frame/frame.h"

void accept_io(VM *vm, const int addr, const int value) {
    if (addr < 0 || addr >= IO_SIZE) return;

    switch (addr) {
        case SCREEN: {
            char c = (char) value;
            char attr = vm->io[SCREEN_ATTRIBUTE];
            put_char_with_attr(c, attr);
            break;
        }
        case SCREEN_ATTRIBUTE:
            vm->io[SCREEN_ATTRIBUTE] = value & 0xFF;
            break;
        case DISK_CMD:
            disk_cmd(vm, value);
            break;
        case DISK_LBA:
            vm->disk.lba = value;
            break;
        case DISK_MEM:
            vm->disk.mem_addr = value;
            break;
        case DISK_COUNT:
            vm->disk.count = value;
            break;
        default:
            break;
    }
    vm->io[addr] = value;
}
