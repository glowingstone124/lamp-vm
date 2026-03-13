#include "sched_internal.h"

/*
 * Scheduler runs tasks via cooperative context switch:
 * task code runs until it yields/blocks/exits and switches back to scheduler context.
 */

volatile unsigned int g_ticks;
volatile unsigned int g_need_resched;
sched_task_slot_t g_tasks[SCHED_MAX_TASKS];
uint32_t g_cpu_current_idx[SCHED_MAX_CPUS];
uint32_t g_next_tid;
uint32_t g_spawn_cpu_rr;
sched_runq_t g_runq[SCHED_MAX_CPUS];
spinlock_t g_sched_lock;
uint32_t g_stack_pool_bitmap[SCHED_STACK_BITMAP_WORDS];
uint32_t g_stack_pool_reserved;
sched_stack_ctx_t g_sched_ctx[SCHED_MAX_CPUS];

#define SCHED_CTX_OFF_CALL_BASE 0
#define SCHED_CTX_OFF_DATA_BASE 4
#define SCHED_CTX_OFF_ISR_BASE 8
#define SCHED_CTX_OFF_CSP 12
#define SCHED_CTX_OFF_DSP 16
#define SCHED_CTX_OFF_ISP 20
#define SCHED_CTX_OFF_IRQ_MASK 24
#define SCHED_CTX_OFF_POOL_SLOT 28
#define SCHED_CTX_OFF_VALID 32
#define SCHED_CTX_OFF_REG0 36
#define SCHED_CTX_OFF_REG1 40
#define SCHED_CTX_OFF_REG2 44
#define SCHED_CTX_OFF_REG3 48
#define SCHED_CTX_OFF_REG4 52
#define SCHED_CTX_OFF_REG5 56
#define SCHED_CTX_OFF_REG6 60
#define SCHED_CTX_OFF_REG7 64
#define SCHED_CTX_OFF_REG8 68
#define SCHED_CTX_OFF_REG9 72
#define SCHED_CTX_OFF_REG10 76
#define SCHED_CTX_OFF_REG11 80
#define SCHED_CTX_OFF_REG12 84
#define SCHED_CTX_OFF_REG13 88
#define SCHED_CTX_OFF_REG14 92
#define SCHED_CTX_OFF_REG15 96
#define SCHED_CTX_OFF_REG16 100
#define SCHED_CTX_OFF_REG17 104
#define SCHED_CTX_OFF_REG18 108
#define SCHED_CTX_OFF_REG19 112
#define SCHED_CTX_OFF_REG20 116
#define SCHED_CTX_OFF_REG21 120
#define SCHED_CTX_OFF_REG22 124
#define SCHED_CTX_OFF_REG23 128
#define SCHED_CTX_OFF_REG24 132
#define SCHED_CTX_OFF_REG25 136
#define SCHED_CTX_OFF_REG26 140
#define SCHED_CTX_OFF_REG27 144
#define SCHED_CTX_OFF_REG28 148
#define SCHED_CTX_OFF_REG29 152
#define SCHED_CTX_OFF_REG30 156
#define SCHED_CTX_OFF_REG31 160

#define SCHED_IO_CALL_BASE 0xF4
#define SCHED_IO_DATA_BASE 0xF5
#define SCHED_IO_ISR_BASE 0xF6
#define SCHED_IO_CSP 0xF0
#define SCHED_IO_DSP 0xF1
#define SCHED_IO_ISP 0xF3
#define SCHED_IO_IRQ_MASK 0xF2

#define SCHED_STR1(x) #x
#define SCHED_STR(x) SCHED_STR1(x)

extern void sched_ctx_swap(sched_stack_ctx_t *from, const sched_stack_ctx_t *to);

static void sched_task_bootstrap(void);

