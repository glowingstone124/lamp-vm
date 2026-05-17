#ifndef LAMP_KERNEL_IRQ_H
#define LAMP_KERNEL_IRQ_H

#include "types.h"

typedef struct trap_frame {
    uint32_t irq_no;
    uint32_t dispatch_count;
    uint32_t tick_snapshot;
    uint32_t reserved;
} trap_frame_t;

void irq_default(uint32_t irq_no);
void irq_divide_by_zero(uint32_t irq_no);
void irq_disk_complete(uint32_t irq_no);
void irq_serial(uint32_t irq_no);
void irq_serial_drain_rx(void);
void irq_keyboard(uint32_t irq_no);
void irq_mouse(uint32_t irq_no);
void irq_timer(uint32_t irq_no);
void irq_syscall(uint32_t irq_no);

void irq_common_entry(uint32_t irq_no);
void irq_common_entry_from_stub(void);
void irq_stub_entry(void);

void irq_enable(uint32_t irq_no);
void irq_disable(uint32_t irq_no);
void irq_set_priority(uint32_t irq_no, uint32_t priority);
void irq_eoi(uint32_t irq_no);

void irq_input_init(void);
uint32_t irq_input_dropped(void);
const trap_frame_t *irq_last_trap_frame(void);
uint32_t irq_saved_user_csp(void);
uint32_t irq_saved_user_dsp(void);

#endif
