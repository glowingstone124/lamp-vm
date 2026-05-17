#ifndef LAMP_KERNEL_SCHED_INTERNAL_H
#define LAMP_KERNEL_SCHED_INTERNAL_H

#include "../include/kernel/platform.h"
#include "../include/kernel/sched.h"
#include "../include/kernel/smp.h"
#include "../include/kernel/spinlock.h"

#define SCHED_TICK_PERIOD_US 5000u
#define SCHED_QUANTUM_TICKS 4u
#define SCHED_MAX_CPUS 32u
#define SCHED_STACK_BITMAP_WORDS ((VM_STACK_POOL_SLOTS + 31u) / 32u)
#define SCHED_FD_OFILE_INVALID ((uint32_t)~0u)

typedef struct sched_stack_ctx {
    uint32_t call_base;
    uint32_t data_base;
    uint32_t isr_base;
    uint32_t csp;
    uint32_t dsp;
    uint32_t isp;
    uint32_t irq_masked;
    uint32_t in_interrupt;
    uint32_t pool_slot;
    uint32_t valid;
    uint32_t regs[32];
} sched_stack_ctx_t;

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
    SCHED_OFILE_TYPE_REGULAR = SCHED_FD_TYPE_REGULAR,
    SCHED_OFILE_TYPE_PIPE_READ = SCHED_FD_TYPE_PIPE_READ,
    SCHED_OFILE_TYPE_PIPE_WRITE = SCHED_FD_TYPE_PIPE_WRITE
};

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
    uint32_t vfork_child_start;
    uint32_t vfork_child_active;
    sched_waitq_t *waitq;
    sched_waitq_t child_waitq;
    sched_ofile_t ofiles[SCHED_MAX_FDS];
    sched_fdent_t fdtab[SCHED_MAX_FDS];
    sched_stack_ctx_t stack_ctx;
    sched_stack_ctx_t vfork_resume_ctx;
    uint32_t vfork_resume_ret;
    uint32_t vfork_resume_valid;
    uint32_t syscall_abi_addr;
    char cwd[SCHED_CWD_CAP];
    uint32_t file_umask;
    uint32_t sig_mask;
    uint32_t sig_pending;
    sched_sigaction32_t sig_action[SCHED_SIGNAL_MAX + 1u];
    spinlock_t fd_lock;
} sched_task_slot_t;

typedef struct sched_runq {
    spinlock_t lock;
    uint32_t bits[(SCHED_MAX_TASKS + 31u) / 32u];
} sched_runq_t;

extern volatile unsigned int g_ticks;
extern volatile unsigned int g_need_resched;
extern sched_task_slot_t g_tasks[SCHED_MAX_TASKS];
extern uint32_t g_cpu_current_idx[SCHED_MAX_CPUS];
extern uint32_t g_next_tid;
extern uint32_t g_spawn_cpu_rr;
extern sched_runq_t g_runq[SCHED_MAX_CPUS];
extern spinlock_t g_sched_lock;
extern uint32_t g_stack_pool_bitmap[SCHED_STACK_BITMAP_WORDS];
extern uint32_t g_stack_pool_reserved;
extern sched_stack_ctx_t g_sched_ctx[SCHED_MAX_CPUS];
extern uint8_t __text_start[];

static inline void timer_program_period_us(uint32_t period_us) {
    *(volatile uint32_t *)(uintptr_t)TIMER_MMIO_BASE = period_us;
}

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

static inline uint32_t sched_stack_pool_base(void) {
    return VM_STACK_POOL_BASE;
}

static inline uint32_t sched_stack_pool_stride(void) {
    return (uint32_t)VM_STACK_SLOT_BYTES;
}

static inline uint32_t sched_stack_pool_slot_base(uint32_t slot) {
    return sched_stack_pool_base() + sched_stack_pool_stride() * slot;
}

static inline void sched_stack_bitmap_set(uint32_t slot) {
    g_stack_pool_bitmap[slot >> 5u] |= (1u << (slot & 31u));
}

static inline void sched_stack_bitmap_clr(uint32_t slot) {
    g_stack_pool_bitmap[slot >> 5u] &= ~(1u << (slot & 31u));
}

static inline uint32_t sched_stack_bitmap_tst(uint32_t slot) {
    return (g_stack_pool_bitmap[slot >> 5u] >> (slot & 31u)) & 1u;
}

static inline uint32_t sched_tick_now(void) {
    return (uint32_t)g_ticks;
}

static inline void waitq_set_bit(sched_waitq_t *q, uint32_t idx) {
    q->bits[idx >> 5u] |= (1u << (idx & 31u));
}

static inline void waitq_clear_bit(sched_waitq_t *q, uint32_t idx) {
    q->bits[idx >> 5u] &= ~(1u << (idx & 31u));
}

static inline uint32_t waitq_test_bit(const sched_waitq_t *q, uint32_t idx) {
    return (q->bits[idx >> 5u] >> (idx & 31u)) & 1u;
}

void sched_fd_table_clear(sched_task_slot_t *slot);
void sched_fd_table_init_stdio(sched_task_slot_t *slot);
int sched_fd_table_clone(sched_task_slot_t *dst, const sched_task_slot_t *src);
int sched_slot_close_fd(sched_task_slot_t *slot, int32_t fd);
void sched_slot_close_all_fds(sched_task_slot_t *slot);

int sched_alloc_slot(void);
void sched_clear_task(sched_task_slot_t *slot);
uint32_t sched_cpu_cap(void);
uint32_t sched_cpu_normalize(uint32_t cpu_id);
void sched_mark_resched_all(void);
void sched_runq_add(uint32_t cpu_id, uint32_t idx);
void sched_runq_del(uint32_t cpu_id, uint32_t idx);
uint32_t sched_slot_index(const sched_task_slot_t *slot);
sched_task_slot_t *sched_current_slot(void);
sched_task_slot_t *sched_current_slot_fd_locked(void);
void sched_ctx_swap(sched_stack_ctx_t *from, const sched_stack_ctx_t *to);
void sched_ctx_capture(sched_stack_ctx_t *out);
void sched_switch_current_to_scheduler(void);
void sched_waitq_init_locked(sched_waitq_t *q);
void sched_try_wake_sleepers(uint32_t now_tick);
int sched_stack_alloc_locked(sched_stack_ctx_t *ctx_out);
void sched_stack_prepare_bootstrap_locked(sched_task_slot_t *slot);
void sched_stack_free_locked(sched_task_slot_t *slot);

#endif