__asm__(
    ".text\n"
    ".globl sched_ctx_swap\n"
    "sched_ctx_swap:\n"
    "  mov r2, r0\n"
    "  mov r3, r1\n"
    "  movi r5, " SCHED_STR(SCHED_IO_IRQ_MASK) "\n"
    "  in r4, r5\n"
    "  store32 r4, r2, " SCHED_STR(SCHED_CTX_OFF_IRQ_MASK) "\n"
    "  movi r4, 1\n"
    "  out r4, r5\n"
    "  movi r5, " SCHED_STR(SCHED_IO_CALL_BASE) "\n"
    "  in r4, r5\n"
    "  store32 r4, r2, " SCHED_STR(SCHED_CTX_OFF_CALL_BASE) "\n"
    "  movi r5, " SCHED_STR(SCHED_IO_DATA_BASE) "\n"
    "  in r4, r5\n"
    "  store32 r4, r2, " SCHED_STR(SCHED_CTX_OFF_DATA_BASE) "\n"
    "  movi r5, " SCHED_STR(SCHED_IO_ISR_BASE) "\n"
    "  in r4, r5\n"
    "  store32 r4, r2, " SCHED_STR(SCHED_CTX_OFF_ISR_BASE) "\n"
    "  movi r5, " SCHED_STR(SCHED_IO_CSP) "\n"
    "  in r4, r5\n"
    "  store32 r4, r2, " SCHED_STR(SCHED_CTX_OFF_CSP) "\n"
    "  movi r5, " SCHED_STR(SCHED_IO_DSP) "\n"
    "  in r4, r5\n"
    "  store32 r4, r2, " SCHED_STR(SCHED_CTX_OFF_DSP) "\n"
    "  movi r5, " SCHED_STR(SCHED_IO_ISP) "\n"
    "  in r4, r5\n"
    "  store32 r4, r2, " SCHED_STR(SCHED_CTX_OFF_ISP) "\n"
    "  store32 r8, r2, " SCHED_STR(SCHED_CTX_OFF_REG8) "\n"
    "  store32 r9, r2, " SCHED_STR(SCHED_CTX_OFF_REG9) "\n"
    "  store32 r10, r2, " SCHED_STR(SCHED_CTX_OFF_REG10) "\n"
    "  store32 r11, r2, " SCHED_STR(SCHED_CTX_OFF_REG11) "\n"
    "  store32 r12, r2, " SCHED_STR(SCHED_CTX_OFF_REG12) "\n"
    "  store32 r13, r2, " SCHED_STR(SCHED_CTX_OFF_REG13) "\n"
    "  store32 r14, r2, " SCHED_STR(SCHED_CTX_OFF_REG14) "\n"
    "  store32 r15, r2, " SCHED_STR(SCHED_CTX_OFF_REG15) "\n"
    "  store32 r16, r2, " SCHED_STR(SCHED_CTX_OFF_REG16) "\n"
    "  store32 r17, r2, " SCHED_STR(SCHED_CTX_OFF_REG17) "\n"
    "  store32 r18, r2, " SCHED_STR(SCHED_CTX_OFF_REG18) "\n"
    "  store32 r19, r2, " SCHED_STR(SCHED_CTX_OFF_REG19) "\n"
    "  store32 r20, r2, " SCHED_STR(SCHED_CTX_OFF_REG20) "\n"
    "  store32 r21, r2, " SCHED_STR(SCHED_CTX_OFF_REG21) "\n"
    "  store32 r22, r2, " SCHED_STR(SCHED_CTX_OFF_REG22) "\n"
    "  store32 r23, r2, " SCHED_STR(SCHED_CTX_OFF_REG23) "\n"
    "  store32 r24, r2, " SCHED_STR(SCHED_CTX_OFF_REG24) "\n"
    "  store32 r25, r2, " SCHED_STR(SCHED_CTX_OFF_REG25) "\n"
    "  store32 r26, r2, " SCHED_STR(SCHED_CTX_OFF_REG26) "\n"
    "  store32 r27, r2, " SCHED_STR(SCHED_CTX_OFF_REG27) "\n"
    "  store32 r28, r2, " SCHED_STR(SCHED_CTX_OFF_REG28) "\n"
    "  store32 r29, r2, " SCHED_STR(SCHED_CTX_OFF_REG29) "\n"
    "  store32 r30, r2, " SCHED_STR(SCHED_CTX_OFF_REG30) "\n"
    "  store32 r31, r2, " SCHED_STR(SCHED_CTX_OFF_REG31) "\n"
    "  movi r5, " SCHED_STR(SCHED_IO_CALL_BASE) "\n"
    "  load32 r4, r3, " SCHED_STR(SCHED_CTX_OFF_CALL_BASE) "\n"
    "  out r4, r5\n"
    "  movi r5, " SCHED_STR(SCHED_IO_DATA_BASE) "\n"
    "  load32 r4, r3, " SCHED_STR(SCHED_CTX_OFF_DATA_BASE) "\n"
    "  out r4, r5\n"
    "  movi r5, " SCHED_STR(SCHED_IO_ISR_BASE) "\n"
    "  load32 r4, r3, " SCHED_STR(SCHED_CTX_OFF_ISR_BASE) "\n"
    "  out r4, r5\n"
    "  movi r5, " SCHED_STR(SCHED_IO_CSP) "\n"
    "  load32 r4, r3, " SCHED_STR(SCHED_CTX_OFF_CSP) "\n"
    "  out r4, r5\n"
    "  movi r5, " SCHED_STR(SCHED_IO_DSP) "\n"
    "  load32 r4, r3, " SCHED_STR(SCHED_CTX_OFF_DSP) "\n"
    "  out r4, r5\n"
    "  movi r5, " SCHED_STR(SCHED_IO_ISP) "\n"
    "  load32 r4, r3, " SCHED_STR(SCHED_CTX_OFF_ISP) "\n"
    "  out r4, r5\n"
    "  load32 r8, r3, " SCHED_STR(SCHED_CTX_OFF_REG8) "\n"
    "  load32 r9, r3, " SCHED_STR(SCHED_CTX_OFF_REG9) "\n"
    "  load32 r10, r3, " SCHED_STR(SCHED_CTX_OFF_REG10) "\n"
    "  load32 r11, r3, " SCHED_STR(SCHED_CTX_OFF_REG11) "\n"
    "  load32 r12, r3, " SCHED_STR(SCHED_CTX_OFF_REG12) "\n"
    "  load32 r13, r3, " SCHED_STR(SCHED_CTX_OFF_REG13) "\n"
    "  load32 r14, r3, " SCHED_STR(SCHED_CTX_OFF_REG14) "\n"
    "  load32 r15, r3, " SCHED_STR(SCHED_CTX_OFF_REG15) "\n"
    "  load32 r16, r3, " SCHED_STR(SCHED_CTX_OFF_REG16) "\n"
    "  load32 r17, r3, " SCHED_STR(SCHED_CTX_OFF_REG17) "\n"
    "  load32 r18, r3, " SCHED_STR(SCHED_CTX_OFF_REG18) "\n"
    "  load32 r19, r3, " SCHED_STR(SCHED_CTX_OFF_REG19) "\n"
    "  load32 r20, r3, " SCHED_STR(SCHED_CTX_OFF_REG20) "\n"
    "  load32 r21, r3, " SCHED_STR(SCHED_CTX_OFF_REG21) "\n"
    "  load32 r22, r3, " SCHED_STR(SCHED_CTX_OFF_REG22) "\n"
    "  load32 r23, r3, " SCHED_STR(SCHED_CTX_OFF_REG23) "\n"
    "  load32 r24, r3, " SCHED_STR(SCHED_CTX_OFF_REG24) "\n"
    "  load32 r25, r3, " SCHED_STR(SCHED_CTX_OFF_REG25) "\n"
    "  load32 r26, r3, " SCHED_STR(SCHED_CTX_OFF_REG26) "\n"
    "  load32 r27, r3, " SCHED_STR(SCHED_CTX_OFF_REG27) "\n"
    "  load32 r28, r3, " SCHED_STR(SCHED_CTX_OFF_REG28) "\n"
    "  load32 r29, r3, " SCHED_STR(SCHED_CTX_OFF_REG29) "\n"
    "  load32 r30, r3, " SCHED_STR(SCHED_CTX_OFF_REG30) "\n"
    "  load32 r31, r3, " SCHED_STR(SCHED_CTX_OFF_REG31) "\n"
    "  movi r5, " SCHED_STR(SCHED_IO_IRQ_MASK) "\n"
    "  load32 r4, r3, " SCHED_STR(SCHED_CTX_OFF_IRQ_MASK) "\n"
    "  out r4, r5\n"
    "  ret\n"
);

