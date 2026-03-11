#include "../include/kernel/platform.h"
#include "../include/kernel/sched.h"
#include "../include/kernel/smp.h"
#include "../include/kernel/spinlock.h"

#define SCHED_TICK_PERIOD_US 5000u
#define SCHED_QUANTUM_TICKS 4u
#define SCHED_MAX_CPUS 32u

typedef struct sched_ofile {
    uint32_t used;
    uint32_t refs;
    uint32_t type;
    uint32_t status_flags;
    uint32_t fs_backend;
    uint32_t file_id;
    uint32_t file_size;
    uint32_t file_offset;
    uint32_t file_is_dir;
} sched_ofile_t;

typedef struct sched_fdent {
    uint32_t used;
    uint32_t ofile_idx;
    uint32_t fd_flags;
} sched_fdent_t;

enum {
    SCHED_OFILE_TYPE_NONE = SCHED_FD_TYPE_NONE,
    SCHED_OFILE_TYPE_STDIN = SCHED_FD_TYPE_STDIN,
    SCHED_OFILE_TYPE_STDOUT = SCHED_FD_TYPE_STDOUT,
    SCHED_OFILE_TYPE_STDERR = SCHED_FD_TYPE_STDERR,
    SCHED_OFILE_TYPE_DEV_NULL = SCHED_FD_TYPE_DEV_NULL,
    SCHED_OFILE_TYPE_DEV_ZERO = SCHED_FD_TYPE_DEV_ZERO,
    SCHED_OFILE_TYPE_DEV_TTY = SCHED_FD_TYPE_DEV_TTY,
    SCHED_OFILE_TYPE_SOCKET = SCHED_FD_TYPE_SOCKET,
    SCHED_OFILE_TYPE_REGULAR = SCHED_FD_TYPE_REGULAR
};

#define SCHED_FD_OFILE_INVALID ((uint32_t)~0u)

/*
 * Scheduler currently executes tasks as short callback steps.
 * This keeps blocking/time semantics testable before ISA-level context switch lands.
 */
typedef struct sched_task_slot {
    sched_task_t pub;
    uint32_t used;
    uint32_t is_idle;
    uint32_t quantum_used;
    uint32_t run_cpu;
    int32_t parent_tid;
    uint32_t exit_code;
    sched_task_entry_t entry;
    const char *name;
    sched_waitq_t *waitq;
    sched_waitq_t child_waitq;
    spinlock_t fd_lock;
    sched_ofile_t ofiles[SCHED_MAX_FDS];
    sched_fdent_t fdtab[SCHED_MAX_FDS];
} sched_task_slot_t;

typedef struct sched_runq {
    spinlock_t lock;
    uint32_t bits[(SCHED_MAX_TASKS + 31u) / 32u];
} sched_runq_t;

static inline void timer_program_period_us(uint32_t period_us) {
    *(volatile uint32_t *)(uintptr_t)TIMER_MMIO_BASE = period_us;
}

static volatile unsigned int g_ticks;
static volatile unsigned int g_need_resched;
static sched_task_slot_t g_tasks[SCHED_MAX_TASKS];
static uint32_t g_cpu_current_idx[SCHED_MAX_CPUS];
static uint32_t g_next_tid;
static uint32_t g_spawn_cpu_rr;
static sched_runq_t g_runq[SCHED_MAX_CPUS];
static spinlock_t g_sched_lock;

static void sched_fd_table_clear(sched_task_slot_t *slot);
static void sched_fd_table_init_stdio(sched_task_slot_t *slot);
static int sched_slot_close_fd(sched_task_slot_t *slot, int32_t fd);
static void sched_slot_close_all_fds(sched_task_slot_t *slot);
static int sched_slot_find_free_ofile(const sched_task_slot_t *slot);
static void sched_wake_slot(sched_task_slot_t *slot);
static void sched_waitq_init_locked(sched_waitq_t *q);
static void sched_mark_resched_all(void);
static void sched_runq_add(uint32_t cpu_id, uint32_t idx);
static void sched_runq_del(uint32_t cpu_id, uint32_t idx);
static uint32_t sched_runq_pick_local(uint32_t cpu_id, uint32_t start_idx);
static uint32_t sched_runq_steal_to(uint32_t dst_cpu_id);

static inline uint32_t sched_cpu_id(void) {
    uint32_t id = 0u;
    __asm__ volatile("cpuid %0" : "=r"(id));
    if (id >= SCHED_MAX_CPUS) {
        return 0u;
    }
    return id;
}

