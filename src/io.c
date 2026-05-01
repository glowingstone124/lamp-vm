#include "vm.h"
#include "io.h"
#include "io_devices/disk/disk.h"
#include "interrupt.h"
#include <string.h>
#include <unistd.h>

#define SERIAL_RX_FIFO_MASK 0xFFu
#define PS2_LEGACY_FIFO_MASK 0xFFu
#define PS2_OUT_FIFO_MASK 0x1FFu

#define PS2_CONFIG_FIRST_IRQ 0x01u
#define PS2_CONFIG_SECOND_IRQ 0x02u
#define PS2_CONFIG_FIRST_DISABLED 0x10u
#define PS2_CONFIG_SECOND_DISABLED 0x20u

#define PS2_EXPECT_CONFIG 1u
#define PS2_EXPECT_KBD_LEDS 2u
#define PS2_EXPECT_KBD_TYPEMATIC 3u
#define PS2_EXPECT_KBD_SCAN_SET 4u
#define PS2_EXPECT_MOUSE_RESOLUTION 5u
#define PS2_EXPECT_MOUSE_SAMPLE_RATE 6u

static void ps2_update_status_locked(VM *vm) {
    uint8_t status = PS2_STATUS_SYSTEM;
    if (vm->ps2_out_tail != vm->ps2_out_head) {
        status |= PS2_STATUS_OUT_FULL;
        if (vm->ps2_out_aux[vm->ps2_out_tail] != 0u) {
            status |= PS2_STATUS_AUX_DATA;
        }
    }
    vm->ps2_status = status;
    vm->io[PS2_STATUS] = status;
}

static int ps2_output_push_locked(VM *vm, uint8_t value, uint8_t aux, uint8_t raise_irq) {
    const uint16_t head = vm->ps2_out_head;
    const uint16_t next = (uint16_t)((head + 1u) & PS2_OUT_FIFO_MASK);
    if (next == vm->ps2_out_tail) {
        return 0;
    }
    const int was_empty = (head == vm->ps2_out_tail);
    vm->ps2_out_fifo[head] = value;
    vm->ps2_out_aux[head] = aux ? 1u : 0u;
    vm->ps2_out_irq[head] = raise_irq ? 1u : 0u;
    vm->ps2_out_head = next;
    ps2_update_status_locked(vm);
    if (was_empty && raise_irq != 0u) {
        if (aux != 0u) {
            if ((vm->ps2_config & PS2_CONFIG_SECOND_IRQ) != 0u) {
                trigger_interrupt(vm, INT_MOUSE);
            }
        } else if ((vm->ps2_config & PS2_CONFIG_FIRST_IRQ) != 0u) {
            trigger_interrupt(vm, INT_KEYBOARD);
        }
    }
    return 1;
}

static void ps2_raise_front_irq_locked(VM *vm) {
    if (vm->ps2_out_tail == vm->ps2_out_head) {
        return;
    }
    if (vm->ps2_out_irq[vm->ps2_out_tail] == 0u) {
        return;
    }
    if (vm->ps2_out_aux[vm->ps2_out_tail] != 0u) {
        if ((vm->ps2_config & PS2_CONFIG_SECOND_IRQ) != 0u) {
            trigger_interrupt(vm, INT_MOUSE);
        }
    } else if ((vm->ps2_config & PS2_CONFIG_FIRST_IRQ) != 0u) {
        trigger_interrupt(vm, INT_KEYBOARD);
    }
}

static void ps2_kbd_response_locked(VM *vm, uint8_t value) {
    (void)ps2_output_push_locked(vm, value, 0u, 0u);
}

static void ps2_mouse_response_locked(VM *vm, uint8_t value) {
    (void)ps2_output_push_locked(vm, value, 1u, 0u);
}

static void ps2_keyboard_command_locked(VM *vm, uint8_t value) {
    if (vm->ps2_kbd_expect != 0u) {
        vm->ps2_kbd_expect = 0u;
        ps2_kbd_response_locked(vm, 0xFAu);
        return;
    }

    switch (value) {
    case 0xEDu:
        ps2_kbd_response_locked(vm, 0xFAu);
        vm->ps2_kbd_expect = PS2_EXPECT_KBD_LEDS;
        break;
    case 0xF0u:
        ps2_kbd_response_locked(vm, 0xFAu);
        vm->ps2_kbd_expect = PS2_EXPECT_KBD_SCAN_SET;
        break;
    case 0xF2u:
        ps2_kbd_response_locked(vm, 0xFAu);
        ps2_kbd_response_locked(vm, 0xABu);
        ps2_kbd_response_locked(vm, 0x83u);
        break;
    case 0xF3u:
        ps2_kbd_response_locked(vm, 0xFAu);
        vm->ps2_kbd_expect = PS2_EXPECT_KBD_TYPEMATIC;
        break;
    case 0xF4u:
        vm->ps2_kbd_scanning = 1u;
        ps2_kbd_response_locked(vm, 0xFAu);
        break;
    case 0xF5u:
        vm->ps2_kbd_scanning = 0u;
        ps2_kbd_response_locked(vm, 0xFAu);
        break;
    case 0xF6u:
        vm->ps2_kbd_scanning = 1u;
        ps2_kbd_response_locked(vm, 0xFAu);
        break;
    case 0xFFu:
        vm->ps2_kbd_scanning = 1u;
        ps2_kbd_response_locked(vm, 0xFAu);
        ps2_kbd_response_locked(vm, 0xAAu);
        break;
    default:
        ps2_kbd_response_locked(vm, 0xFAu);
        break;
    }
}

