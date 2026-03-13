#include "sched_internal.h"

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
    slot->waitq = 0;
    sched_waitq_init_locked(&slot->child_waitq);
    slot->quantum_used = 0u;
    slot->exit_code = 0u;
    parent_slot = sched_current_slot();
    slot->parent_tid = parent_slot ? (int32_t)parent_slot->pub.tid : 0;
    slot->pub.tid = g_next_tid++;
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