static void sched_stack_zero_slot(uint32_t base) {
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)base;
    for (uint32_t i = 0u; i < (uint32_t)VM_STACK_SLOT_BYTES; i++) {
        p[i] = 0u;
    }
}

static void sched_stack_pool_init_locked(void) {
    uint32_t reserve = smp_target_cpus();
    if (reserve == 0u) {
        reserve = 1u;
    }
    if (reserve > VM_STACK_POOL_SLOTS) {
        reserve = VM_STACK_POOL_SLOTS;
    }
    for (uint32_t i = 0u; i < SCHED_STACK_BITMAP_WORDS; i++) {
        g_stack_pool_bitmap[i] = 0u;
    }
    g_stack_pool_reserved = reserve;
    for (uint32_t i = 0u; i < reserve; i++) {
        sched_stack_bitmap_set(i);
    }
}

int sched_stack_alloc_locked(sched_stack_ctx_t *ctx_out) {
    if (!ctx_out) {
        return -1;
    }
    for (uint32_t slot = g_stack_pool_reserved; slot < VM_STACK_POOL_SLOTS; slot++) {
        uint32_t base;
        if (sched_stack_bitmap_tst(slot)) {
            continue;
        }
        sched_stack_bitmap_set(slot);
        base = sched_stack_pool_slot_base(slot);
        sched_stack_zero_slot(base);
        ctx_out->call_base = base;
        ctx_out->data_base = base + VM_CALL_STACK_BYTES;
        ctx_out->isr_base = base + (VM_CALL_STACK_BYTES + VM_DATA_STACK_BYTES);
        ctx_out->csp = VM_CALL_STACK_ENTRIES;
        ctx_out->dsp = VM_DATA_STACK_ENTRIES;
        ctx_out->isp = VM_ISR_STACK_ENTRIES;
        ctx_out->irq_masked = 0u;
        ctx_out->pool_slot = slot;
        ctx_out->valid = 1u;
        ctx_out->regs[30] = base + VM_STACK_SLOT_BYTES;
        ctx_out->regs[31] = base + VM_STACK_SLOT_BYTES;
        return 0;
    }
    return -1;
}