static void ps2_mouse_command_locked(VM *vm, uint8_t value) {
    if (vm->ps2_mouse_expect != 0u) {
        if (vm->ps2_mouse_expect == PS2_EXPECT_MOUSE_SAMPLE_RATE) {
            vm->ps2_mouse_sample_rate = value;
        } else if (vm->ps2_mouse_expect == PS2_EXPECT_MOUSE_RESOLUTION) {
            vm->ps2_mouse_resolution = value;
        }
        vm->ps2_mouse_expect = 0u;
        ps2_mouse_response_locked(vm, 0xFAu);
        return;
    }

    switch (value) {
    case 0xE6u:
        vm->ps2_mouse_scaling_2_1 = 0u;
        ps2_mouse_response_locked(vm, 0xFAu);
        break;
    case 0xE7u:
        vm->ps2_mouse_scaling_2_1 = 1u;
        ps2_mouse_response_locked(vm, 0xFAu);
        break;
    case 0xE8u:
        ps2_mouse_response_locked(vm, 0xFAu);
        vm->ps2_mouse_expect = PS2_EXPECT_MOUSE_RESOLUTION;
        break;
    case 0xE9u:
        ps2_mouse_response_locked(vm, 0xFAu);
        ps2_mouse_response_locked(vm, (uint8_t)(vm->ps2_mouse_reporting ? 0x20u : 0u));
        ps2_mouse_response_locked(vm, vm->ps2_mouse_resolution);
        ps2_mouse_response_locked(vm, vm->ps2_mouse_sample_rate);
        break;
    case 0xF2u:
        ps2_mouse_response_locked(vm, 0xFAu);
        ps2_mouse_response_locked(vm, 0x00u);
        break;
    case 0xF3u:
        ps2_mouse_response_locked(vm, 0xFAu);
        vm->ps2_mouse_expect = PS2_EXPECT_MOUSE_SAMPLE_RATE;
        break;
    case 0xF4u:
        vm->ps2_mouse_reporting = 1u;
        ps2_mouse_response_locked(vm, 0xFAu);
        break;
    case 0xF5u:
        vm->ps2_mouse_reporting = 0u;
        ps2_mouse_response_locked(vm, 0xFAu);
        break;
    case 0xF6u:
        vm->ps2_mouse_reporting = 0u;
        vm->ps2_mouse_sample_rate = 100u;
        vm->ps2_mouse_resolution = 2u;
        vm->ps2_mouse_scaling_2_1 = 0u;
        ps2_mouse_response_locked(vm, 0xFAu);
        break;
    case 0xFFu:
        vm->ps2_mouse_reporting = 0u;
        vm->ps2_mouse_sample_rate = 100u;
        vm->ps2_mouse_resolution = 2u;
        vm->ps2_mouse_scaling_2_1 = 0u;
        ps2_mouse_response_locked(vm, 0xFAu);
        ps2_mouse_response_locked(vm, 0xAAu);
        ps2_mouse_response_locked(vm, 0x00u);
        break;
    default:
        ps2_mouse_response_locked(vm, 0xFAu);
        break;
    }
}

static void fb_accel_scroll_up_locked(VM *vm, uint32_t clear_color) {
    const size_t line_pixels = (size_t)FB_WIDTH * 8u;
    const size_t move_pixels = (size_t)FB_WIDTH * ((size_t)FB_HEIGHT - 8u);
    memmove(vm->fb, vm->fb + line_pixels, move_pixels * sizeof(uint32_t));
    for (size_t i = move_pixels; i < (size_t)FB_WIDTH * (size_t)FB_HEIGHT; i++) {
        vm->fb[i] = clear_color;
    }
}

static void fb_accel_clear_locked(VM *vm, uint32_t color) {
    for (size_t i = 0; i < (size_t)FB_WIDTH * (size_t)FB_HEIGHT; i++) {
        vm->fb[i] = color;
    }
}

uint8_t vm_ps2_read_status(VM *vm) {
    if (!vm) {
        return 0u;
    }
    vm_shared_lock(vm);
    ps2_update_status_locked(vm);
    const uint8_t status = vm->ps2_status;
    vm_shared_unlock(vm);
    return status;
}

