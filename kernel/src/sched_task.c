#include "sched_internal.h"
#include "../include/kernel/irq.h"
#include "../include/kernel/printk.h"
#include "../include/kernel/syscall.h"

static sched_stack_ctx_t g_vfork_discard_ctx[SCHED_MAX_CPUS];

enum {
    SCHED_SYSCALL_ABI_OFF_MAGIC = 0x00u,
    SCHED_SYSCALL_ABI_OFF_VERSION = 0x04u,
    SCHED_SYSCALL_ABI_OFF_LAST_NR = 0x08u,
    SCHED_SYSCALL_ABI_OFF_ARG0 = 0x0Cu,
    SCHED_SYSCALL_ABI_OFF_ARG1 = 0x10u,
    SCHED_SYSCALL_ABI_OFF_ARG2 = 0x14u,
    SCHED_SYSCALL_ABI_OFF_ARG3 = 0x18u,
    SCHED_SYSCALL_ABI_OFF_ARG4 = 0x1Cu,
    SCHED_SYSCALL_ABI_OFF_ARG5 = 0x20u,
    SCHED_SYSCALL_ABI_OFF_RET = 0x24u,
    SCHED_SYSCALL_ABI_OFF_ERRNO = 0x28u,
    SCHED_SYSCALL_ABI_OFF_TICK = 0x2Cu,
    SCHED_SYSCALL_ABI_SIZE = 0x30u
};

static uint32_t sched_task_kind_normalize(uint32_t kind) {
    return (kind == SCHED_TASK_KIND_USER) ? SCHED_TASK_KIND_USER : SCHED_TASK_KIND_KERNEL;
}

static void sched_waitq_sleep_locked(sched_waitq_t *q, uint32_t timeout_ticks);

static uint32_t sched_exec_state_normalize(uint32_t exec_state) {
    if (exec_state == SCHED_EXEC_STATE_KERNEL || exec_state == SCHED_EXEC_STATE_USER) {
        return exec_state;
    }
    return SCHED_EXEC_STATE_NONE;
}

__attribute__((noinline)) static void sched_mem_copy_u8(uint32_t dst_addr, uint32_t src_addr, uint32_t len) {
    volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)dst_addr;
    volatile const uint8_t *src = (volatile const uint8_t *)(uintptr_t)src_addr;
    for (uint32_t i = 0u; i < len; i++) {
        dst[i] = src[i];
    }
}

static void sched_mem_set_u8(uint32_t dst_addr, uint8_t v, uint32_t len) {
    volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)dst_addr;
    for (uint32_t i = 0u; i < len; i++) {
        dst[i] = v;
    }
}

static void sched_stack_ctx_copy(sched_stack_ctx_t *dst, const sched_stack_ctx_t *src) {
    if (!dst || !src) {
        return;
    }
    dst->call_base = src->call_base;
    dst->data_base = src->data_base;
    dst->isr_base = src->isr_base;
    dst->csp = src->csp;
    dst->dsp = src->dsp;
    dst->isp = src->isp;
    dst->irq_masked = src->irq_masked;
    dst->pool_slot = src->pool_slot;
    dst->valid = src->valid;
    for (uint32_t i = 0u; i < 32u; i++) {
        dst->regs[i] = src->regs[i];
    }
}

static inline uint32_t sched_addr_in_range(uint32_t v, uint32_t lo, uint32_t hi) {
    return (v >= lo && v < hi) ? 1u : 0u;
}

static inline uint32_t sched_addr_reloc(uint32_t v, uint32_t lo, uint32_t hi, int32_t delta) {
    if (!sched_addr_in_range(v, lo, hi)) {
        return v;
    }
    return (uint32_t)((int32_t)v + delta);
}

__attribute__((always_inline)) static inline uint32_t sched_cpu_ctx_read32(uint32_t io_addr) {
    uint32_t value;
    __asm__ volatile("in %0, %1" : "=r"(value) : "r"(io_addr));
    return value;
}

__attribute__((always_inline)) static inline void sched_cpu_ctx_write32(uint32_t io_addr, uint32_t value) {
    __asm__ volatile("out %0, %1" :: "r"(value), "r"(io_addr));
}

static inline void sched_cpu_ctx_clear_interrupt(void) {
    sched_cpu_ctx_write32(IO_CPU_CTX_IN_INTERRUPT, 0u);
}

static void sched_reloc_words(uint32_t base,
                              uint32_t bytes,
                              uint32_t r0_lo,
                              uint32_t r0_hi,
                              int32_t d0,
                              uint32_t r1_lo,
                              uint32_t r1_hi,
                              int32_t d1) {
    uint32_t words = bytes / 4u;
    for (uint32_t i = 0u; i < words; i++) {
        volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(base + i * 4u);
        uint32_t v = *p;
        v = sched_addr_reloc(v, r0_lo, r0_hi, d0);
        v = sched_addr_reloc(v, r1_lo, r1_hi, d1);
        *p = v;
    }
}

static uint32_t sched_stack_slot_used_locked(uint32_t slot) {
    uint32_t slot_base = sched_stack_pool_slot_base(slot);
    for (uint32_t i = 0u; i < SCHED_MAX_TASKS; i++) {
        sched_task_slot_t *t = &g_tasks[i];
        if (!t->used || !t->stack_ctx.valid) {
            continue;
        }
        if (t->stack_ctx.call_base == slot_base) {
            return 1u;
        }
    }
    return 0u;
}

static int sched_stack_slot_index_for_base(uint32_t base, uint32_t *slot_out) {
    if (!slot_out) {
        return -1;
    }
    for (uint32_t slot = 0u; slot < VM_STACK_POOL_SLOTS; slot++) {
        if (sched_stack_pool_slot_base(slot) == base) {
            *slot_out = slot;
            return 0;
        }
    }
    return -1;
}

