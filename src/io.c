#include "vm.h"
#include "io.h"
#include "io_devices/disk/disk.h"
#include "interrupt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SERIAL_RX_FIFO_MASK 0xFFu
#define SERIAL_TX_FIFO_MASK 0x1FFFu
#define PS2_LEGACY_FIFO_MASK 0xFFu
#define PS2_OUT_FIFO_MASK 0x1FFu
#define PS2_RESPONSE_RESERVE 4u

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

static int console_trace_enabled(void) {
    static int initialized;
    static int enabled;
    if (!initialized) {
        enabled = getenv("LAMP_CONSOLE_TRACE") ? 1 : 0;
        initialized = 1;
    }
    return enabled;
}

static void ps2_update_status_locked(VM *vm) {
    uint8_t status = PS2_STATUS_SYSTEM;
    if (vm->ps2_out_tail != vm->ps2_out_head) {
        status |= PS2_STATUS_OUT_FULL;
        if (vm->ps2_out_aux[vm->ps2_out_tail] != 0u) {
            status |= PS2_STATUS_AUX_DATA;
        }
        if (vm->ps2_out_irq[vm->ps2_out_tail] == 0u) {
            status |= PS2_STATUS_COMMAND_RESPONSE;
        }
    }
    vm->ps2_status = status;
    vm->io[PS2_STATUS] = status;
}

static uint16_t ps2_fifo_free(uint16_t head, uint16_t tail,
                              uint16_t mask) {
    return (uint16_t)((tail - head - 1u) & mask);
}

static int ps2_output_push_bytes_locked(VM *vm, const uint8_t *values,
                                         size_t count, uint8_t aux,
                                         uint8_t raise_irq);

static int ps2_output_push_locked(VM *vm, uint8_t value, uint8_t aux, uint8_t raise_irq) {
    return ps2_output_push_bytes_locked(vm, &value, 1u, aux, raise_irq);
}