void sched_stack_prepare_bootstrap_locked(sched_task_slot_t *slot) {
    uint32_t boot_addr;
    uint32_t call_idx;
    uint32_t ret_addr;
    uint32_t text_base;
    if (!slot || !slot->stack_ctx.valid) {
        return;
    }
    call_idx = VM_CALL_STACK_ENTRIES - 1u;
    ret_addr = slot->stack_ctx.call_base + call_idx * 8u;
    boot_addr = (uint32_t)(uintptr_t)&sched_task_bootstrap;
    text_base = (uint32_t)(uintptr_t)__text_start;
    if (boot_addr < text_base) {
        boot_addr += text_base;
    }
    *(volatile uint32_t *)(uintptr_t)(ret_addr + 0u) = boot_addr;
    *(volatile uint32_t *)(uintptr_t)(ret_addr + 4u) = 0u;
    slot->stack_ctx.csp = call_idx;
    slot->stack_ctx.dsp = VM_DATA_STACK_ENTRIES;
    slot->stack_ctx.isp = VM_ISR_STACK_ENTRIES;
}

void sched_stack_free_locked(sched_task_slot_t *slot) {
    if (!slot || !slot->stack_ctx.valid) {
        return;
    }
    if (slot->stack_ctx.pool_slot >= VM_STACK_POOL_SLOTS) {
        return;
    }
    if (slot->stack_ctx.pool_slot < g_stack_pool_reserved) {
        return;
    }
    sched_stack_bitmap_clr(slot->stack_ctx.pool_slot);
    slot->stack_ctx.valid = 0u;
}

void sched_clear_task(sched_task_slot_t *slot) {
    uint32_t i;
    volatile uint8_t *bytes;
    if (!slot) {
        return;
    }
    bytes = (volatile uint8_t *)(uintptr_t)slot;
    for (i = 0u; i < (uint32_t)sizeof(*slot); i++) {
        bytes[i] = 0u;
    }
}

uint32_t sched_cpu_cap(void) {
    uint32_t n = smp_online_cpus();
    if (n == 0u) {
        n = 1u;
    }
    if (n > SCHED_MAX_CPUS) {
        n = SCHED_MAX_CPUS;
    }
    return n;
}