static inline uint32_t *sched_cpu_current_idx_ptr(void) {
    return &g_cpu_current_idx[sched_cpu_id()];
}

static inline uint32_t sched_tick_now(void) {
    return (uint32_t)g_ticks;
}

static void sched_clear_task(sched_task_slot_t *slot) {
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

static inline void waitq_set_bit(sched_waitq_t *q, uint32_t idx) {
    q->bits[idx / 32u] |= (1u << (idx % 32u));
}

static inline void waitq_clear_bit(sched_waitq_t *q, uint32_t idx) {
    q->bits[idx / 32u] &= ~(1u << (idx % 32u));
}

static inline uint32_t waitq_test_bit(const sched_waitq_t *q, uint32_t idx) {
    return (q->bits[idx / 32u] >> (idx % 32u)) & 1u;
}

static inline uint32_t sched_cpu_cap(void) {
    uint32_t n = smp_online_cpus();
    if (n == 0u) {
        n = 1u;
    }
    if (n > SCHED_MAX_CPUS) {
        n = SCHED_MAX_CPUS;
    }
    return n;
}

static inline uint32_t sched_cpu_normalize(uint32_t cpu_id) {
    uint32_t cap = sched_cpu_cap();
    if (cpu_id >= cap) {
        return 0u;
    }
    return cpu_id;
}

static void sched_mark_resched_all(void) {
    g_need_resched = 1u;
}

static void sched_runq_add(uint32_t cpu_id, uint32_t idx) {
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

static void sched_runq_del(uint32_t cpu_id, uint32_t idx) {
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
}

static uint32_t sched_slot_index(const sched_task_slot_t *slot) {
    if (!slot) {
        return 0u;
    }
    return (uint32_t)(slot - &g_tasks[0]);
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

static void sched_idle_task(sched_task_t *task, void *arg) {
    (void)task;
    (void)arg;
    __asm__ __volatile__("" ::: "memory");
}

static int sched_alloc_slot(void) {
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

static sched_task_slot_t *sched_current_slot(void) {
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

static sched_task_slot_t *sched_current_slot_fd_locked(void) {
    sched_task_slot_t *slot = sched_current_slot();
    if (!slot) {
        return 0;
    }
    spinlock_lock(&slot->fd_lock);
    return slot;
}

static void sched_fd_table_clear(sched_task_slot_t *slot) {
    uint32_t i;
    if (!slot) {
        return;
    }
    for (i = 0u; i < SCHED_MAX_FDS; i++) {
        slot->ofiles[i].used = 0u;
        slot->ofiles[i].refs = 0u;
        slot->ofiles[i].type = SCHED_OFILE_TYPE_NONE;
        slot->ofiles[i].status_flags = 0u;
        slot->ofiles[i].fs_backend = 0u;
        slot->ofiles[i].file_id = 0u;
        slot->ofiles[i].file_size = 0u;
        slot->ofiles[i].file_offset = 0u;
        slot->ofiles[i].file_is_dir = 0u;
        slot->fdtab[i].used = 0u;
        slot->fdtab[i].ofile_idx = SCHED_FD_OFILE_INVALID;
        slot->fdtab[i].fd_flags = 0u;
    }
}

static void sched_fd_table_init_stdio(sched_task_slot_t *slot) {
    if (!slot || SCHED_MAX_FDS < 3u) {
        return;
    }
    sched_fd_table_clear(slot);

    slot->ofiles[0].used = 1u;
    slot->ofiles[0].refs = 1u;
    slot->ofiles[0].type = SCHED_OFILE_TYPE_STDIN;
    slot->ofiles[0].status_flags = SCHED_FD_O_RDONLY;
    slot->fdtab[0].used = 1u;
    slot->fdtab[0].ofile_idx = 0u;

    slot->ofiles[1].used = 1u;
    slot->ofiles[1].refs = 1u;
    slot->ofiles[1].type = SCHED_OFILE_TYPE_STDOUT;
    slot->ofiles[1].status_flags = SCHED_FD_O_WRONLY;
    slot->fdtab[1].used = 1u;
    slot->fdtab[1].ofile_idx = 1u;

    slot->ofiles[2].used = 1u;
    slot->ofiles[2].refs = 1u;
    slot->ofiles[2].type = SCHED_OFILE_TYPE_STDERR;
    slot->ofiles[2].status_flags = SCHED_FD_O_WRONLY;
    slot->fdtab[2].used = 1u;
    slot->fdtab[2].ofile_idx = 2u;
}

static sched_ofile_t *sched_slot_ofile_by_fd(sched_task_slot_t *slot, int32_t fd, sched_fdent_t **fdent_out) {
    uint32_t of_idx;
    sched_fdent_t *fdent;
    if (!slot || fd < 0 || (uint32_t)fd >= SCHED_MAX_FDS) {
        return 0;
    }
    fdent = &slot->fdtab[(uint32_t)fd];
    if (!fdent->used) {
        return 0;
    }
    of_idx = fdent->ofile_idx;
    if (of_idx >= SCHED_MAX_FDS) {
        return 0;
    }
    if (!slot->ofiles[of_idx].used || slot->ofiles[of_idx].refs == 0u) {
        return 0;
    }
    if (fdent_out) {
        *fdent_out = fdent;
    }
    return &slot->ofiles[of_idx];
}

static int sched_slot_find_free_fd(const sched_task_slot_t *slot, uint32_t start) {
    uint32_t i;
    if (!slot || start >= SCHED_MAX_FDS) {
        return -1;
    }
    for (i = start; i < SCHED_MAX_FDS; i++) {
        if (!slot->fdtab[i].used) {
            return (int)i;
        }
    }
    return -1;
}

static int sched_slot_find_free_ofile(const sched_task_slot_t *slot) {
    uint32_t i;
    if (!slot) {
        return -1;
    }
    for (i = 0u; i < SCHED_MAX_FDS; i++) {
        if (!slot->ofiles[i].used) {
            return (int)i;
        }
    }
    return -1;
}

static int sched_slot_close_fd(sched_task_slot_t *slot, int32_t fd) {
    sched_fdent_t *fdent;
    sched_ofile_t *of;
    if (!slot) {
        return SCHED_FD_EBADF;
    }
    of = sched_slot_ofile_by_fd(slot, fd, &fdent);
    if (!of) {
        return SCHED_FD_EBADF;
    }
    fdent->used = 0u;
    fdent->ofile_idx = SCHED_FD_OFILE_INVALID;
    fdent->fd_flags = 0u;
    if (of->refs != 0u) {
        of->refs--;
    }
    if (of->refs == 0u) {
        of->used = 0u;
        of->type = SCHED_OFILE_TYPE_NONE;
        of->status_flags = 0u;
        of->fs_backend = 0u;
        of->file_id = 0u;
        of->file_size = 0u;
        of->file_offset = 0u;
        of->file_is_dir = 0u;
    }
    return SCHED_FD_OK;
}

static void sched_slot_close_all_fds(sched_task_slot_t *slot) {
    uint32_t i;
    if (!slot) {
        return;
    }
    spinlock_lock(&slot->fd_lock);
    for (i = 0u; i < SCHED_MAX_FDS; i++) {
        if (slot->fdtab[i].used) {
            (void)sched_slot_close_fd(slot, (int32_t)i);
        }
    }
    spinlock_unlock(&slot->fd_lock);
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
            sched_clear_task(slot);
            return child_tid;
        }
    }
    if (!has_child) {
        return SCHED_WAITPID_NO_CHILD;
    }
    return 0;
}

static void sched_try_wake_sleepers(uint32_t now_tick) {
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

void sched_init(void) {
    sched_task_slot_t *root;
    spinlock_init(&g_sched_lock);
    spinlock_lock(&g_sched_lock);

    for (uint32_t i = 0u; i < SCHED_MAX_TASKS; i++) {
        sched_clear_task(&g_tasks[i]);
    }

    g_ticks = 0u;
    g_need_resched = 0u;
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

void sched_yield(void) {
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
    }
    slot->quantum_used = 0u;
    sched_mark_resched_all();
    spinlock_unlock(&g_sched_lock);
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
}

static void sched_waitq_init_locked(sched_waitq_t *q) {
    if (!q) {
        return;
    }
    for (uint32_t i = 0u; i < (SCHED_MAX_TASKS + 31u) / 32u; i++) {
        q->bits[i] = 0u;
    }
}

void sched_waitq_init(sched_waitq_t *q) {
    spinlock_lock(&g_sched_lock);
    sched_waitq_init_locked(q);
    spinlock_unlock(&g_sched_lock);
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

void sched_waitq_sleep(sched_waitq_t *q, uint32_t timeout_ticks) {
    spinlock_lock(&g_sched_lock);
    sched_waitq_sleep_locked(q, timeout_ticks);
    spinlock_unlock(&g_sched_lock);
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

int sched_fd_close(int32_t fd) {
    int rc;
    sched_task_slot_t *slot = sched_current_slot_fd_locked();
    if (!slot) {
        return SCHED_FD_EBADF;
    }
    rc = sched_slot_close_fd(slot, fd);
    spinlock_unlock(&slot->fd_lock);
    return rc;
}

int sched_fd_dup(int32_t oldfd) {
    int newfd;
    sched_fdent_t *oldfdent;
    sched_ofile_t *of;
    sched_task_slot_t *slot = sched_current_slot_fd_locked();
    if (!slot) {
        return SCHED_FD_EBADF;
    }
    of = sched_slot_ofile_by_fd(slot, oldfd, &oldfdent);
    if (!of) {
        spinlock_unlock(&slot->fd_lock);
        return SCHED_FD_EBADF;
    }
    newfd = sched_slot_find_free_fd(slot, 0u);
    if (newfd < 0) {
        spinlock_unlock(&slot->fd_lock);
        return SCHED_FD_EMFILE;
    }
    slot->fdtab[(uint32_t)newfd].used = 1u;
    slot->fdtab[(uint32_t)newfd].ofile_idx = oldfdent->ofile_idx;
    slot->fdtab[(uint32_t)newfd].fd_flags = 0u;
    of->refs++;
    spinlock_unlock(&slot->fd_lock);
    return newfd;
}

int sched_fd_dup2(int32_t oldfd, int32_t newfd) {
    sched_fdent_t *oldfdent;
    sched_ofile_t *of;
    sched_task_slot_t *slot = sched_current_slot_fd_locked();
    if (!slot) {
        return SCHED_FD_EBADF;
    }
    if (newfd < 0 || (uint32_t)newfd >= SCHED_MAX_FDS) {
        spinlock_unlock(&slot->fd_lock);
        return SCHED_FD_EINVAL;
    }
    of = sched_slot_ofile_by_fd(slot, oldfd, &oldfdent);
    if (!of) {
        spinlock_unlock(&slot->fd_lock);
        return SCHED_FD_EBADF;
    }
    if (oldfd == newfd) {
        spinlock_unlock(&slot->fd_lock);
        return newfd;
    }
    if (slot->fdtab[(uint32_t)newfd].used) {
        (void)sched_slot_close_fd(slot, newfd);
    }
    slot->fdtab[(uint32_t)newfd].used = 1u;
    slot->fdtab[(uint32_t)newfd].ofile_idx = oldfdent->ofile_idx;
    slot->fdtab[(uint32_t)newfd].fd_flags = 0u;
    of->refs++;
    spinlock_unlock(&slot->fd_lock);
    return newfd;
}

int sched_fd_fcntl_getfl(int32_t fd, uint32_t *out_flags) {
    sched_ofile_t *of;
    sched_task_slot_t *slot;
    if (!out_flags) {
        return SCHED_FD_EINVAL;
    }
    slot = sched_current_slot_fd_locked();
    if (!slot) {
        return SCHED_FD_EBADF;
    }
    of = sched_slot_ofile_by_fd(slot, fd, 0);
    if (!of) {
        spinlock_unlock(&slot->fd_lock);
        return SCHED_FD_EBADF;
    }
    *out_flags = of->status_flags;
    spinlock_unlock(&slot->fd_lock);
    return SCHED_FD_OK;
}

int sched_fd_fcntl_getfd(int32_t fd, uint32_t *out_flags) {
    sched_fdent_t *fdent = 0;
    sched_task_slot_t *slot;
    if (!out_flags) {
        return SCHED_FD_EINVAL;
    }
    slot = sched_current_slot_fd_locked();
    if (!slot) {
        return SCHED_FD_EBADF;
    }
    if (!sched_slot_ofile_by_fd(slot, fd, &fdent)) {
        spinlock_unlock(&slot->fd_lock);
        return SCHED_FD_EBADF;
    }
    *out_flags = fdent->fd_flags;
    spinlock_unlock(&slot->fd_lock);
    return SCHED_FD_OK;
}

int sched_fd_fcntl_setfd(int32_t fd, uint32_t flags) {
    sched_fdent_t *fdent = 0;
    sched_task_slot_t *slot = sched_current_slot_fd_locked();
    if (!slot) {
        return SCHED_FD_EBADF;
    }
    if (!sched_slot_ofile_by_fd(slot, fd, &fdent)) {
        spinlock_unlock(&slot->fd_lock);
        return SCHED_FD_EBADF;
    }
    fdent->fd_flags = flags & SCHED_FD_CLOEXEC;
    spinlock_unlock(&slot->fd_lock);
    return SCHED_FD_OK;
}

int sched_fd_fcntl_setfl(int32_t fd, uint32_t flags) {
    sched_ofile_t *of;
    sched_task_slot_t *slot = sched_current_slot_fd_locked();
    if (!slot) {
        return SCHED_FD_EBADF;
    }
    of = sched_slot_ofile_by_fd(slot, fd, 0);
    if (!of) {
        spinlock_unlock(&slot->fd_lock);
        return SCHED_FD_EBADF;
    }
    of->status_flags = (of->status_flags & ~SCHED_FD_O_NONBLOCK) | (flags & SCHED_FD_O_NONBLOCK);
    spinlock_unlock(&slot->fd_lock);
    return SCHED_FD_OK;
}

uint32_t sched_fd_can_read(int32_t fd) {
    uint32_t can_read = 0u;
    uint32_t acc;
    sched_ofile_t *of;
    sched_task_slot_t *slot = sched_current_slot_fd_locked();
    if (!slot) {
        return 0u;
    }
    of = sched_slot_ofile_by_fd(slot, fd, 0);
    if (of) {
        acc = of->status_flags & SCHED_FD_O_ACCMODE;
        if (acc == SCHED_FD_O_RDONLY || acc == SCHED_FD_O_RDWR) {
            can_read = (of->type == SCHED_OFILE_TYPE_STDIN ||
                        of->type == SCHED_OFILE_TYPE_DEV_ZERO ||
                        of->type == SCHED_OFILE_TYPE_DEV_NULL ||
                        of->type == SCHED_OFILE_TYPE_DEV_TTY ||
                        of->type == SCHED_OFILE_TYPE_REGULAR ||
                        of->type == SCHED_OFILE_TYPE_SOCKET) ? 1u : 0u;
        }
    }
    spinlock_unlock(&slot->fd_lock);
    return can_read;
}

uint32_t sched_fd_can_write(int32_t fd) {
    uint32_t can_write = 0u;
    uint32_t acc;
    sched_ofile_t *of;
    sched_task_slot_t *slot = sched_current_slot_fd_locked();
    if (!slot) {
        return 0u;
    }
    of = sched_slot_ofile_by_fd(slot, fd, 0);
    if (of) {
        acc = of->status_flags & SCHED_FD_O_ACCMODE;
        if (acc == SCHED_FD_O_WRONLY || acc == SCHED_FD_O_RDWR) {
            can_write = (of->type == SCHED_OFILE_TYPE_STDOUT ||
                         of->type == SCHED_OFILE_TYPE_STDERR ||
                         of->type == SCHED_OFILE_TYPE_DEV_NULL ||
                         of->type == SCHED_OFILE_TYPE_DEV_ZERO ||
                         of->type == SCHED_OFILE_TYPE_DEV_TTY ||
                         of->type == SCHED_OFILE_TYPE_REGULAR ||
                         of->type == SCHED_OFILE_TYPE_SOCKET) ? 1u : 0u;
        }
    }
    spinlock_unlock(&slot->fd_lock);
    return can_write;
}

uint32_t sched_fd_is_nonblock(int32_t fd) {
    uint32_t nonblock = 0u;
    sched_ofile_t *of;
    sched_task_slot_t *slot = sched_current_slot_fd_locked();
    if (!slot) {
        return 0u;
    }
    of = sched_slot_ofile_by_fd(slot, fd, 0);
    if (of && ((of->status_flags & SCHED_FD_O_NONBLOCK) != 0u)) {
        nonblock = 1u;
    }
    spinlock_unlock(&slot->fd_lock);
    return nonblock;
}

uint32_t sched_fd_is_open(int32_t fd) {
    uint32_t open = 0u;
    sched_task_slot_t *slot = sched_current_slot_fd_locked();
    if (!slot || fd < 0 || (uint32_t)fd >= SCHED_MAX_FDS) {
        if (slot) {
            spinlock_unlock(&slot->fd_lock);
        }
        return 0u;
    }
    open = slot->fdtab[(uint32_t)fd].used ? 1u : 0u;
    spinlock_unlock(&slot->fd_lock);
    return open;
}

uint32_t sched_fd_is_stdin(int32_t fd) {
    uint32_t is_stdin = 0u;
    sched_ofile_t *of;
    sched_task_slot_t *slot = sched_current_slot_fd_locked();
    if (!slot) {
        return 0u;
    }
    of = sched_slot_ofile_by_fd(slot, fd, 0);
    if (of && of->type == SCHED_OFILE_TYPE_STDIN) {
        is_stdin = 1u;
    }
    spinlock_unlock(&slot->fd_lock);
    return is_stdin;
}

uint32_t sched_fd_is_tty(int32_t fd) {
    uint32_t is_tty = 0u;
    sched_ofile_t *of;
    sched_task_slot_t *slot = sched_current_slot_fd_locked();
    if (!slot) {
        return 0u;
    }
    of = sched_slot_ofile_by_fd(slot, fd, 0);
    if (of) {
        is_tty = (of->type == SCHED_OFILE_TYPE_STDIN ||
                  of->type == SCHED_OFILE_TYPE_STDOUT ||
                  of->type == SCHED_OFILE_TYPE_STDERR ||
                  of->type == SCHED_OFILE_TYPE_DEV_TTY) ? 1u : 0u;
    }
    spinlock_unlock(&slot->fd_lock);
    return is_tty;
}

int sched_fd_open_special(uint32_t special_type, uint32_t status_flags) {
    int fd_idx;
    int of_idx;
    uint32_t type;
    sched_task_slot_t *slot = sched_current_slot_fd_locked();
    if (!slot) {
        return SCHED_FD_EBADF;
    }

    switch (special_type) {
        case SCHED_FD_SPECIAL_DEV_NULL:
            type = SCHED_OFILE_TYPE_DEV_NULL;
            break;
        case SCHED_FD_SPECIAL_DEV_ZERO:
            type = SCHED_OFILE_TYPE_DEV_ZERO;
            break;
        case SCHED_FD_SPECIAL_DEV_TTY:
            type = SCHED_OFILE_TYPE_DEV_TTY;
            break;
        case SCHED_FD_SPECIAL_SOCKET:
            type = SCHED_OFILE_TYPE_SOCKET;
            break;
        default:
            spinlock_unlock(&slot->fd_lock);
            return SCHED_FD_EINVAL;
    }

    fd_idx = sched_slot_find_free_fd(slot, 0u);
    if (fd_idx < 0) {
        spinlock_unlock(&slot->fd_lock);
        return SCHED_FD_EMFILE;
    }
    of_idx = sched_slot_find_free_ofile(slot);
    if (of_idx < 0) {
        spinlock_unlock(&slot->fd_lock);
        return SCHED_FD_EMFILE;
    }

    slot->ofiles[(uint32_t)of_idx].used = 1u;
    slot->ofiles[(uint32_t)of_idx].refs = 1u;
    slot->ofiles[(uint32_t)of_idx].type = type;
    slot->ofiles[(uint32_t)of_idx].status_flags =
        (status_flags & (SCHED_FD_O_ACCMODE | SCHED_FD_O_NONBLOCK));
    slot->ofiles[(uint32_t)of_idx].fs_backend = 0u;
    slot->ofiles[(uint32_t)of_idx].file_id = 0u;
    slot->ofiles[(uint32_t)of_idx].file_size = 0u;
    slot->ofiles[(uint32_t)of_idx].file_offset = 0u;
    slot->ofiles[(uint32_t)of_idx].file_is_dir = 0u;

    slot->fdtab[(uint32_t)fd_idx].used = 1u;
    slot->fdtab[(uint32_t)fd_idx].ofile_idx = (uint32_t)of_idx;
    slot->fdtab[(uint32_t)fd_idx].fd_flags = 0u;
    spinlock_unlock(&slot->fd_lock);
    return fd_idx;
}

int sched_fd_open_regular(uint32_t status_flags, uint32_t fs_backend, uint32_t file_id, uint32_t file_size,
                          uint32_t is_dir) {
    int fd_idx;
    int of_idx;
    uint32_t acc = status_flags & SCHED_FD_O_ACCMODE;
    sched_task_slot_t *slot = sched_current_slot_fd_locked();
    if (!slot) {
        return SCHED_FD_EBADF;
    }
    if (!(acc == SCHED_FD_O_RDONLY || acc == SCHED_FD_O_WRONLY || acc == SCHED_FD_O_RDWR)) {
        spinlock_unlock(&slot->fd_lock);
        return SCHED_FD_EINVAL;
    }

    fd_idx = sched_slot_find_free_fd(slot, 0u);
    if (fd_idx < 0) {
        spinlock_unlock(&slot->fd_lock);
        return SCHED_FD_EMFILE;
    }
    of_idx = sched_slot_find_free_ofile(slot);
    if (of_idx < 0) {
        spinlock_unlock(&slot->fd_lock);
        return SCHED_FD_EMFILE;
    }

    slot->ofiles[(uint32_t)of_idx].used = 1u;
    slot->ofiles[(uint32_t)of_idx].refs = 1u;
    slot->ofiles[(uint32_t)of_idx].type = SCHED_OFILE_TYPE_REGULAR;
    slot->ofiles[(uint32_t)of_idx].status_flags =
        (status_flags & (SCHED_FD_O_ACCMODE | SCHED_FD_O_NONBLOCK));
    slot->ofiles[(uint32_t)of_idx].fs_backend = fs_backend;
    slot->ofiles[(uint32_t)of_idx].file_id = file_id;
    slot->ofiles[(uint32_t)of_idx].file_size = file_size;
    slot->ofiles[(uint32_t)of_idx].file_offset = 0u;
    slot->ofiles[(uint32_t)of_idx].file_is_dir = is_dir ? 1u : 0u;

    slot->fdtab[(uint32_t)fd_idx].used = 1u;
    slot->fdtab[(uint32_t)fd_idx].ofile_idx = (uint32_t)of_idx;
    slot->fdtab[(uint32_t)fd_idx].fd_flags = 0u;
    spinlock_unlock(&slot->fd_lock);
    return fd_idx;
}

int sched_fd_get_type(int32_t fd, uint32_t *out_type) {
    sched_ofile_t *of;
    sched_task_slot_t *slot;
    if (!out_type) {
        return SCHED_FD_EINVAL;
    }
    slot = sched_current_slot_fd_locked();
    if (!slot) {
        return SCHED_FD_EBADF;
    }
    of = sched_slot_ofile_by_fd(slot, fd, 0);
    if (!of) {
        spinlock_unlock(&slot->fd_lock);
        return SCHED_FD_EBADF;
    }
    *out_type = of->type;
    spinlock_unlock(&slot->fd_lock);
    return SCHED_FD_OK;
}

int sched_fd_regular_get(int32_t fd, uint32_t *fs_backend, uint32_t *file_id, uint32_t *file_size,
                         uint32_t *file_offset, uint32_t *is_dir) {
    sched_ofile_t *of;
    sched_task_slot_t *slot = sched_current_slot_fd_locked();
    if (!slot) {
        return SCHED_FD_EBADF;
    }
    of = sched_slot_ofile_by_fd(slot, fd, 0);
    if (!of || of->type != SCHED_OFILE_TYPE_REGULAR) {
        spinlock_unlock(&slot->fd_lock);
        return SCHED_FD_EBADF;
    }
    if (fs_backend) {
        *fs_backend = of->fs_backend;
    }
    if (file_id) {
        *file_id = of->file_id;
    }
    if (file_size) {
        *file_size = of->file_size;
    }
    if (file_offset) {
        *file_offset = of->file_offset;
    }
    if (is_dir) {
        *is_dir = of->file_is_dir;
    }
    spinlock_unlock(&slot->fd_lock);
    return SCHED_FD_OK;
}

int sched_fd_regular_advance(int32_t fd, uint32_t delta, uint32_t *new_offset) {
    uint32_t cur;
    uint32_t next;
    sched_ofile_t *of;
    sched_task_slot_t *slot = sched_current_slot_fd_locked();
    if (!slot) {
        return SCHED_FD_EBADF;
    }
    of = sched_slot_ofile_by_fd(slot, fd, 0);
    if (!of || of->type != SCHED_OFILE_TYPE_REGULAR) {
        spinlock_unlock(&slot->fd_lock);
        return SCHED_FD_EBADF;
    }
    cur = of->file_offset;
    next = cur + delta;
    if (next < cur) {
        next = 0xFFFFFFFFu;
    }
    if (next > of->file_size) {
        next = of->file_size;
    }
    of->file_offset = next;
    if (new_offset) {
        *new_offset = next;
    }
    spinlock_unlock(&slot->fd_lock);
    return SCHED_FD_OK;
}

int sched_fd_regular_commit_write(int32_t fd, uint32_t written, uint32_t new_size, uint32_t *new_offset) {
    uint32_t cur;
    uint32_t next;
    sched_ofile_t *of;
    sched_task_slot_t *slot = sched_current_slot_fd_locked();
    if (!slot) {
        return SCHED_FD_EBADF;
    }
    of = sched_slot_ofile_by_fd(slot, fd, 0);
    if (!of || of->type != SCHED_OFILE_TYPE_REGULAR) {
        spinlock_unlock(&slot->fd_lock);
        return SCHED_FD_EBADF;
    }
    if (new_size > of->file_size) {
        of->file_size = new_size;
    }
    cur = of->file_offset;
    next = cur + written;
    if (next < cur) {
        next = of->file_size;
    }
    if (next > of->file_size) {
        next = of->file_size;
    }
    of->file_offset = next;
    if (new_offset) {
        *new_offset = next;
    }
    spinlock_unlock(&slot->fd_lock);
    return SCHED_FD_OK;
}

void sched_pump_once(void) {
    uint32_t *idx_ptr;
    sched_task_slot_t *slot;
    uint32_t prev_idx;
    uint32_t next;
    uint32_t cpu_id;

    spinlock_lock(&g_sched_lock);
    cpu_id = sched_cpu_id();
    idx_ptr = sched_cpu_current_idx_ptr();
    prev_idx = *idx_ptr;
    next = sched_pick_next_idx(cpu_id, prev_idx);
    if (next >= SCHED_MAX_TASKS) {
        spinlock_unlock(&g_sched_lock);
        return;
    }
    slot = &g_tasks[next];
    if (!slot->used || !slot->entry || slot->pub.state != SCHED_TASK_RUNNABLE) {
        sched_runq_del(cpu_id, next);
        spinlock_unlock(&g_sched_lock);
        return;
    }
    *idx_ptr = next;
    slot->run_cpu = cpu_id;
    sched_runq_del(cpu_id, next);
    slot->pub.state = SCHED_TASK_RUNNING;
    slot->pub.run_ticks++;
    spinlock_unlock(&g_sched_lock);

    slot->entry(&slot->pub, slot->pub.arg);

    spinlock_lock(&g_sched_lock);
    if (slot->pub.state == SCHED_TASK_RUNNING) {
        slot->pub.state = SCHED_TASK_RUNNABLE;
        sched_runq_add(cpu_id, next);
    }
    *idx_ptr = prev_idx;
    spinlock_unlock(&g_sched_lock);
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
        sched_pump_once();
        __asm__ __volatile__("" ::: "memory");
    }
}

void sched_run(void) {
    for (;;) {
        uint32_t *idx_ptr;
        sched_task_slot_t *slot;
        uint32_t cpu_id;
        spinlock_lock(&g_sched_lock);
        cpu_id = sched_cpu_id();
        idx_ptr = sched_cpu_current_idx_ptr();
        if (g_need_resched || !sched_idx_runnable(*idx_ptr)) {
            g_need_resched = 0u;
            *idx_ptr = sched_pick_next_idx(cpu_id, *idx_ptr);
        }

        slot = &g_tasks[*idx_ptr];
        if (!slot->used || !slot->entry || slot->pub.state != SCHED_TASK_RUNNABLE) {
            sched_runq_del(cpu_id, *idx_ptr);
            g_need_resched = 1u;
            spinlock_unlock(&g_sched_lock);
            continue;
        }

        slot->run_cpu = cpu_id;
        sched_runq_del(cpu_id, *idx_ptr);
        slot->pub.state = SCHED_TASK_RUNNING;
        slot->pub.run_ticks++;
        spinlock_unlock(&g_sched_lock);

        slot->entry(&slot->pub, slot->pub.arg);

        spinlock_lock(&g_sched_lock);
        if (slot->pub.state == SCHED_TASK_RUNNING) {
            slot->pub.state = SCHED_TASK_RUNNABLE;
            sched_runq_add(cpu_id, *idx_ptr);
        }
        spinlock_unlock(&g_sched_lock);
    }
}