static int ps2_output_push_bytes_locked(VM *vm, const uint8_t *values,
                                         size_t count, uint8_t aux,
                                         uint8_t raise_irq) {
    uint16_t head;
    const int was_empty = vm->ps2_out_head == vm->ps2_out_tail;
    if (!values || count == 0u || count > (size_t)PS2_OUT_FIFO_MASK ||
        ps2_fifo_free(vm->ps2_out_head, vm->ps2_out_tail,
                      PS2_OUT_FIFO_MASK) < count) {
        return 0;
    }

    head = vm->ps2_out_head;
    for (size_t i = 0u; i < count; i++) {
        vm->ps2_out_fifo[head] = values[i];
        vm->ps2_out_aux[head] = aux ? 1u : 0u;
        vm->ps2_out_irq[head] = raise_irq ? 1u : 0u;
        head = (uint16_t)((head + 1u) & PS2_OUT_FIFO_MASK);
    }
    vm->ps2_out_head = head;
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

static int ps2_output_push_packet_locked(VM *vm, const uint8_t packet[3]) {
    return ps2_output_push_bytes_locked(vm, packet, 3u, 1u, 1u);
}

static int ps2_legacy_kbd_push_bytes_locked(VM *vm, const uint8_t *bytes,
                                             size_t count) {
    uint16_t head;
    const uint16_t tail = vm->ps2_kbd_tail;
    const int was_empty = vm->ps2_kbd_head == tail;
    if (!bytes || count == 0u || count > (size_t)PS2_LEGACY_FIFO_MASK ||
        ps2_fifo_free(vm->ps2_kbd_head, tail, PS2_LEGACY_FIFO_MASK) < count) {
        return 0;
    }

    head = vm->ps2_kbd_head;
    for (size_t i = 0u; i < count; i++) {
        vm->ps2_kbd_fifo[head] = bytes[i];
        head = (uint16_t)((head + 1u) & PS2_LEGACY_FIFO_MASK);
    }
    vm->ps2_kbd_head = head;
    if (was_empty) {
        vm->io[PS2_KBD_DATA] = (int)bytes[0];
        vm->io[PS2_KBD_STATUS] |= PS2_STATUS_RX_READY;
        trigger_interrupt(vm, INT_KEYBOARD);
    }
    return 1;
}

static int ps2_legacy_mouse_push_packet_locked(VM *vm,
                                                const uint8_t packet[3]) {
    uint16_t head;
    const uint16_t tail = vm->ps2_mouse_tail;
    const int was_empty = vm->ps2_mouse_head == tail;

    if (ps2_fifo_free(vm->ps2_mouse_head, tail,
                      PS2_LEGACY_FIFO_MASK) < 3u) {
        return 0;
    }

    head = vm->ps2_mouse_head;
    for (uint32_t i = 0u; i < 3u; i++) {
        vm->ps2_mouse_fifo[head] = packet[i];
        head = (uint16_t)((head + 1u) & PS2_LEGACY_FIFO_MASK);
    }
    vm->ps2_mouse_head = head;
    if (was_empty) {
        vm->io[PS2_MOUSE_DATA] = (int)packet[0];
        vm->io[PS2_MOUSE_STATUS] |= PS2_STATUS_RX_READY;
        trigger_interrupt(vm, INT_MOUSE);
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
        vm->ps2_input_ready = 0u;
        ps2_kbd_response_locked(vm, 0xFAu);
        break;
    case 0xF6u:
        vm->ps2_kbd_scanning = 1u;
        ps2_kbd_response_locked(vm, 0xFAu);
        break;
    case 0xFFu:
        vm->ps2_kbd_scanning = 1u;
        vm->ps2_input_ready = 0u;
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
        /* The guest kernel finishes its controller handshake with mouse F4.
         * Do not expose host events before that point: queued packets would
         * otherwise be mistaken for config/ACK bytes during boot. */
        vm->ps2_input_ready =
            vm->ps2_kbd_scanning != 0u && vm->ps2_kbd_enabled != 0u &&
            vm->ps2_mouse_enabled != 0u &&
            (vm->ps2_config & (PS2_CONFIG_FIRST_IRQ |
                               PS2_CONFIG_SECOND_IRQ)) ==
                (PS2_CONFIG_FIRST_IRQ | PS2_CONFIG_SECOND_IRQ);
        ps2_mouse_response_locked(vm, 0xFAu);
        break;
    case 0xF5u:
        vm->ps2_mouse_reporting = 0u;
        vm->ps2_input_ready = 0u;
        ps2_mouse_response_locked(vm, 0xFAu);
        break;
    case 0xF6u:
        vm->ps2_mouse_reporting = 0u;
        vm->ps2_mouse_sample_rate = 100u;
        vm->ps2_mouse_resolution = 2u;
        vm->ps2_mouse_scaling_2_1 = 0u;
        vm->ps2_input_ready = 0u;
        ps2_mouse_response_locked(vm, 0xFAu);
        break;
    case 0xFFu:
        vm->ps2_mouse_reporting = 0u;
        vm->ps2_input_ready = 0u;
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
    for (size_t row = 0; row < FB_HEIGHT; row++) {
        vm_fb_row_lock(vm, row);
    }
    memmove(vm->fb, vm->fb + line_pixels, move_pixels * sizeof(uint32_t));
    for (size_t i = move_pixels; i < (size_t)FB_WIDTH * (size_t)FB_HEIGHT; i++) {
        vm->fb[i] = clear_color;
    }
    vm_fb_mark_all_dirty(vm);
    for (size_t row = FB_HEIGHT; row > 0; row--) {
        vm_fb_row_unlock(vm, row - 1u);
    }
}

static void fb_accel_clear_locked(VM *vm, uint32_t color) {
    for (size_t row = 0; row < FB_HEIGHT; row++) {
        vm_fb_row_lock(vm, row);
    }
    for (size_t i = 0; i < (size_t)FB_WIDTH * (size_t)FB_HEIGHT; i++) {
        vm->fb[i] = color;
    }
    vm_fb_mark_all_dirty(vm);
    for (size_t row = FB_HEIGHT; row > 0; row--) {
        vm_fb_row_unlock(vm, row - 1u);
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

int vm_ps2_input_ready(VM *vm) {
    int ready;
    if (!vm) {
        return 0;
    }
    vm_shared_lock(vm);
    ready = vm->ps2_input_ready != 0u;
    vm_shared_unlock(vm);
    return ready;
}

uint8_t vm_ps2_read_data(VM *vm) {
    if (!vm) {
        return 0u;
    }
    vm_shared_lock(vm);
    uint8_t value = 0u;
    uint8_t aux = 0u;
    uint8_t raise_irq = 0u;
    if (vm->ps2_out_tail != vm->ps2_out_head) {
        value = vm->ps2_out_fifo[vm->ps2_out_tail];
        aux = vm->ps2_out_aux[vm->ps2_out_tail];
        raise_irq = vm->ps2_out_irq[vm->ps2_out_tail];
        vm->ps2_out_tail = (uint16_t)((vm->ps2_out_tail + 1u) & PS2_OUT_FIFO_MASK);
    }
    ps2_update_status_locked(vm);
    /* Command replies do not acknowledge a device IRQ. */
    if (raise_irq != 0u) {
        if (aux != 0u) {
            vm_interrupt_eoi(vm, BSP_CORE, INT_MOUSE);
        } else {
            vm_interrupt_eoi(vm, BSP_CORE, INT_KEYBOARD);
        }
    }
    ps2_raise_front_irq_locked(vm);
    vm_shared_unlock(vm);
    return value;
}

void vm_ps2_reassert_irq(VM *vm) {
    if (!vm) {
        return;
    }
    vm_shared_lock(vm);
    /* PS/2 output is a level source: completing an ISR must not lose data
     * that arrived between the handler's final read and its INTC EOI. */
    ps2_update_status_locked(vm);
    ps2_raise_front_irq_locked(vm);
    vm_shared_unlock(vm);
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
    if (console_trace_enabled()) {
        fprintf(stderr, "[serial enqueue] c=0x%02x empty=%d head=%u tail=%u\n",
                (unsigned)c, was_empty, (unsigned)vm->serial_rx_head, (unsigned)tail);
    }
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

int vm_serial_tx_enqueue(VM *vm, uint8_t c) {
    uint16_t head;
    uint16_t next;

    if (!vm) {
        return 0;
    }
    vm_shared_lock(vm);
    head = vm->serial_tx_head;
    next = (uint16_t)((head + 1u) & SERIAL_TX_FIFO_MASK);
    if (next == vm->serial_tx_tail) {
        vm->serial_tx_dropped++;
        vm_shared_unlock(vm);
        return 0;
    }
    vm->serial_tx_fifo[head] = c;
    vm->serial_tx_head = next;
    vm_shared_unlock(vm);
    return 1;
}

int vm_serial_tx_dequeue(VM *vm, uint8_t *c) {
    if (!vm || !c) {
        return 0;
    }
    vm_shared_lock(vm);
    if (vm->serial_tx_tail == vm->serial_tx_head) {
        vm_shared_unlock(vm);
        return 0;
    }
    *c = vm->serial_tx_fifo[vm->serial_tx_tail];
    vm->serial_tx_tail = (uint16_t)((vm->serial_tx_tail + 1u) & SERIAL_TX_FIFO_MASK);
    vm_shared_unlock(vm);
    return 1;
}

int vm_ps2_kbd_enqueue(VM *vm, uint8_t c) {
    return vm_ps2_kbd_enqueue_sequence(vm, &c, 1u);
}

static int vm_ps2_kbd_enqueue_sequence_locked(VM *vm,
                                               const uint8_t *bytes,
                                               size_t count,
                                               int reserve_response_space) {
    int output_enabled;
    int output_queued = 0;
    int legacy_queued = 0;
    output_enabled = vm->ps2_kbd_enabled != 0u &&
                     vm->ps2_kbd_scanning != 0u &&
                     (vm->ps2_config & PS2_CONFIG_FIRST_DISABLED) == 0u;
    if (output_enabled) {
        /* The 8042 stream is authoritative for the modern kernel. Reserve
         * the complete sequence before publishing its first byte. */
        if (reserve_response_space &&
            ps2_fifo_free(vm->ps2_out_head, vm->ps2_out_tail,
                          PS2_OUT_FIFO_MASK) < count + PS2_RESPONSE_RESERVE) {
            return 0;
        }
        output_queued = ps2_output_push_bytes_locked(vm, bytes, count,
                                                       0u, 1u);
        /* Keep the old compatibility path as a best-effort mirror, but never
         * let its backpressure reject an otherwise valid 8042 event. */
        if (output_queued &&
            ps2_fifo_free(vm->ps2_kbd_head, vm->ps2_kbd_tail,
                          PS2_LEGACY_FIFO_MASK) >= count) {
            (void)ps2_legacy_kbd_push_bytes_locked(vm, bytes, count);
        }
    } else {
        legacy_queued = ps2_legacy_kbd_push_bytes_locked(vm, bytes, count);
    }
    return output_enabled ? output_queued : legacy_queued;
}

int vm_ps2_kbd_enqueue_sequence(VM *vm, const uint8_t *bytes, size_t count) {
    int queued;
    if (!vm || !bytes || count == 0u) {
        return 0;
    }
    vm_shared_lock(vm);
    queued = vm_ps2_kbd_enqueue_sequence_locked(vm, bytes, count, 0);
    vm_shared_unlock(vm);
    return queued;
}

int vm_ps2_kbd_enqueue_sequence_if_ready(VM *vm, const uint8_t *bytes,
                                          size_t count) {
    int queued = 0;
    if (!vm || !bytes || count == 0u) {
        return 0;
    }
    vm_shared_lock(vm);
    if (vm->ps2_input_ready != 0u) {
        queued = vm_ps2_kbd_enqueue_sequence_locked(vm, bytes, count, 1);
    }
    vm_shared_unlock(vm);
    return queued;
}

int vm_ps2_mouse_enqueue(VM *vm, uint8_t c) {
    int legacy_queued = 0;
    int output_enabled;
    int output_queued = 0;
    if (!vm) {
        return 0;
    }

    vm_shared_lock(vm);

    output_enabled = vm->ps2_mouse_enabled != 0u &&
                     vm->ps2_mouse_reporting != 0u &&
                     (vm->ps2_config & PS2_CONFIG_SECOND_DISABLED) == 0u;
    if (output_enabled) {
        /* The controller FIFO is authoritative. Do not publish a legacy
         * copy when the authoritative write is rejected; otherwise a retry
         * can leave an invisible duplicate in the compatibility path. */
        output_queued = ps2_output_push_locked(vm, c, 1u, 1u);
        if (output_queued &&
            ps2_fifo_free(vm->ps2_mouse_head, vm->ps2_mouse_tail,
                          PS2_LEGACY_FIFO_MASK) != 0u) {
            const uint16_t head = vm->ps2_mouse_head;
            const uint16_t tail = vm->ps2_mouse_tail;
            const int was_empty = (head == tail);
            vm->ps2_mouse_fifo[head] = c;
            vm->ps2_mouse_head =
                (uint16_t)((head + 1u) & PS2_LEGACY_FIFO_MASK);
            legacy_queued = 1;
            if (was_empty) {
                vm->io[PS2_MOUSE_DATA] = (int)c;
                vm->io[PS2_MOUSE_STATUS] |= PS2_STATUS_RX_READY;
                trigger_interrupt(vm, INT_MOUSE);
            }
        }
    } else {
        const uint16_t head = vm->ps2_mouse_head;
        const uint16_t tail = vm->ps2_mouse_tail;
        const uint16_t next = (uint16_t)((head + 1u) & PS2_LEGACY_FIFO_MASK);
        if (next != tail) {
            const int was_empty = (head == tail);
            vm->ps2_mouse_fifo[head] = c;
            vm->ps2_mouse_head = next;
            legacy_queued = 1;
            if (was_empty) {
                vm->io[PS2_MOUSE_DATA] = (int)c;
                vm->io[PS2_MOUSE_STATUS] |= PS2_STATUS_RX_READY;
                trigger_interrupt(vm, INT_MOUSE);
            }
        }
    }

    vm_shared_unlock(vm);
    return output_enabled ? output_queued : legacy_queued;
}

int vm_ps2_mouse_enqueue_packet(VM *vm, uint8_t flags,
                                uint8_t delta_x, uint8_t delta_y) {
    const uint8_t packet[3] = { flags, delta_x, delta_y };
    int legacy_queued = 0;
    int output_enabled;
    int output_queued = 0;

    if (!vm) {
        return 0;
    }

    vm_shared_lock(vm);
    output_enabled = vm->ps2_mouse_enabled != 0u &&
                     vm->ps2_mouse_reporting != 0u &&
                     (vm->ps2_config & PS2_CONFIG_SECOND_DISABLED) == 0u;
    if (output_enabled) {
        /* Reserve the complete controller packet before mirroring it. */
        output_queued = ps2_output_push_packet_locked(vm, packet);
        if (output_queued &&
            ps2_fifo_free(vm->ps2_mouse_head, vm->ps2_mouse_tail,
                          PS2_LEGACY_FIFO_MASK) >= 3u) {
            (void)ps2_legacy_mouse_push_packet_locked(vm, packet);
        }
    } else {
        legacy_queued = ps2_legacy_mouse_push_packet_locked(vm, packet);
    }
    vm_shared_unlock(vm);

    /* Once the 8042 path is active, report its backpressure rather than the
     * compatibility FIFO's result. Both paths are packet-atomic. */
    return output_enabled ? output_queued : legacy_queued;
}

static int vm_ps2_mouse_enqueue_delta_locked(VM *vm, int32_t delta_x,
                                              int32_t delta_y,
                                              uint8_t buttons,
                                              int reserve_response_space) {
    int64_t remaining_x = delta_x;
    int64_t remaining_y = delta_y;
    const uint64_t abs_x = delta_x < 0 ? (uint64_t)(-(int64_t)delta_x) :
                                         (uint64_t)delta_x;
    const uint64_t abs_y = delta_y < 0 ? (uint64_t)(-(int64_t)delta_y) :
                                         (uint64_t)delta_y;
    const uint64_t x_packets = (abs_x + 126u) / 127u;
    const uint64_t y_packets = (abs_y + 126u) / 127u;
    const uint64_t packet_count =
        (x_packets > y_packets ? x_packets : y_packets) != 0u ?
            (x_packets > y_packets ? x_packets : y_packets) : 1u;
    const uint64_t output_bytes = packet_count * 3u;
    int output_enabled;
    int output_queued = 0;
    int legacy_queued = 0;

    if ((buttons & ~0x07u) != 0u ||
        output_bytes > (uint64_t)PS2_OUT_FIFO_MASK) {
        return 0;
    }

    output_enabled = vm->ps2_mouse_enabled != 0u &&
                     vm->ps2_mouse_reporting != 0u &&
                     (vm->ps2_config & PS2_CONFIG_SECOND_DISABLED) == 0u;
    if (output_enabled &&
        ps2_fifo_free(vm->ps2_out_head, vm->ps2_out_tail,
                      PS2_OUT_FIFO_MASK) <
            output_bytes + (reserve_response_space ? PS2_RESPONSE_RESERVE : 0u)) {
        return 0;
    }
    if (!output_enabled &&
        ps2_fifo_free(vm->ps2_mouse_head, vm->ps2_mouse_tail,
                      PS2_LEGACY_FIFO_MASK) < output_bytes) {
        return 0;
    }

    /* The preflight makes a whole relative-motion update atomic: a caller
     * may retry a failed update without duplicating an earlier packet. */
    const int mirror_legacy = output_enabled &&
        ps2_fifo_free(vm->ps2_mouse_head, vm->ps2_mouse_tail,
                      PS2_LEGACY_FIFO_MASK) >= output_bytes;
    for (uint64_t i = 0u; i < packet_count; i++) {
        int packet_x = (int)remaining_x;
        int packet_y = (int)remaining_y;
        int ps2_y;
        uint8_t packet[3];
        uint8_t flags = 0x08u;
        if (packet_x > 127) packet_x = 127;
        if (packet_x < -127) packet_x = -127;
        if (packet_y > 127) packet_y = 127;
        if (packet_y < -127) packet_y = -127;
        remaining_x -= packet_x;
        remaining_y -= packet_y;
        ps2_y = -packet_y;
        if ((buttons & 0x01u) != 0u) flags |= 0x01u;
        if ((buttons & 0x02u) != 0u) flags |= 0x02u;
        if ((buttons & 0x04u) != 0u) flags |= 0x04u;
        if (packet_x < 0) flags |= 0x10u;
        if (ps2_y < 0) flags |= 0x20u;
        packet[0] = flags;
        packet[1] = (uint8_t)(int8_t)packet_x;
        packet[2] = (uint8_t)(int8_t)ps2_y;
        if (output_enabled) {
            output_queued |= ps2_output_push_packet_locked(vm, packet);
            if (mirror_legacy) {
                legacy_queued |= ps2_legacy_mouse_push_packet_locked(vm, packet);
            }
        } else {
            legacy_queued |= ps2_legacy_mouse_push_packet_locked(vm, packet);
        }
    }
    return output_enabled ? output_queued : legacy_queued;
}

int vm_ps2_mouse_enqueue_delta(VM *vm, int32_t delta_x, int32_t delta_y,
                               uint8_t buttons) {
    int queued;
    if (!vm) {
        return 0;
    }
    vm_shared_lock(vm);
    queued = vm_ps2_mouse_enqueue_delta_locked(vm, delta_x, delta_y, buttons, 0);
    vm_shared_unlock(vm);
    return queued;
}

int vm_ps2_mouse_enqueue_delta_if_ready(VM *vm, int32_t delta_x,
                                        int32_t delta_y, uint8_t buttons) {
    int queued = 0;
    if (!vm) {
        return 0;
    }
    vm_shared_lock(vm);
    if (vm->ps2_input_ready != 0u) {
        queued = vm_ps2_mouse_enqueue_delta_locked(vm, delta_x, delta_y,
                                                    buttons, 1);
    }
    vm_shared_unlock(vm);
    return queued;
}

void accept_io(VM *vm, const int addr, const int value) {
    if (addr < 0 || addr >= IO_SIZE)
        return;
    vm_shared_lock(vm);

    switch (addr) {
    case SCREEN: {
        unsigned char c = (unsigned char)value;
        if (vm->serial_capture_enabled) {
            (void)vm_serial_tx_enqueue(vm, c);
        } else {
            const ssize_t written = write(STDOUT_FILENO, &c, 1);
            (void)written;
        }
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
            if (!vm->ps2_kbd_enabled || !vm->ps2_mouse_enabled ||
                (vm->ps2_config & (PS2_CONFIG_FIRST_IRQ |
                                   PS2_CONFIG_SECOND_IRQ)) !=
                    (PS2_CONFIG_FIRST_IRQ | PS2_CONFIG_SECOND_IRQ)) {
                vm->ps2_input_ready = 0u;
            }
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
            vm->ps2_input_ready = 0u;
            break;
        case 0xAEu:
            vm->ps2_config &= (uint8_t)~PS2_CONFIG_FIRST_DISABLED;
            vm->ps2_kbd_enabled = 1u;
            break;
        case 0xA7u:
            vm->ps2_config |= PS2_CONFIG_SECOND_DISABLED;
            vm->ps2_mouse_enabled = 0u;
            vm->ps2_input_ready = 0u;
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