static int sched_stack_slot_index_for_sp(uint32_t sp, uint32_t *slot_out) {
    if (!slot_out) {
        return -1;
    }
    for (uint32_t slot = 0u; slot < VM_STACK_POOL_SLOTS; slot++) {
        uint32_t base = sched_stack_pool_slot_base(slot);
        uint32_t top = base + VM_STACK_SLOT_BYTES;
        if (sp >= base && sp < top) {
            *slot_out = slot;
            return 0;
        }
    }
    return -1;
}

static inline uint32_t sched_syscall_abi_base(uint32_t abi_addr) {
    if (abi_addr != 0u && (abi_addr & 0x3u) == 0u && abi_addr < KERNEL_MEM_SIZE &&
        SCHED_SYSCALL_ABI_SIZE <= (KERNEL_MEM_SIZE - abi_addr) &&
        *(volatile uint32_t *)(uintptr_t)abi_addr == SYSCALL_ABI_MAGIC) {
        return abi_addr;
    }
    return SYSCALL_ABI_ADDR;
}

static inline void sched_syscall_abi_write32_at(uint32_t base, uint32_t off, uint32_t value) {
    *(volatile uint32_t *)(uintptr_t)(base + off) = value;
}

__attribute__((noreturn)) static void sched_vfork_resume_iret(sched_task_slot_t *slot, uint32_t ret_value) {
    uint32_t base;
    if (!slot || !slot->used || slot->vfork_resume_valid == 0u) {
        sched_exit_code(0x7Fu);
    }
    base = sched_syscall_abi_base(slot->syscall_abi_addr);
    sched_syscall_abi_write32_at(base, SCHED_SYSCALL_ABI_OFF_MAGIC, SYSCALL_ABI_MAGIC);
    sched_syscall_abi_write32_at(base, SCHED_SYSCALL_ABI_OFF_VERSION, SYSCALL_ABI_VERSION);
    sched_syscall_abi_write32_at(base, SCHED_SYSCALL_ABI_OFF_LAST_NR, SYS_VFORK);
    sched_syscall_abi_write32_at(base, SCHED_SYSCALL_ABI_OFF_ARG0, 0u);
    sched_syscall_abi_write32_at(base, SCHED_SYSCALL_ABI_OFF_ARG1, 0u);
    sched_syscall_abi_write32_at(base, SCHED_SYSCALL_ABI_OFF_ARG2, 0u);
    sched_syscall_abi_write32_at(base, SCHED_SYSCALL_ABI_OFF_ARG3, 0u);
    sched_syscall_abi_write32_at(base, SCHED_SYSCALL_ABI_OFF_ARG4, 0u);
    sched_syscall_abi_write32_at(base, SCHED_SYSCALL_ABI_OFF_ARG5, 0u);
    sched_syscall_abi_write32_at(base, SCHED_SYSCALL_ABI_OFF_RET, ret_value);
    sched_syscall_abi_write32_at(base, SCHED_SYSCALL_ABI_OFF_ERRNO, 0u);
    sched_syscall_abi_write32_at(base, SCHED_SYSCALL_ABI_OFF_TICK, sched_ticks());
    sched_stack_ctx_copy(&slot->stack_ctx, &slot->vfork_resume_ctx);
    slot->vfork_resume_valid = 0u;
    sched_cpu_ctx_write32(IO_CPU_CTX_CALL_BASE, slot->stack_ctx.call_base);
    sched_cpu_ctx_write32(IO_CPU_CTX_DATA_BASE, slot->stack_ctx.data_base);
    sched_cpu_ctx_write32(IO_CPU_CTX_ISR_BASE, slot->stack_ctx.isr_base);
    sched_cpu_ctx_write32(IO_CPU_CTX_CSP, slot->stack_ctx.csp);
    sched_cpu_ctx_write32(IO_CPU_CTX_DSP, slot->stack_ctx.dsp);
    sched_cpu_ctx_write32(IO_CPU_CTX_ISP, slot->stack_ctx.isp);
    sched_cpu_ctx_write32(IO_CPU_CTX_IRQ_MASK, slot->stack_ctx.irq_masked);
    __asm__ volatile("iret");
    __builtin_unreachable();
}

__attribute__((noreturn)) static void sched_vfork_child_entry(sched_task_t *task, void *arg) {
    sched_task_slot_t *slot;
    (void)task;
    (void)arg;
    slot = sched_current_slot();
    sched_vfork_resume_iret(slot, 0u);
}

__attribute__((noreturn)) static void sched_vfork_parent_entry(sched_task_t *task, void *arg) {
    sched_task_slot_t *slot;
    (void)task;
    (void)arg;
    slot = sched_current_slot();
    sched_vfork_resume_iret(slot, slot ? slot->vfork_resume_ret : (uint32_t)-1);
}

