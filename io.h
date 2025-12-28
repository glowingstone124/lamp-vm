#ifndef VM_IO_H
#define VM_IO_H
void accept_io(VM *vm, int addr, int value);

enum IO_TABLE {
    SCREEN = 0,
    SCREEN_ATTRIBUTE
};
#endif