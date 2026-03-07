#ifndef LAMP_KERNEL_SPINLOCK_H
#define LAMP_KERNEL_SPINLOCK_H

#include "types.h"

typedef struct {
    volatile uint32_t v;
    uint32_t irq_mask_prev;
} spinlock_t;

void spinlock_init(spinlock_t *lock);
void spinlock_lock(spinlock_t *lock);
void spinlock_unlock(spinlock_t *lock);

#endif
