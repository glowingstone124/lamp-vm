#include "vm.h"
#include "io.h"

#include "io_devices/frame/frame.h"
void accept_io(VM *vm, int addr, int value) {
    if (addr < 0 || addr >= IO_SIZE) return;

    switch (addr) {
        case SCREEN: {
            char c = (char)value;
            char attr = vm->io[SCREEN_ATTRIBUTE];
            put_char_with_attr(c, attr);
            break;
        }
        case SCREEN_ATTRIBUTE: {
            vm->io[SCREEN_ATTRIBUTE] = value & 0xFF;
            break;
        }
        default: {
            vm->io[addr] = value;
        }
    }
}

