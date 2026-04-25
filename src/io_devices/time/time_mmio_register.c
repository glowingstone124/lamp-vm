//
// Created by Max Wang on 2026/1/18.
//

#include "time_mmio_register.h"

#include <stdlib.h>

#include "timer.h"
#include "../../panic.h"

uint32_t time_read32(VM *vm, uint32_t addr) {
    uint32_t offset = addr - TIME_BASE;

    if (offset == 0x00) return 1; // Control

    // Realtime
    if (offset == 0x04) { // low: latch
        struct time_struct t = get_timer(vm, REALTIME);
        return t.lo;
    }
    if (offset == 0x08) { // high: read same latched value
        return (uint32_t)(vm->latched_realtime >> 32);
    }

    // Monotonic
    if (offset == 0x0C) { // low
        struct time_struct t = get_timer(vm, MONOTONIC);
        return t.lo;
    }
    if (offset == 0x10) { // high
        return (uint32_t)(vm->latched_monotonic >> 32);
    }

    // Boot
    if (offset == 0x14) { // low
        struct time_struct t = get_timer(vm, BOOT);
        return t.lo;
    }
    if (offset == 0x18) { // high
        return (uint32_t)(vm->latched_boottime >> 32);
    }

    printf("Unknown MMIO Register Offset: 0x%08x\n", offset);
    return 0;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
void time_write32(VM *vm, uint32_t addr, uint32_t value) {
    if (addr == TIME_BASE) {
        handle_programmable_tick(vm,value);
        return;
    }

    fprintf(stderr, "Attempted to write to read-only TIME MMIO at 0x%08x\n", addr);
    atomic_set_vm_halt(vm, 1);;
}


static void initialize_timer_related(VM *vm) {
    atomic_init(&vm->timer_enabled, false);
    atomic_init(&vm->timer_period_us, 0u);
    atomic_init(&vm->timer_next_deadline_ns, 0u);
    atomic_init(&vm->timer_thread_running, true);
    vm->timer_thread_started = 0;

    if (pthread_create(&vm->timer_worker_thread, NULL, timer_tick, vm) != 0) {
        panic("Failed to create timer worker", vm);
    }
    vm->timer_thread_started = 1;
}
#pragma GCC diagnostic pop

void register_time_mmio(VM *vm) {
    static MMIO_Device time_dev;
    time_dev.start = TIME_BASE;
    time_dev.end = TIME_BASE + 23;
    time_dev.read32 = time_read32;
    time_dev.write32 = time_write32;
    initialize_timer_related(vm);
    if (vm->mmio_count < MAX_MMIO_DEVICES) {
        vm->mmio_devices[vm->mmio_count++] = &time_dev;
        printf("Registered VM Timer to MMIO ID %d\n", vm->mmio_count);
    }
}
