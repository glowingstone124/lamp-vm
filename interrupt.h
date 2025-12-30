//
// Created by Max Wang on 2025/12/30.
//

#ifndef VM_INTERRUPT_H
#define VM_INTERRUPT_H

void vm_handle_interrupts(VM *vm);
void init_ivt(VM *vm);
void register_isr(VM *vm, int int_no, int isr_ip);
#endif //VM_INTERRUPT_H