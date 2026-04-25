#ifndef VM_IO_H
#define VM_IO_H

#include <stdint.h>

void accept_io(VM *vm, int addr, int value);
int vm_serial_rx_enqueue(VM *vm, uint8_t c);
int vm_ps2_kbd_enqueue(VM *vm, uint8_t c);
int vm_ps2_mouse_enqueue(VM *vm, uint8_t c);

enum IO_TABLE {
    SCREEN = 0x01,
    SCREEN_ATTRIBUTE = 0x02,
    KEYBOARD = 0x03,
    PS2_KBD_STATUS = 0x04,
    PS2_KBD_DATA = 0x05,
    PS2_MOUSE_STATUS = 0x06,
    PS2_MOUSE_DATA = 0x07,
    DISK_CMD = 0x10,
    DISK_LBA = 0x11,
    DISK_MEM = 0x12,
    DISK_COUNT = 0x13,
    DISK_STATUS = 0x14,
    CPU_CTX_CSP = 0xF0,
    CPU_CTX_DSP = 0xF1,
    CPU_CTX_IRQ_MASK = 0xF2,
    CPU_CTX_ISP = 0xF3,
    CPU_CTX_CALL_BASE = 0xF4,
    CPU_CTX_DATA_BASE = 0xF5,
    CPU_CTX_ISR_BASE = 0xF6,
};

// SCREEN / SCREEN_ATTRIBUTE / KEYBOARD are repurposed as a basic serial device:
// SCREEN: TX write-only, KEYBOARD: RX read-only, SCREEN_ATTRIBUTE: status/control.
#define SERIAL_STATUS_RX_READY 0x01
#define SERIAL_STATUS_TX_READY 0x02
#define SERIAL_CTRL_RX_INT_ENABLE 0x01

#define PS2_STATUS_RX_READY 0x01
#endif