static int sched_vfork_prepare_child_user_locked(sched_task_slot_t *child,
                                                 const sched_stack_ctx_t *live) {
    uint32_t saved_csp;
    uint32_t saved_dsp;
    uint32_t call_live_bytes;
    uint32_t data_live_bytes;
    uint32_t isr_live_bytes;
    uint32_t helper_idx;
    uint32_t helper_addr;
    uint32_t ret_addr;
    uint32_t text_base;
    if (!child || !live || !child->stack_ctx.valid) {
        return -1;
    }
    saved_csp = irq_saved_user_csp();
    saved_dsp = irq_saved_user_dsp();
    if (saved_csp == 0u || saved_csp > VM_CALL_STACK_ENTRIES || saved_dsp > VM_DATA_STACK_ENTRIES ||
        live->isp > VM_ISR_STACK_ENTRIES) {
        return -1;
    }

    /*
     * Child returns to user mode on a fresh kernel slot. Only copy the live
     * user-visible tails; copying the full parent kernel call/data stacks
     * would leak the current syscall/trampoline frames into the child.
     */
    call_live_bytes = (VM_CALL_STACK_ENTRIES - saved_csp) * 8u;
    if (call_live_bytes != 0u) {
        sched_mem_copy_u8(child->stack_ctx.call_base + saved_csp * 8u,
                          live->call_base + saved_csp * 8u,
                          call_live_bytes);
    }

    data_live_bytes = (VM_DATA_STACK_ENTRIES - saved_dsp) * 4u;
    if (data_live_bytes != 0u) {
        sched_mem_copy_u8(child->stack_ctx.data_base + saved_dsp * 4u,
                          live->data_base + saved_dsp * 4u,
                          data_live_bytes);
    }

    isr_live_bytes = (VM_ISR_STACK_ENTRIES - live->isp) * 8u;
    if (isr_live_bytes != 0u) {
        sched_mem_copy_u8(child->stack_ctx.isr_base + live->isp * 8u,
                          live->isr_base + live->isp * 8u,
                          isr_live_bytes);
    }

    sched_stack_ctx_copy(&child->vfork_resume_ctx, &child->stack_ctx);
    child->vfork_resume_ctx.csp = saved_csp;
    child->vfork_resume_ctx.dsp = saved_dsp;
    child->vfork_resume_ctx.isp = live->isp;
    child->vfork_resume_ctx.irq_masked = live->irq_masked;
    child->vfork_resume_valid = 1u;

    helper_idx = saved_csp - 1u;
    ret_addr = child->stack_ctx.call_base + helper_idx * 8u;
    helper_addr = (uint32_t)(uintptr_t)&sched_vfork_child_entry;
    text_base = (uint32_t)(uintptr_t)__text_start;
    if (helper_addr < text_base) {
        helper_addr += text_base;
    }
    *(volatile uint32_t *)(uintptr_t)(ret_addr + 0u) = helper_addr;
    *(volatile uint32_t *)(uintptr_t)(ret_addr + 4u) = 0u;
    child->stack_ctx.csp = helper_idx;
    child->stack_ctx.dsp = VM_DATA_STACK_ENTRIES;
    child->stack_ctx.isp = live->isp;
    child->stack_ctx.irq_masked = live->irq_masked;
    child->stack_ctx.regs[30] = child->stack_ctx.call_base + VM_STACK_SLOT_BYTES;
    child->stack_ctx.regs[31] = child->stack_ctx.call_base + VM_STACK_SLOT_BYTES;
    return 0;
}

static int sched_vfork_prepare_parent_resume_locked(sched_task_slot_t *self,
                                                    const sched_stack_ctx_t *live,
                                                    uint32_t child_tid) {
    uint32_t saved_csp;
    uint32_t saved_dsp;
    uint32_t helper_idx;
    uint32_t helper_addr;
    if (!self || !live || !self->stack_ctx.valid) {
        return -1;
    }
    saved_csp = irq_saved_user_csp();
    saved_dsp = irq_saved_user_dsp();
    if (saved_csp == 0u || saved_csp > VM_CALL_STACK_ENTRIES || saved_dsp > VM_DATA_STACK_ENTRIES ||
        live->isp > VM_ISR_STACK_ENTRIES) {
        return -1;
    }
    sched_stack_ctx_copy(&self->vfork_resume_ctx, &self->stack_ctx);
    self->vfork_resume_ctx.csp = saved_csp;
    self->vfork_resume_ctx.dsp = saved_dsp;
    self->vfork_resume_ctx.isp = live->isp;
    self->vfork_resume_ctx.irq_masked = live->irq_masked;
    self->vfork_resume_ret = child_tid;
    self->vfork_resume_valid = 1u;

    helper_idx = saved_csp - 1u;
    helper_addr = (uint32_t)(uintptr_t)&sched_vfork_parent_entry;
    *(volatile uint32_t *)(uintptr_t)(self->stack_ctx.call_base + helper_idx * 8u + 0u) = helper_addr;
    *(volatile uint32_t *)(uintptr_t)(self->stack_ctx.call_base + helper_idx * 8u + 4u) = 0u;
    self->stack_ctx.csp = helper_idx;
    self->stack_ctx.dsp = VM_DATA_STACK_ENTRIES;
    self->stack_ctx.isp = live->isp;
    self->stack_ctx.irq_masked = live->irq_masked;
    self->stack_ctx.regs[30] = self->stack_ctx.call_base + VM_STACK_SLOT_BYTES;
    self->stack_ctx.regs[31] = self->stack_ctx.call_base + VM_STACK_SLOT_BYTES;
    return 0;
}

