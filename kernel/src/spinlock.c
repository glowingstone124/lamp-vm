#include "../include/kernel/spinlock.h"
#include "../include/kernel/platform.h"

static inline __attribute__((always_inline)) uint32_t spin_try_acquire(volatile uint32_t *ptr) {
    uint32_t expected = 0u;
    const uint32_t desired = 1u;
    uint32_t old = expected;
    __asm__ volatile (
        "cas %0, %1, %2, %3, 0\n"
        : "+r"(old)
        : "r"(ptr), "r"(desired), "r"(desired)
        : "memory"
    );
    return (old == expected) ? 1u : 0u;
}

static inline __attribute__((always_inline)) uint32_t spin_load_acquire(volatile uint32_t *ptr) {
    uint32_t v = 0u;
    __asm__ volatile (
        "ldar %0, %1, 0\n"
        : "=r"(v)
        : "r"(ptr)
        : "memory"
    );
    return v;
}

static inline __attribute__((always_inline)) uint32_t spin_irq_mask_read(void) {
    uint32_t v = 0u;
    __asm__ volatile (
        "in %0, %1\n"
        : "=r"(v)
        : "r"(IO_CPU_CTX_IRQ_MASK)
    );
    return v & 1u;
}

static inline __attribute__((always_inline)) void spin_irq_mask_write(uint32_t masked) {
    const uint32_t v = masked ? 1u : 0u;
    __asm__ volatile (
        "out %0, %1\n"
        :
        : "r"(v), "r"(IO_CPU_CTX_IRQ_MASK)
        : "memory"
    );
}

void spinlock_init(spinlock_t *lock) {
    if (!lock) {
        return;
    }
    lock->v = 0u;
    lock->irq_mask_prev = 0u;
}

void spinlock_lock(spinlock_t *lock) {
    uint32_t irq_mask_prev;
    if (!lock) {
        return;
    }

    irq_mask_prev = spin_irq_mask_read();
    spin_irq_mask_write(1u);

    for (;;) {
        if (spin_try_acquire(&lock->v)) {
            lock->irq_mask_prev = irq_mask_prev;
            return;
        }
        while (spin_load_acquire(&lock->v) != 0u) {
            __asm__ volatile ("pause\n" ::: "memory");
        }
    }
}

void spinlock_unlock(spinlock_t *lock) {
    uint32_t irq_mask_prev;
    uint32_t zero = 0u;
    if (!lock) {
        return;
    }
    irq_mask_prev = lock->irq_mask_prev;
    __asm__ volatile (
        "stlr %0, %1, 0\n"
        :
        : "r"(zero), "r"(&lock->v)
        : "memory"
    );
    spin_irq_mask_write(irq_mask_prev);
}