uint8_t vm_ps2_read_data(VM *vm) {
    if (!vm) {
        return 0u;
    }
    vm_shared_lock(vm);
    uint8_t value = 0u;
    uint8_t aux = 0u;
    if (vm->ps2_out_tail != vm->ps2_out_head) {
        value = vm->ps2_out_fifo[vm->ps2_out_tail];
        aux = vm->ps2_out_aux[vm->ps2_out_tail];
        vm->ps2_out_tail = (uint16_t)((vm->ps2_out_tail + 1u) & PS2_OUT_FIFO_MASK);
    }
    ps2_update_status_locked(vm);
    if (aux != 0u) {
        vm_interrupt_eoi(vm, BSP_CORE, INT_MOUSE);
    } else {
        vm_interrupt_eoi(vm, BSP_CORE, INT_KEYBOARD);
    }
    ps2_raise_front_irq_locked(vm);
    vm_shared_unlock(vm);
    return value;
}

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
        const uint16_t next = (uint16_t)((head + 1u) & PS2_LEGACY_FIFO_MASK);
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
        if (vm->ps2_kbd_enabled != 0u && vm->ps2_kbd_scanning != 0u &&
            (vm->ps2_config & PS2_CONFIG_FIRST_DISABLED) == 0u) {
            (void)ps2_output_push_locked(vm, c, 0u, 1u);
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
        const uint16_t next = (uint16_t)((head + 1u) & PS2_LEGACY_FIFO_MASK);
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
        if (vm->ps2_mouse_enabled != 0u && vm->ps2_mouse_reporting != 0u &&
            (vm->ps2_config & PS2_CONFIG_SECOND_DISABLED) == 0u) {
            (void)ps2_output_push_locked(vm, c, 1u, 1u);
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

    case FB_ACCEL_ARG0:
        vm->io[FB_ACCEL_ARG0] = value;
        break;

    case FB_ACCEL_CMD:
        vm->io[FB_ACCEL_CMD] = value;
        if (value == FB_ACCEL_CMD_SCROLL_UP_8PX) {
            fb_accel_scroll_up_locked(vm, (uint32_t)vm->io[FB_ACCEL_ARG0]);
        } else if (value == FB_ACCEL_CMD_CLEAR) {
            fb_accel_clear_locked(vm, (uint32_t)vm->io[FB_ACCEL_ARG0]);
        }
        break;

    case PS2_DATA:
        if (vm->ps2_pending_controller_write == PS2_EXPECT_CONFIG) {
            vm->ps2_config = (uint8_t)(value & 0x7Fu);
            vm->ps2_kbd_enabled = ((vm->ps2_config & PS2_CONFIG_FIRST_DISABLED) == 0u);
            vm->ps2_mouse_enabled = ((vm->ps2_config & PS2_CONFIG_SECOND_DISABLED) == 0u);
            vm->ps2_pending_controller_write = 0u;
        } else if (vm->ps2_next_to_mouse != 0u) {
            vm->ps2_next_to_mouse = 0u;
            ps2_mouse_command_locked(vm, (uint8_t)value);
        } else {
            ps2_keyboard_command_locked(vm, (uint8_t)value);
        }
        break;

    case PS2_COMMAND:
        switch ((uint8_t)value) {
        case 0x20u:
            ps2_output_push_locked(vm, vm->ps2_config, 0u, 0u);
            break;
        case 0x60u:
            vm->ps2_pending_controller_write = PS2_EXPECT_CONFIG;
            break;
        case 0xAAu:
            ps2_output_push_locked(vm, 0x55u, 0u, 0u);
            break;
        case 0xABu:
            ps2_output_push_locked(vm, 0x00u, 0u, 0u);
            break;
        case 0xA9u:
            ps2_output_push_locked(vm, 0x00u, 0u, 0u);
            break;
        case 0xADu:
            vm->ps2_config |= PS2_CONFIG_FIRST_DISABLED;
            vm->ps2_kbd_enabled = 0u;
            break;
        case 0xAEu:
            vm->ps2_config &= (uint8_t)~PS2_CONFIG_FIRST_DISABLED;
            vm->ps2_kbd_enabled = 1u;
            break;
        case 0xA7u:
            vm->ps2_config |= PS2_CONFIG_SECOND_DISABLED;
            vm->ps2_mouse_enabled = 0u;
            break;
        case 0xA8u:
            vm->ps2_config &= (uint8_t)~PS2_CONFIG_SECOND_DISABLED;
            vm->ps2_mouse_enabled = 1u;
            break;
        case 0xD4u:
            vm->ps2_next_to_mouse = 1u;
            break;
        case 0xFEu:
            break;
        default:
            break;
        }
        ps2_update_status_locked(vm);
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
