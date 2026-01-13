#include "vm.h"
#include "io.h"
#include "io_devices/disk/disk.h"
#include "io_devices/frame/frame.h"

void accept_io(VM *vm, const int addr, const int value) {
    if (addr < 0 || addr >= IO_SIZE)
        return;

    switch (addr) {
    case SCREEN: {
        char c = (char)value;
        char attr = (char)vm->io[SCREEN_ATTRIBUTE];
        put_char_with_attr(c, attr);
        vm->io[SCREEN] = value;
        break;
    }

    case SCREEN_ATTRIBUTE:
        vm->io[SCREEN_ATTRIBUTE] = value & 0xFF;
        break;

    case DISK_CMD:
        vm->io[DISK_CMD] = value;
        disk_cmd(vm, value);
        break;

    case DISK_LBA:
        vm->disk.lba = value;
        vm->io[DISK_LBA] = value;
        break;

    case DISK_MEM:
        vm->disk.mem_addr = value;
        vm->io[DISK_MEM] = value;
        break;

    case DISK_COUNT:
        vm->disk.count = value;
        vm->io[DISK_COUNT] = value;
        break;

    default:
        vm->io[addr] = value;
        break;
    }
}