static int sched_vfork_trampoline(uint32_t abi_addr) {
    sched_stack_ctx_t live;
    sched_task_slot_t *self;
    sched_task_slot_t *child;
    uint32_t self_slot;
    int slot_idx;
    int child_tid;

    self = sched_current_slot();
    if (!self || self->is_idle) {
        return -1;
    }

    sched_ctx_capture(&live);

    spinlock_lock(&g_sched_lock);
    self = sched_current_slot();
    if (!self || self->is_idle || !self->used || !self->stack_ctx.valid) {
        spinlock_unlock(&g_sched_lock);
        return -1;
    }
    self->syscall_abi_addr = abi_addr;

    self_slot = self->stack_ctx.pool_slot;
    sched_stack_ctx_copy(&self->stack_ctx, &live);
    self->stack_ctx.pool_slot = self_slot;
    self->stack_ctx.valid = 1u;

    slot_idx = sched_alloc_slot();
    if (slot_idx < 0) {
        spinlock_unlock(&g_sched_lock);
        return -1;
    }

    child = &g_tasks[(uint32_t)slot_idx];
    sched_clear_task(child);
    child->used = 1u;
    child->is_idle = 0u;
    child->entry = sched_vfork_child_entry;
    child->name = self->name;
    child->vfork_child_start = 0u;
    child->vfork_child_active = 1u;
    child->waitq = 0;
    sched_waitq_init_locked(&child->child_waitq);
    child->quantum_used = 0u;
    child->run_cpu = sched_cpu_id();
    child->exit_code = 0u;
    child->parent_tid = (int32_t)self->pub.tid;
    child->pub.tid = g_next_tid++;
    child->pub.pid = child->pub.tid;
    child->pub.ppid = (int32_t)self->pub.pid;
    child->pub.state = SCHED_TASK_RUNNABLE;
    child->pub.kind = self->pub.kind;
    child->pub.exec_state = self->pub.exec_state;
    child->pub.wake_tick = 0u;
    child->pub.run_ticks = self->pub.run_ticks;
    child->pub.arg = 0;
    child->syscall_abi_addr = self->syscall_abi_addr;
    child->vfork_resume_valid = 0u;
    if (g_next_tid == 0u) {
        g_next_tid = 1u;
    }
    spinlock_init(&child->fd_lock);

    if (sched_stack_alloc_locked(&child->stack_ctx) != 0) {
        sched_clear_task(child);
        spinlock_unlock(&g_sched_lock);
        return -1;
    }
    sched_stack_prepare_bootstrap_locked(child);
    if (sched_fd_table_clone(child, self) != SCHED_FD_OK) {
        sched_stack_free_locked(child);
        sched_clear_task(child);
        spinlock_unlock(&g_sched_lock);
        return -1;
    }
    if (sched_vfork_prepare_child_user_locked(child, &live) != 0) {
        sched_stack_free_locked(child);
        sched_clear_task(child);
        spinlock_unlock(&g_sched_lock);
        return -1;
    }
    child_tid = (int)child->pub.tid;
    if (sched_vfork_prepare_parent_resume_locked(self, &live, (uint32_t)child_tid) != 0) {
        sched_stack_free_locked(child);
        sched_clear_task(child);
        self->vfork_resume_valid = 0u;
        spinlock_unlock(&g_sched_lock);
        return -1;
    }

    sched_runq_add(child->run_cpu, (uint32_t)slot_idx);
    sched_waitq_sleep_locked(&self->child_waitq, 0u);
    sched_mark_resched_all();
    spinlock_unlock(&g_sched_lock);
    sched_ctx_swap(&g_vfork_discard_ctx[sched_cpu_id()], &g_sched_ctx[sched_cpu_id()]);
    return -1;
}

static int sched_vfork_sync_parent_locked(sched_task_slot_t *self,
                                          const sched_stack_ctx_t *parent_live_in,
                                          uint32_t *live_slot_out,
                                          uint32_t *call_slot_out) {
    uint32_t live_slot = VM_STACK_POOL_SLOTS;
    uint32_t call_slot = VM_STACK_POOL_SLOTS;
    uint32_t self_slot;
    if (!self || !parent_live_in || !live_slot_out || !call_slot_out) {
        return -1;
    }
    if (sched_stack_slot_index_for_sp(parent_live_in->regs[30], &live_slot) != 0 &&
        sched_stack_slot_index_for_base(parent_live_in->call_base, &live_slot) != 0) {
        return -1;
    }
    (void)sched_stack_slot_index_for_base(parent_live_in->call_base, &call_slot);
    sched_stack_ctx_copy(&self->stack_ctx, parent_live_in);
    self->stack_ctx.valid = 1u;
    self_slot = (call_slot < VM_STACK_POOL_SLOTS) ? call_slot : live_slot;
    self->stack_ctx.pool_slot = self_slot;
    sched_stack_bitmap_set(live_slot);
    if (self_slot != live_slot) {
        sched_stack_bitmap_set(self_slot);
    }
    *live_slot_out = live_slot;
    *call_slot_out = call_slot;
    return 0;
}

static int sched_vfork_alloc_child_stack_locked(sched_task_slot_t *child,
                                                uint32_t parent_sp,
                                                uint32_t parent_call_base) {
    if (!child) {
        return -1;
    }
    child->stack_ctx.valid = 0u;
    for (uint32_t slot = g_stack_pool_reserved; slot < VM_STACK_POOL_SLOTS; slot++) {
        uint32_t base = sched_stack_pool_slot_base(slot);
        uint32_t top = base + VM_STACK_SLOT_BYTES;
        if (base == parent_call_base) {
            continue;
        }
        if (parent_sp >= base && parent_sp < top) {
            continue;
        }
        if (sched_stack_slot_used_locked(slot)) {
            continue;
        }
        sched_stack_bitmap_set(slot);
        sched_mem_set_u8(base, 0u, VM_STACK_SLOT_BYTES);
        child->stack_ctx.call_base = base;
        child->stack_ctx.data_base = base + VM_CALL_STACK_BYTES;
        child->stack_ctx.isr_base = base + (VM_CALL_STACK_BYTES + VM_DATA_STACK_BYTES);
        child->stack_ctx.csp = VM_CALL_STACK_ENTRIES;
        child->stack_ctx.dsp = VM_DATA_STACK_ENTRIES;
        child->stack_ctx.isp = VM_ISR_STACK_ENTRIES;
        child->stack_ctx.irq_masked = 0u;
        child->stack_ctx.pool_slot = slot;
        child->stack_ctx.valid = 1u;
        child->stack_ctx.regs[30] = base + VM_STACK_SLOT_BYTES;
        child->stack_ctx.regs[31] = base + VM_STACK_SLOT_BYTES;
        return 0;
    }
    return -1;
}

