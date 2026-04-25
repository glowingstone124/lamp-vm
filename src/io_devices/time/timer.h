//
// Created by Max Wang on 2026/2/14.
//

#ifndef VM_TIMER_H
#define VM_TIMER_H

#include <stdint.h>

#include "../../vm.h"

enum TIME_CONTROL_OFFSET {
    REALTIME = 0x0,
    MONOTONIC = 0x1,
    BOOT = 0x2,
};

struct time_struct {
    uint32_t lo;
    uint32_t hi;
};

struct time_struct get_timer(VM *vm, uint32_t timer);

void handle_programmable_tick(VM *vm, uint32_t value);
void *timer_tick(void *arg);

#endif // VM_TIMER_H