uint32_t sched_cpu_normalize(uint32_t cpu_id) {
    uint32_t cap = sched_cpu_cap();
    if (cpu_id >= cap) {
        return 0u;
    }
    return cpu_id;
}

void sched_mark_resched_all(void) {
    g_need_resched = 1u;
}

void sched_runq_add(uint32_t cpu_id, uint32_t idx) {
    sched_runq_t *rq;
    cpu_id = sched_cpu_normalize(cpu_id);
    if (idx == 0u || idx >= SCHED_MAX_TASKS) {
        return;
    }
    rq = &g_runq[cpu_id];
    spinlock_lock(&rq->lock);
    rq->bits[idx / 32u] |= (1u << (idx % 32u));
    spinlock_unlock(&rq->lock);
}

void sched_runq_del(uint32_t cpu_id, uint32_t idx) {
    sched_runq_t *rq;
    cpu_id = sched_cpu_normalize(cpu_id);
    if (idx == 0u || idx >= SCHED_MAX_TASKS) {
        return;
    }
    rq = &g_runq[cpu_id];
    spinlock_lock(&rq->lock);
    rq->bits[idx / 32u] &= ~(1u << (idx % 32u));
    spinlock_unlock(&rq->lock);
}

static uint32_t sched_runq_pick_local(uint32_t cpu_id, uint32_t start_idx) {
    uint32_t idx = 0u;
    sched_runq_t *rq;
    cpu_id = sched_cpu_normalize(cpu_id);
    rq = &g_runq[cpu_id];
    spinlock_lock(&rq->lock);
    for (uint32_t off = 1u; off <= SCHED_MAX_TASKS; off++) {
        uint32_t cand = (start_idx + off) % SCHED_MAX_TASKS;
        if (cand == 0u) {
            continue;
        }
        if ((rq->bits[cand / 32u] & (1u << (cand % 32u))) != 0u) {
            idx = cand;
            break;
        }
    }
    spinlock_unlock(&rq->lock);
    return idx;
}

static uint32_t sched_runq_steal_to(uint32_t dst_cpu_id) {
    (void)dst_cpu_id;
    /*
     * Context-switch mode currently keeps tasks on their assigned run_cpu.
     * Cross-core steal can race with in-flight task->scheduler switches.
     */
    return 0u;
#if 0
    uint32_t stolen = 0u;
    uint32_t cap = sched_cpu_cap();
    dst_cpu_id = sched_cpu_normalize(dst_cpu_id);
    for (uint32_t src = 0u; src < cap; src++) {
        sched_runq_t *rq;
        if (src == dst_cpu_id) {
            continue;
        }
        rq = &g_runq[src];
        spinlock_lock(&rq->lock);
        for (uint32_t idx = 1u; idx < SCHED_MAX_TASKS; idx++) {
            if ((rq->bits[idx / 32u] & (1u << (idx % 32u))) == 0u) {
                continue;
            }
            rq->bits[idx / 32u] &= ~(1u << (idx % 32u));
            stolen = idx;
            break;
        }
        spinlock_unlock(&rq->lock);
        if (stolen != 0u) {
            g_tasks[stolen].run_cpu = dst_cpu_id;
            sched_runq_add(dst_cpu_id, stolen);
            return stolen;
        }
    }
    return 0u;
#endif
}

uint32_t sched_slot_index(const sched_task_slot_t *slot) {
    if (!slot) {
        return 0u;
    }
    return (uint32_t)(slot - &g_tasks[0]);
}

static void sched_idle_task(sched_task_t *task, void *arg) {
    (void)task;
    (void)arg;
    __asm__ __volatile__("" ::: "memory");
}

int sched_alloc_slot(void) {
    for (uint32_t i = 1u; i < SCHED_MAX_TASKS; i++) {
        sched_task_slot_t *slot = &g_tasks[i];
        if (!slot->used) {
            return (int)i;
        }
    }
    return -1;
}

static uint32_t sched_idx_runnable(uint32_t idx) {
    sched_task_slot_t *slot;
    if (idx == 0u) {
        return 0u;
    }
    if (idx >= SCHED_MAX_TASKS) {
        return 0u;
    }
    slot = &g_tasks[idx];
    if (!slot->used) {
        return 0u;
    }
    return slot->pub.state == SCHED_TASK_RUNNABLE;
}

