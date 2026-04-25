#include "vm.h"
#include "io.h"
#include "io_devices/disk/disk.h"
#include "interrupt.h"
#include <unistd.h>

#define SERIAL_RX_FIFO_MASK 0xFFu

int vm_serial_rx_enqueue(VM *vm, uint8_t c) {
    if (!vm) {
        return 0;
    }

    vm_shared_lock(vm);

    const uint16_t head = vm->serial_rx_head;
    const uint16_t tail = vm->serial_rx_tail;
    const uint16_t next = (uint16_t)((head + 1u) & SERIAL_RX_FIFO_MASK);
    if (next == tail) {
        vm_shared_unlock(vm);
        return 0;
    }

    const int was_empty = (head == tail);
    vm->serial_rx_fifo[head] = c;
    vm->serial_rx_head = next;

    if (was_empty) {
        vm->io[KEYBOARD] = (int)vm->serial_rx_fifo[tail];
        vm->io[SCREEN_ATTRIBUTE] |= SERIAL_STATUS_RX_READY;
        if ((vm->io[SCREEN_ATTRIBUTE] >> 8) & SERIAL_CTRL_RX_INT_ENABLE) {
            trigger_interrupt(vm, INT_SERIAL);
        }
    }

    vm_shared_unlock(vm);
    return 1;
}

int vm_ps2_kbd_enqueue(VM *vm, uint8_t c) {
    if (!vm) {
        return 0;
    }

    vm_shared_lock(vm);

    {
        const uint16_t head = vm->ps2_kbd_head;
        const uint16_t tail = vm->ps2_kbd_tail;
        const uint16_t next = (uint16_t)((head + 1u) & SERIAL_RX_FIFO_MASK);
        if (next == tail) {
            vm_shared_unlock(vm);
            return 0;
        }

        const int was_empty = (head == tail);
        vm->ps2_kbd_fifo[head] = c;
        vm->ps2_kbd_head = next;

        if (was_empty) {
            vm->io[PS2_KBD_DATA] = (int)vm->ps2_kbd_fifo[tail];
            vm->io[PS2_KBD_STATUS] |= PS2_STATUS_RX_READY;
            trigger_interrupt(vm, INT_KEYBOARD);
        }
    }

    vm_shared_unlock(vm);
    return 1;
}

int vm_ps2_mouse_enqueue(VM *vm, uint8_t c) {
    if (!vm) {
        return 0;
    }

    vm_shared_lock(vm);

    {
        const uint16_t head = vm->ps2_mouse_head;
        const uint16_t tail = vm->ps2_mouse_tail;
        const uint16_t next = (uint16_t)((head + 1u) & SERIAL_RX_FIFO_MASK);
        if (next == tail) {
            vm_shared_unlock(vm);
            return 0;
        }

        const int was_empty = (head == tail);
        vm->ps2_mouse_fifo[head] = c;
        vm->ps2_mouse_head = next;

        if (was_empty) {
            vm->io[PS2_MOUSE_DATA] = (int)vm->ps2_mouse_fifo[tail];
            vm->io[PS2_MOUSE_STATUS] |= PS2_STATUS_RX_READY;
            trigger_interrupt(vm, INT_MOUSE);
        }
    }

    vm_shared_unlock(vm);
    return 1;
}

void accept_io(VM *vm, const int addr, const int value) {
    if (addr < 0 || addr >= IO_SIZE)
        return;
    vm_shared_lock(vm);

    switch (addr) {
    case SCREEN: {
        unsigned char c = (unsigned char)value;
        write(STDOUT_FILENO, &c, 1);
        vm->io[SCREEN] = value;
        break;
    }

    case SCREEN_ATTRIBUTE:
        vm->io[SCREEN_ATTRIBUTE] = (vm->io[SCREEN_ATTRIBUTE] & 0xFF) |
            ((value & 0xFF) << 8);
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
    vm_shared_unlock(vm);
}
