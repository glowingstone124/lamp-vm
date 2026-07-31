//
// Created by Max Wang on 2026/2/14.
//

#include "timer.h"

#include <errno.h>
#include <stdint.h>
#include <time.h>
#include "../../vm.h"
#include "../../interrupt.h"

#define TIMER_MIN_PERIOD_US 1000u

struct time_struct get_timer(VM *vm, uint32_t timer) {
    switch (timer) {
        case REALTIME: {
            vm->latched_realtime = host_unix_time_ns();
            return (struct time_struct){
                .lo = (uint32_t)(vm->latched_realtime & 0xFFFFFFFFu),
                .hi = (uint32_t)(vm->latched_realtime >> 32),
            };
        }
        case MONOTONIC: {
            vm->latched_monotonic = host_monotonic_time_ns();
            return (struct time_struct){
                .lo = (uint32_t)(vm->latched_monotonic & 0xFFFFFFFFu),
                .hi = (uint32_t)(vm->latched_monotonic >> 32),
            };
        }
        case BOOT: {
            vm->latched_boottime = host_monotonic_time_ns() - vm->start_monotonic_ns;
            return (struct time_struct){
                .lo = (uint32_t)(vm->latched_boottime & 0xFFFFFFFFu),
                .hi = (uint32_t)(vm->latched_boottime >> 32),
            };
        }
        default:
            return (struct time_struct){ .lo = 0, .hi = 0 };
    }
}

static inline void sleep_us_interruptible(uint32_t us) {
    struct timespec ts;
    ts.tv_sec = us / 1000000u;
    ts.tv_nsec = (long)(us % 1000000u) * 1000l;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

void *timer_tick(void *arg) {
    VM *vm = (VM *)arg;
    while (atomic_load(&vm->timer_thread_running)) {
        if (atomic_is_vm_stopped(vm)) {
            break;
        }

        if (!atomic_load(&vm->timer_enabled)) {
            sleep_us_interruptible(1000);
            atomic_store(&vm->timer_next_deadline_ns, 0);
            continue;
        }

        uint32_t period_us = atomic_load(&vm->timer_period_us);
        if (period_us == 0) {
            atomic_store(&vm->timer_enabled, false);
            atomic_store(&vm->timer_next_deadline_ns, 0);
            continue;
        }

        const uint64_t step = (uint64_t)period_us * 1000ull;
        uint64_t now = host_monotonic_time_ns();
        uint64_t deadline = atomic_load(&vm->timer_next_deadline_ns);

        if (deadline == 0) {
            deadline = now + step;
            atomic_store(&vm->timer_next_deadline_ns, deadline);
        }

        if (now < deadline) {
            const uint64_t remain_ns = deadline - now;
            uint32_t sleep_us = (remain_ns / 1000ull > 1000ull) ? 1000u : (uint32_t)(remain_ns / 1000ull);
            if (sleep_us == 0) {
                sleep_us = 1;
            }
            sleep_us_interruptible(sleep_us);
            continue;
        }

        trigger_interrupt(vm, INT_TIMER);

        deadline += step;
        now = host_monotonic_time_ns();
        if (deadline < now) {
            const uint64_t behind = now - deadline;
            const uint64_t skip = behind / step + 1ull;
            deadline += step * skip;
        }
        atomic_store(&vm->timer_next_deadline_ns, deadline);
    }
    return NULL;
}

void handle_programmable_tick(VM *vm, uint32_t value) {
    if (value == 0) {
        atomic_store(&vm->timer_enabled, false);
        atomic_store(&vm->timer_period_us, 0);
        atomic_store(&vm->timer_next_deadline_ns, 0);
        return;
    }

    if (value < TIMER_MIN_PERIOD_US) {
        value = TIMER_MIN_PERIOD_US;
    }

    atomic_store(&vm->timer_period_us, value);
    atomic_store(&vm->timer_next_deadline_ns, 0);
    atomic_store(&vm->timer_enabled, true);
}