static uint32_t sched_pick_next_idx(uint32_t cpu_id, uint32_t start_idx) {
    uint32_t idx = sched_runq_pick_local(cpu_id, start_idx);
    if (idx != 0u) {
        return idx;
    }
    idx = sched_runq_steal_to(cpu_id);
    if (idx != 0u) {
        return idx;
    }
    return 0u;
}

sched_task_slot_t *sched_current_slot(void) {
    uint32_t idx;
    sched_task_slot_t *slot;
    idx = *sched_cpu_current_idx_ptr();
    if (idx >= SCHED_MAX_TASKS) {
        return 0;
    }
    slot = &g_tasks[idx];
    if (!slot->used) {
        return 0;
    }
    return slot;
}

sched_task_slot_t *sched_current_slot_fd_locked(void) {
    sched_task_slot_t *slot = sched_current_slot();
    if (!slot) {
        return 0;
    }
    spinlock_lock(&slot->fd_lock);
    return slot;
}

void sched_switch_current_to_scheduler(void) {
    uint32_t cpu_id = sched_cpu_id();
    uint32_t idx = *sched_cpu_current_idx_ptr();
    sched_task_slot_t *slot;
    if (idx == 0u || idx >= SCHED_MAX_TASKS) {
        return;
    }
    slot = &g_tasks[idx];
    if (!slot->used || slot->is_idle || !slot->stack_ctx.valid) {
        return;
    }
    sched_ctx_swap(&slot->stack_ctx, &g_sched_ctx[cpu_id]);
}

static void sched_task_bootstrap(void) {
    for (;;) {
        uint32_t idx = *sched_cpu_current_idx_ptr();
        uint32_t cpu_id = sched_cpu_id();
        uint32_t state = SCHED_TASK_UNUSED;
        sched_task_slot_t *slot = sched_current_slot();
        if (!slot || slot->is_idle || !slot->entry) {
            sched_exit_code(0xFFu);
            for (;;) {
                __asm__ volatile("pause\n" ::: "memory");
            }
        }

        slot->entry(&slot->pub, slot->pub.arg);

        spinlock_lock(&g_sched_lock);
        if (idx < SCHED_MAX_TASKS && g_tasks[idx].used) {
            slot = &g_tasks[idx];
            state = slot->pub.state;
            if (state == SCHED_TASK_RUNNING) {
                slot->pub.state = SCHED_TASK_RUNNABLE;
                slot->run_cpu = cpu_id;
                sched_runq_add(slot->run_cpu, idx);
                slot->quantum_used = 0u;
                sched_mark_resched_all();
                state = slot->pub.state;
            }
        }
        spinlock_unlock(&g_sched_lock);

        if (state == SCHED_TASK_ZOMBIE) {
            sched_switch_current_to_scheduler();
            for (;;) {
                __asm__ volatile("pause\n" ::: "memory");
            }
        }

        sched_switch_current_to_scheduler();
    }
}

void sched_init(void) {
    sched_task_slot_t *root;
    spinlock_init(&g_sched_lock);
    spinlock_lock(&g_sched_lock);

    for (uint32_t i = 0u; i < SCHED_MAX_TASKS; i++) {
        sched_clear_task(&g_tasks[i]);
    }

    g_ticks = 0u;
    g_need_resched = 0u;
    sched_stack_pool_init_locked();
    for (uint32_t i = 0u; i < SCHED_MAX_CPUS; i++) {
        g_cpu_current_idx[i] = 0u;
        spinlock_init(&g_runq[i].lock);
        for (uint32_t j = 0u; j < (SCHED_MAX_TASKS + 31u) / 32u; j++) {
            g_runq[i].bits[j] = 0u;
        }
    }
    g_next_tid = 1u;
    g_spawn_cpu_rr = 0u;

    root = &g_tasks[0];
    root->used = 1u;
    root->is_idle = 1u;
    root->entry = sched_idle_task;
    root->name = "idle";
    root->parent_tid = -1;
    root->exit_code = 0u;
    sched_waitq_init_locked(&root->child_waitq);
    root->pub.tid = 0u;
    root->pub.state = SCHED_TASK_RUNNABLE;
    root->pub.wake_tick = 0u;
    root->pub.run_ticks = 0u;
    root->pub.arg = 0;
    root->run_cpu = 0u;
    spinlock_init(&root->fd_lock);
    sched_fd_table_init_stdio(root);

    spinlock_unlock(&g_sched_lock);
    timer_program_period_us(SCHED_TICK_PERIOD_US);
}