static int sched_stack_alloc_vfork_child_locked(sched_stack_ctx_t *ctx_out,
                                                uint32_t forbid_a,
                                                uint32_t forbid_b) {
    if (!ctx_out) {
        return -1;
    }
    for (uint32_t slot = g_stack_pool_reserved; slot < VM_STACK_POOL_SLOTS; slot++) {
        uint32_t base;
        if (slot == forbid_a || slot == forbid_b) {
            continue;
        }
        if (sched_stack_slot_used_locked(slot)) {
            continue;
        }
        base = sched_stack_pool_slot_base(slot);
        sched_stack_bitmap_set(slot);
        sched_mem_set_u8(base, 0u, VM_STACK_SLOT_BYTES);
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

static void sched_detach_waitq(sched_task_slot_t *slot) {
    uint32_t idx;
    if (!slot || !slot->waitq) {
        return;
    }
    idx = sched_slot_index(slot);
    if (idx < SCHED_MAX_TASKS) {
        waitq_clear_bit(slot->waitq, idx);
    }
    slot->waitq = 0;
}

void sched_waitq_init_locked(sched_waitq_t *q) {
    if (!q) {
        return;
    }
    for (uint32_t i = 0u; i < (SCHED_MAX_TASKS + 31u) / 32u; i++) {
        q->bits[i] = 0u;
    }
}

void sched_try_wake_sleepers(uint32_t now_tick) {
    for (uint32_t i = 0u; i < SCHED_MAX_TASKS; i++) {
        sched_task_slot_t *slot = &g_tasks[i];
        if (!slot->used) {
            continue;
        }
        if (slot->pub.state != SCHED_TASK_SLEEPING && slot->pub.state != SCHED_TASK_BLOCKED) {
            continue;
        }
        if (slot->pub.wake_tick == 0u || slot->pub.wake_tick > now_tick) {
            continue;
        }
        sched_detach_waitq(slot);
        slot->pub.wake_tick = 0u;
        slot->pub.state = SCHED_TASK_RUNNABLE;
        slot->run_cpu = sched_cpu_normalize(slot->run_cpu);
        sched_runq_add(slot->run_cpu, i);
        slot->quantum_used = 0u;
        sched_mark_resched_all();
    }
}

static sched_task_slot_t *sched_find_by_tid(uint32_t tid) {
    if (tid == 0u) {
        return &g_tasks[0];
    }
    for (uint32_t i = 1u; i < SCHED_MAX_TASKS; i++) {
        sched_task_slot_t *slot = &g_tasks[i];
        if (!slot->used) {
            continue;
        }
        if (slot->pub.tid == tid) {
            return slot;
        }
    }
    return 0;
}

static uint32_t sched_task_is_child_of(const sched_task_slot_t *slot, int32_t parent_tid, int32_t pid) {
    if (!slot || !slot->used || slot->is_idle) {
        return 0u;
    }
    if (slot->parent_tid != parent_tid) {
        return 0u;
    }
    if (pid == SCHED_WAITPID_ANY) {
        return 1u;
    }
    return (slot->pub.tid == (uint32_t)pid) ? 1u : 0u;
}

static int sched_try_reap_child(int32_t parent_tid, int32_t pid, uint32_t *status_out) {
    uint32_t has_child = 0u;
    for (uint32_t i = 1u; i < SCHED_MAX_TASKS; i++) {
        sched_task_slot_t *slot = &g_tasks[i];
        if (!sched_task_is_child_of(slot, parent_tid, pid)) {
            continue;
        }
        has_child = 1u;
        if (slot->pub.state != SCHED_TASK_ZOMBIE) {
            continue;
        }
        if (status_out) {
            *status_out = (slot->exit_code & 0xFFu) << 8;
        }
        {
            const int child_tid = (int)slot->pub.tid;
            sched_runq_del(slot->run_cpu, i);
            sched_stack_free_locked(slot);
            sched_clear_task(slot);
            return child_tid;
        }
    }
    if (!has_child) {
        return SCHED_WAITPID_NO_CHILD;
    }
    return 0;
}

static void sched_waitq_sleep_locked(sched_waitq_t *q, uint32_t timeout_ticks) {
    uint32_t now;
    uint32_t slot_idx;
    sched_task_slot_t *slot = sched_current_slot();
    if (!q || !slot || slot->is_idle) {
        return;
    }

    now = sched_tick_now();
    slot_idx = sched_slot_index(slot);
    if (slot_idx >= SCHED_MAX_TASKS) {
        return;
    }

    sched_detach_waitq(slot);
    slot->waitq = q;
    waitq_set_bit(q, slot_idx);
    slot->pub.state = SCHED_TASK_BLOCKED;
    sched_runq_del(slot->run_cpu, slot_idx);
    slot->pub.wake_tick = timeout_ticks ? (now + timeout_ticks) : 0u;
    slot->quantum_used = 0u;
    sched_mark_resched_all();
}

static void sched_wake_slot(sched_task_slot_t *slot) {
    if (!slot || !slot->used || slot->is_idle) {
        return;
    }
    if (slot->pub.state != SCHED_TASK_BLOCKED && slot->pub.state != SCHED_TASK_SLEEPING) {
        return;
    }
    sched_detach_waitq(slot);
    slot->pub.wake_tick = 0u;
    slot->pub.state = SCHED_TASK_RUNNABLE;
    slot->run_cpu = sched_cpu_normalize(slot->run_cpu);
    sched_runq_add(slot->run_cpu, sched_slot_index(slot));
    slot->quantum_used = 0u;
    sched_mark_resched_all();
}

static int sched_irq_live_top_for_sp(uint32_t sp, uint32_t *live_top_out) {
    uint32_t cpus = smp_target_cpus();
    if (!live_top_out) {
        return -1;
    }
    if (cpus == 0u || cpus > SCHED_MAX_CPUS) {
        cpus = 1u;
    }
    for (uint32_t cpu = 0u; cpu < cpus; cpu++) {
        uint32_t top = KERNEL_IRQ_STACK_TOP - (cpu << KERNEL_IRQ_STACK_SHIFT);
        uint32_t bot = top - KERNEL_IRQ_STACK_BYTES;
        if (sp <= top && sp >= bot) {
            *live_top_out = top;
            return 0;
        }
    }
    return -1;
}

enum {
    VFORK_PREP_ERR_BAD_ARG = 1,
    VFORK_PREP_ERR_BAD_STACK_CTX = 2,
    VFORK_PREP_ERR_PARENT_SLOT = 3,
    VFORK_PREP_ERR_PARENT_SP_ALIGN = 4,
    VFORK_PREP_ERR_LIVE_RANGE = 5,
    VFORK_PREP_ERR_LIVE_BYTES = 6,
    VFORK_PREP_ERR_CHILD_ALIAS = 7
};

static int sched_prepare_vfork_child_ctx(const sched_task_slot_t *parent,
                                         const sched_stack_ctx_t *parent_live_in,
                                         const sched_task_slot_t *child,
                                         sched_stack_ctx_t *child_live_out) {
    uint32_t parent_slot_base;
    uint32_t parent_slot_top;
    uint32_t child_slot_base;
    int32_t delta_slot;
    uint32_t live_top;
    uint32_t parent_sp;
    uint32_t live_active_bytes;
    uint32_t child_c_top;
    uint32_t child_live_sp;
    uint32_t call_slot_base;
    uint32_t sp_slot_base;
    uint32_t has_call_slot;
    uint32_t has_sp_slot;
    int32_t delta_live;
    sched_stack_ctx_t child_live;

    if (!parent || !parent_live_in || !child || !child_live_out) {
        return VFORK_PREP_ERR_BAD_ARG;
    }
    if (!parent->stack_ctx.valid || !child->stack_ctx.valid) {
        return VFORK_PREP_ERR_BAD_STACK_CTX;
    }
    parent_sp = parent_live_in->regs[30];
    call_slot_base = 0u;
    sp_slot_base = 0u;
    has_call_slot = 0u;
    has_sp_slot = 0u;

    {
        uint32_t base = VM_STACK_POOL_BASE;
        for (uint32_t i = 0u; i < VM_STACK_POOL_SLOTS; i++) {
            if (!has_sp_slot && parent_sp >= base && parent_sp < (base + VM_STACK_SLOT_BYTES)) {
                sp_slot_base = base;
                has_sp_slot = 1u;
            }
            if (!has_call_slot && parent_live_in->call_base == base) {
                call_slot_base = base;
                has_call_slot = 1u;
            }
            base += VM_STACK_SLOT_BYTES;
        }
    }

    if (has_call_slot) {
        parent_slot_base = call_slot_base;
    } else if (has_sp_slot) {
        parent_slot_base = sp_slot_base;
    } else {
        return VFORK_PREP_ERR_PARENT_SLOT;
    }
    parent_slot_top = parent_slot_base + VM_STACK_SLOT_BYTES;
    child_slot_base = child->stack_ctx.call_base;
    delta_slot = (int32_t)(child_slot_base - parent_slot_base);

    if (child_slot_base == parent_slot_base) {
        return VFORK_PREP_ERR_CHILD_ALIAS;
    }

    if ((parent_sp & 0x3u) != 0u) {
        return VFORK_PREP_ERR_PARENT_SP_ALIGN;
    }
    if (sched_irq_live_top_for_sp(parent_sp, &live_top) == 0) {
        /* live_top set by helper */
    } else if (parent_sp >= parent_slot_base && parent_sp <= parent_slot_top) {
        live_top = parent_slot_top;
    } else {
        return VFORK_PREP_ERR_LIVE_RANGE;
    }

    live_active_bytes = live_top - parent_sp;
    if (live_active_bytes == 0u || live_active_bytes > VM_TASK_C_STACK_BYTES) {
        return VFORK_PREP_ERR_LIVE_BYTES;
    }

    child_c_top = child_slot_base + VM_STACK_SLOT_BYTES;
    child_live_sp = child_c_top - live_active_bytes;
    delta_live = (int32_t)(child_live_sp - parent_sp);

    sched_mem_copy_u8(child_slot_base, parent_slot_base, VM_STACK_SLOT_BYTES);
    sched_mem_copy_u8(child_live_sp, parent_sp, live_active_bytes);

    child_live.call_base = (uint32_t)((int32_t)parent_live_in->call_base + delta_slot);
    child_live.data_base = (uint32_t)((int32_t)parent_live_in->data_base + delta_slot);
    child_live.isr_base = (uint32_t)((int32_t)parent_live_in->isr_base + delta_slot);
    child_live.csp = parent_live_in->csp;
    child_live.dsp = parent_live_in->dsp;
    child_live.isp = parent_live_in->isp;
    child_live.irq_masked = parent_live_in->irq_masked;
    child_live.pool_slot = child->stack_ctx.pool_slot;
    child_live.valid = 1u;
    for (uint32_t i = 0u; i < 32u; i++) {
        child_live.regs[i] = parent_live_in->regs[i];
    }

    for (uint32_t i = 8u; i < 32u; i++) {
        uint32_t v = child_live.regs[i];
        v = sched_addr_reloc(v, parent_slot_base, parent_slot_base + VM_STACK_SLOT_BYTES, delta_slot);
        v = sched_addr_reloc(v, parent_sp, live_top, delta_live);
        child_live.regs[i] = v;
    }

    sched_reloc_words(child_slot_base,
                      VM_STACK_SLOT_BYTES,
                      parent_slot_base,
                      parent_slot_base + VM_STACK_SLOT_BYTES,
                      delta_slot,
                      parent_sp,
                      live_top,
                      delta_live);
    sched_reloc_words(child_live_sp,
                      live_active_bytes,
                      parent_slot_base,
                      parent_slot_base + VM_STACK_SLOT_BYTES,
                      delta_slot,
                      parent_sp,
                      live_top,
                      delta_live);

    sched_stack_ctx_copy(child_live_out, &child_live);
    return 0;
}

int sched_vfork(uint32_t abi_addr) {
    return sched_vfork_trampoline(abi_addr);
}

int sched_spawn(const char *name, sched_task_entry_t entry, void *arg) {
    int tid;
    int slot_idx;
    sched_task_slot_t *slot;
    sched_task_slot_t *parent_slot;
    if (!entry) {
        return -1;
    }

    spinlock_lock(&g_sched_lock);
    slot_idx = sched_alloc_slot();
    if (slot_idx < 0) {
        spinlock_unlock(&g_sched_lock);
        return -1;
    }

    slot = &g_tasks[(uint32_t)slot_idx];
    slot->used = 1u;
    slot->is_idle = 0u;
    slot->entry = entry;
    slot->name = name;
    slot->vfork_child_start = 0u;
    slot->vfork_child_active = 0u;
    slot->syscall_abi_addr = 0u;
    slot->waitq = 0;
    sched_waitq_init_locked(&slot->child_waitq);
    slot->quantum_used = 0u;
    slot->exit_code = 0u;
    parent_slot = sched_current_slot();
    slot->parent_tid = parent_slot ? (int32_t)parent_slot->pub.tid : 0;
    slot->pub.tid = g_next_tid++;
    slot->pub.pid = slot->pub.tid;
    slot->pub.ppid = parent_slot ? (int32_t)parent_slot->pub.pid : 0;
    slot->pub.kind = SCHED_TASK_KIND_KERNEL;
    slot->pub.exec_state = SCHED_EXEC_STATE_KERNEL;
    if (g_next_tid == 0u) {
        g_next_tid = 1u;
    }
    slot->pub.state = SCHED_TASK_RUNNABLE;
    slot->pub.wake_tick = 0u;
    slot->pub.run_ticks = 0u;
    slot->pub.arg = arg;
    if (sched_stack_alloc_locked(&slot->stack_ctx) != 0) {
        sched_clear_task(slot);
        spinlock_unlock(&g_sched_lock);
        return -1;
    }
    sched_stack_prepare_bootstrap_locked(slot);
    slot->run_cpu = sched_cpu_normalize(g_spawn_cpu_rr % sched_cpu_cap());
    g_spawn_cpu_rr++;
    spinlock_init(&slot->fd_lock);
    sched_fd_table_init_stdio(slot);
    sched_runq_add(slot->run_cpu, (uint32_t)slot_idx);
    sched_mark_resched_all();
    tid = (int)slot->pub.tid;
    spinlock_unlock(&g_sched_lock);
    return tid;
}

void sched_yield(void) {
    uint32_t should_switch = 0u;
    spinlock_lock(&g_sched_lock);
    sched_task_slot_t *slot = sched_current_slot();
    if (!slot) {
        spinlock_unlock(&g_sched_lock);
        return;
    }
    if (slot->pub.state == SCHED_TASK_RUNNING) {
        slot->pub.state = SCHED_TASK_RUNNABLE;
        slot->run_cpu = sched_cpu_id();
        sched_runq_add(slot->run_cpu, sched_slot_index(slot));
        should_switch = 1u;
    }
    slot->quantum_used = 0u;
    sched_mark_resched_all();
    spinlock_unlock(&g_sched_lock);
    if (should_switch) {
        sched_switch_current_to_scheduler();
    }
}

void sched_exit_code(uint32_t code) {
    spinlock_lock(&g_sched_lock);
    sched_task_slot_t *slot = sched_current_slot();
    if (!slot || slot->is_idle) {
        spinlock_unlock(&g_sched_lock);
        return;
    }
    sched_detach_waitq(slot);
    slot->vfork_child_active = 0u;
    sched_slot_close_all_fds(slot);
    slot->exit_code = code;
    slot->pub.state = SCHED_TASK_ZOMBIE;
    sched_runq_del(slot->run_cpu, sched_slot_index(slot));
    slot->pub.wake_tick = 0u;
    slot->quantum_used = 0u;
    if (slot->parent_tid >= 0) {
        sched_task_slot_t *parent = sched_find_by_tid((uint32_t)slot->parent_tid);
        if (parent) {
            for (uint32_t i = 1u; i < SCHED_MAX_TASKS; i++) {
                if (!waitq_test_bit(&parent->child_waitq, i)) {
                    continue;
                }
                sched_wake_slot(&g_tasks[i]);
            }
        }
    }
    sched_mark_resched_all();
    spinlock_unlock(&g_sched_lock);
    sched_cpu_ctx_clear_interrupt();
    sched_switch_current_to_scheduler();
    for (;;) {
        __asm__ volatile("pause\n" ::: "memory");
    }
}

void sched_exit(void) {
    sched_exit_code(0u);
}

void sched_sleep_ticks(uint32_t ticks) {
    uint32_t now;
    spinlock_lock(&g_sched_lock);
    sched_task_slot_t *slot = sched_current_slot();
    if (!slot) {
        spinlock_unlock(&g_sched_lock);
        return;
    }
    if (slot->is_idle) {
        spinlock_unlock(&g_sched_lock);
        return;
    }

    now = sched_tick_now();
    slot->pub.wake_tick = now + (ticks ? ticks : 1u);
    slot->pub.state = SCHED_TASK_SLEEPING;
    sched_runq_del(slot->run_cpu, sched_slot_index(slot));
    slot->quantum_used = 0u;
    sched_mark_resched_all();
    spinlock_unlock(&g_sched_lock);
    sched_switch_current_to_scheduler();
}

void sched_waitq_init(sched_waitq_t *q) {
    spinlock_lock(&g_sched_lock);
    sched_waitq_init_locked(q);
    spinlock_unlock(&g_sched_lock);
}

void sched_waitq_sleep(sched_waitq_t *q, uint32_t timeout_ticks) {
    spinlock_lock(&g_sched_lock);
    sched_waitq_sleep_locked(q, timeout_ticks);
    spinlock_unlock(&g_sched_lock);
}

void sched_waitq_wake_one(sched_waitq_t *q) {
    spinlock_lock(&g_sched_lock);
    if (!q) {
        spinlock_unlock(&g_sched_lock);
        return;
    }
    for (uint32_t i = 1u; i < SCHED_MAX_TASKS; i++) {
        if (!waitq_test_bit(q, i)) {
            continue;
        }
        sched_wake_slot(&g_tasks[i]);
        spinlock_unlock(&g_sched_lock);
        return;
    }
    spinlock_unlock(&g_sched_lock);
}

void sched_waitq_wake_all(sched_waitq_t *q) {
    spinlock_lock(&g_sched_lock);
    if (!q) {
        spinlock_unlock(&g_sched_lock);
        return;
    }
    for (uint32_t i = 1u; i < SCHED_MAX_TASKS; i++) {
        if (!waitq_test_bit(q, i)) {
            continue;
        }
        sched_wake_slot(&g_tasks[i]);
    }
    spinlock_unlock(&g_sched_lock);
}

int sched_waitpid(int32_t pid, uint32_t options, uint32_t *status_out) {
    int rc;
    spinlock_lock(&g_sched_lock);
    sched_task_slot_t *slot = sched_current_slot();
    if (!slot || slot->is_idle) {
        spinlock_unlock(&g_sched_lock);
        return SCHED_WAITPID_NO_CHILD;
    }

    rc = sched_try_reap_child((int32_t)slot->pub.tid, pid, status_out);
    if (rc != 0) {
        spinlock_unlock(&g_sched_lock);
        return rc;
    }

    if (options & SCHED_WAITPID_WNOHANG) {
        spinlock_unlock(&g_sched_lock);
        return 0;
    }

    sched_waitq_sleep_locked(&slot->child_waitq, 0u);
    spinlock_unlock(&g_sched_lock);
    return SCHED_WAITPID_BLOCKED;
}

void sched_task_set_kind(uint32_t kind) {
    spinlock_lock(&g_sched_lock);
    sched_task_slot_t *slot = sched_current_slot();
    if (slot && slot->used) {
        slot->pub.kind = sched_task_kind_normalize(kind);
    }
    spinlock_unlock(&g_sched_lock);
}

void sched_task_set_exec_state(uint32_t exec_state) {
    spinlock_lock(&g_sched_lock);
    sched_task_slot_t *slot = sched_current_slot();
    if (slot && slot->used) {
        slot->pub.exec_state = sched_exec_state_normalize(exec_state);
    }
    spinlock_unlock(&g_sched_lock);
}

uint32_t sched_task_get_kind(void) {
    uint32_t kind = SCHED_TASK_KIND_KERNEL;
    spinlock_lock(&g_sched_lock);
    sched_task_slot_t *slot = sched_current_slot();
    if (slot && slot->used) {
        kind = sched_task_kind_normalize(slot->pub.kind);
    }
    spinlock_unlock(&g_sched_lock);
    return kind;
}

uint32_t sched_task_get_exec_state(void) {
    uint32_t exec_state = SCHED_EXEC_STATE_NONE;
    spinlock_lock(&g_sched_lock);
    sched_task_slot_t *slot = sched_current_slot();
    if (slot && slot->used) {
        exec_state = sched_exec_state_normalize(slot->pub.exec_state);
    }
    spinlock_unlock(&g_sched_lock);
    return exec_state;
}

void sched_vfork_release_parent(void) {
    spinlock_lock(&g_sched_lock);
    sched_task_slot_t *slot = sched_current_slot();
    if (!slot || slot->is_idle || !slot->used || slot->vfork_child_active == 0u) {
        spinlock_unlock(&g_sched_lock);
        return;
    }

    slot->vfork_child_active = 0u;
    if (slot->parent_tid >= 0) {
        sched_task_slot_t *parent = sched_find_by_tid((uint32_t)slot->parent_tid);
        if (parent) {
            uint32_t parent_idx = sched_slot_index(parent);
            if (parent_idx < SCHED_MAX_TASKS && waitq_test_bit(&parent->child_waitq, parent_idx)) {
                sched_wake_slot(parent);
            }
        }
    }
    spinlock_unlock(&g_sched_lock);
}
