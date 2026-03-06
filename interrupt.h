#pragma once
#ifndef VM_INTERRUPT_H
#define VM_INTERRUPT_H

#include <stdint.h>
#include "vm.h"

#define BSP_CORE 0
void vm_handle_interrupts(VM *vm);
void init_ivt(VM *vm);
void register_isr(VM *vm, uint32_t int_no, uint64_t isr_ip);
void trigger_interrupt(VM *vm, uint32_t int_no);
void trigger_interrupt_target(VM *vm, int core_id, uint32_t int_no);

void vm_enter_interrupt(VM *vm, uint32_t int_no);
void vm_iret(VM *vm);

uint32_t vm_interrupt_read_pending32(VM *vm, int core_id, uint32_t reg_index);
void vm_interrupt_raise_pending32(VM *vm, int core_id, uint32_t reg_index, uint32_t value);
uint32_t vm_interrupt_read_enable32(VM *vm, int core_id, uint32_t reg_index);
void vm_interrupt_write_enable32(VM *vm, int core_id, uint32_t reg_index, uint32_t value);
uint32_t vm_interrupt_read_priority(VM *vm, uint32_t int_no);
void vm_interrupt_write_priority(VM *vm, uint32_t int_no, uint32_t priority);
void vm_interrupt_eoi(VM *vm, int core_id, uint32_t int_no);

typedef enum InterruptNo {
    INT_KEYBOARD        = 0x00,
    INT_DIVIDE_BY_ZERO  = 0x01,
    INT_DISK_COMPLETE   = 0x02,
    INT_SERIAL          = 0x03,
    INT_TIMER           = 0x04,
} InterruptNo;

#endif // VM_INTERRUPT_H
