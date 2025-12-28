#include "vm.h"
#include "io.h"

#include "frame.h"
void accept_io(VM *vm, int addr, int value) {
    switch(addr) {
        case SCREEN: {
            char c = (char)value;
            char attr = vm->io[SCREEN_ATTRIBUTE];
            put_char_with_attr(c, attr);
            break;
        }
        default:
            vm->io[addr] = value;
    }
}