uint32_t sched_tick_period_us(void) {
    return SCHED_TICK_PERIOD_US;
}

void schedule_tick(void) {
    uint32_t cur_idx;
    spinlock_lock(&g_sched_lock);
    g_ticks++;
    sched_try_wake_sleepers((uint32_t)g_ticks);

    cur_idx = *sched_cpu_current_idx_ptr();
    if (cur_idx < SCHED_MAX_TASKS) {
        sched_task_slot_t *slot = &g_tasks[cur_idx];
        if (slot->used && !slot->is_idle && slot->pub.state == SCHED_TASK_RUNNING) {
            slot->quantum_used++;
            if (slot->quantum_used >= SCHED_QUANTUM_TICKS) {
                g_need_resched = 1u;
            }
        }
    }
    spinlock_unlock(&g_sched_lock);
}

unsigned int sched_ticks(void) {
    unsigned int v;
    spinlock_lock(&g_sched_lock);
    v = g_ticks;
    spinlock_unlock(&g_sched_lock);
    return v;
}

int sched_current_tid(void) {
    int tid;
    sched_task_slot_t *slot;
    spinlock_lock(&g_sched_lock);
    slot = sched_current_slot();
    if (!slot) {
        spinlock_unlock(&g_sched_lock);
        return -1;
    }
    tid = (int)slot->pub.tid;
    spinlock_unlock(&g_sched_lock);
    return tid;
}

void sched_pump_once(void) {
    sched_switch_current_to_scheduler();
}

void sched_block_until_runnable(void) {
    for (;;) {
        uint32_t blocked = 0u;
        spinlock_lock(&g_sched_lock);
        sched_task_slot_t *self = sched_current_slot();
        if (self && !self->is_idle && self->used &&
            (self->pub.state == SCHED_TASK_BLOCKED || self->pub.state == SCHED_TASK_SLEEPING)) {
            blocked = 1u;
        }
        spinlock_unlock(&g_sched_lock);

        if (!blocked) {
            return;
        }
        sched_switch_current_to_scheduler();
        __asm__ __volatile__("" ::: "memory");
    }
}

void sched_run(void) {
    for (;;) {
        uint32_t *idx_ptr;
        sched_task_slot_t *slot;
        uint32_t cpu_id;
        uint32_t next;
        spinlock_lock(&g_sched_lock);
        cpu_id = sched_cpu_id();
        idx_ptr = sched_cpu_current_idx_ptr();
        next = *idx_ptr;
        if (g_need_resched || !sched_idx_runnable(next)) {
            g_need_resched = 0u;
            next = sched_pick_next_idx(cpu_id, next);
        }

        if (next == 0u) {
            *idx_ptr = 0u;
            spinlock_unlock(&g_sched_lock);
            __asm__ volatile("pause\n" ::: "memory");
            continue;
        }

        slot = &g_tasks[next];
        if (!slot->used || !slot->entry || slot->pub.state != SCHED_TASK_RUNNABLE) {
            sched_runq_del(cpu_id, next);
            g_need_resched = 1u;
            *idx_ptr = 0u;
            spinlock_unlock(&g_sched_lock);
            continue;
        }

        *idx_ptr = next;
        slot->run_cpu = cpu_id;
        sched_runq_del(cpu_id, next);
        slot->pub.state = SCHED_TASK_RUNNING;
        slot->pub.run_ticks++;
        spinlock_unlock(&g_sched_lock);

        sched_ctx_swap(&g_sched_ctx[cpu_id], &slot->stack_ctx);

        spinlock_lock(&g_sched_lock);
        *idx_ptr = 0u;
        if (slot->pub.state == SCHED_TASK_RUNNING) {
            slot->pub.state = SCHED_TASK_RUNNABLE;
            sched_runq_add(cpu_id, next);
        }
        spinlock_unlock(&g_sched_lock);
    }
}
