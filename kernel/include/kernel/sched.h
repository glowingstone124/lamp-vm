#ifndef LAMP_KERNEL_SCHED_H
#define LAMP_KERNEL_SCHED_H

#include "types.h"

typedef struct sched_task sched_task_t;
typedef struct sched_waitq sched_waitq_t;
typedef void (*sched_task_entry_t)(sched_task_t *task, void *arg);

typedef struct sched_sigaction32 {
    uint32_t handler;
    uint32_t flags;
    uint32_t mask;
    uint32_t restorer;
} sched_sigaction32_t;

enum {
    SCHED_MAX_TASKS = 16u,
    SCHED_NAME_MAX = 16u,
    SCHED_MAX_FDS = 32u,
    SCHED_CWD_CAP = 64u
};

enum {
    SCHED_SIGNAL_MAX = 31u,
    SCHED_SIGNAL_DFL = 0u,
    SCHED_SIGNAL_IGN = 1u,
    SCHED_SIGNAL_HUP = 1u,
    SCHED_SIGNAL_INT = 2u,
    SCHED_SIGNAL_QUIT = 3u,
    SCHED_SIGNAL_KILL = 9u,
    SCHED_SIGNAL_TERM = 15u,
    SCHED_SIGNAL_CHLD = 17u,
    SCHED_SIGNAL_STOP = 19u
};

enum {
    SCHED_SIG_BLOCK = 0u,
    SCHED_SIG_UNBLOCK = 1u,
    SCHED_SIG_SETMASK = 2u
};

enum {
    SCHED_WAITPID_ANY = -1,
    SCHED_WAITPID_WNOHANG = 1u,
    SCHED_WAITPID_NO_CHILD = -1,
    SCHED_WAITPID_BLOCKED = -2
};

enum {
    SCHED_TASK_UNUSED = 0u,
    SCHED_TASK_RUNNABLE = 1u,
    SCHED_TASK_RUNNING = 2u,
    SCHED_TASK_SLEEPING = 3u,
    SCHED_TASK_BLOCKED = 4u,
    SCHED_TASK_ZOMBIE = 5u
};

enum {
    SCHED_TASK_KIND_KERNEL = 0u,
    SCHED_TASK_KIND_USER = 1u
};

enum {
    SCHED_EXEC_STATE_NONE = 0u,
    SCHED_EXEC_STATE_KERNEL = 1u,
    SCHED_EXEC_STATE_USER = 2u
};

enum {
    SCHED_FD_O_ACCMODE = 0x00000003u,
    SCHED_FD_O_RDONLY = 0x00000000u,
    SCHED_FD_O_WRONLY = 0x00000001u,
    SCHED_FD_O_RDWR = 0x00000002u,
    SCHED_FD_O_NONBLOCK = 0x00000800u
};

enum {
    SCHED_FD_CLOEXEC = 0x00000001u
};

enum {
    SCHED_FD_OK = 0,
    SCHED_FD_EBADF = -1,
    SCHED_FD_EMFILE = -2,
    SCHED_FD_EINVAL = -3,
    SCHED_FD_ESPIPE = -4,
    SCHED_FD_EAGAIN = -5,
    SCHED_FD_EPIPE = -6
};

enum {
    SCHED_SIGNAL_OK = 0,
    SCHED_SIGNAL_EINVAL = -1,
    SCHED_SIGNAL_ESRCH = -2,
    SCHED_SIGNAL_EPERM = -3
};

enum {
    SCHED_FD_TYPE_NONE = 0u,
    SCHED_FD_TYPE_STDIN = 1u,
    SCHED_FD_TYPE_STDOUT = 2u,
    SCHED_FD_TYPE_STDERR = 3u,
    SCHED_FD_TYPE_DEV_NULL = 4u,
    SCHED_FD_TYPE_DEV_ZERO = 5u,
    SCHED_FD_TYPE_DEV_TTY = 6u,
    SCHED_FD_TYPE_SOCKET = 7u,
    SCHED_FD_TYPE_REGULAR = 8u,
    SCHED_FD_TYPE_PIPE_READ = 9u,
    SCHED_FD_TYPE_PIPE_WRITE = 10u
};

enum {
    SCHED_FD_SPECIAL_DEV_NULL = 1u,
    SCHED_FD_SPECIAL_DEV_ZERO = 2u,
    SCHED_FD_SPECIAL_DEV_TTY = 3u,
    SCHED_FD_SPECIAL_SOCKET = 4u
};

struct sched_task {
    uint32_t tid;
    uint32_t pid;
    int32_t ppid;
    uint32_t state;
    uint32_t kind;
    uint32_t exec_state;
    uint32_t wake_tick;
    uint32_t run_ticks;
    void *arg;
};

struct sched_waitq {
    uint32_t bits[(SCHED_MAX_TASKS + 31u) / 32u];
};

void sched_init(void);
int sched_spawn(const char *name, sched_task_entry_t entry, void *arg);
void sched_exit(void);
void sched_exit_code(uint32_t code);
void sched_yield(void);
void sched_sleep_ticks(uint32_t ticks);
int sched_current_tid(void);
int sched_current_ppid(void);
int sched_current_getcwd(char *dst, uint32_t cap);
int sched_current_setcwd(const char *path);
uint32_t sched_current_umask(uint32_t new_mask);
uint32_t sched_tick_period_us(void);
int sched_waitpid(int32_t pid, uint32_t options, uint32_t *status_out);
int sched_vfork(uint32_t abi_addr);
void sched_vfork_release_parent(void);

int sched_fd_close(int32_t fd);
int sched_fd_dup(int32_t oldfd);
int sched_fd_dup_min(int32_t oldfd, int32_t minfd, uint32_t fd_flags);
int sched_fd_dup2(int32_t oldfd, int32_t newfd);
int sched_fd_fcntl_getfd(int32_t fd, uint32_t *out_flags);
int sched_fd_fcntl_setfd(int32_t fd, uint32_t flags);
int sched_fd_fcntl_getfl(int32_t fd, uint32_t *out_flags);
int sched_fd_fcntl_setfl(int32_t fd, uint32_t flags);
uint32_t sched_fd_can_read(int32_t fd);
uint32_t sched_fd_can_write(int32_t fd);
uint32_t sched_fd_is_nonblock(int32_t fd);
uint32_t sched_fd_is_open(int32_t fd);
uint32_t sched_fd_is_stdin(int32_t fd);
uint32_t sched_fd_is_tty(int32_t fd);
int sched_fd_open_special(uint32_t special_type, uint32_t status_flags);
int sched_fd_open_regular(uint32_t status_flags, uint32_t fs_backend, uint32_t file_id, uint32_t file_size,
                          uint32_t is_dir);
int sched_fd_pipe(uint32_t pipefd_addr);
int sched_fd_close_cloexec(void);
int sched_fd_get_type(int32_t fd, uint32_t *out_type);
int sched_fd_regular_get(int32_t fd, uint32_t *fs_backend, uint32_t *file_id, uint32_t *file_size,
                         uint32_t *file_offset, uint32_t *is_dir);
int sched_fd_lseek(int32_t fd, int32_t offset, uint32_t whence, uint32_t *new_offset);
int sched_fd_pipe_read(int32_t fd, uint8_t *dst, uint32_t len);
int sched_fd_pipe_write(int32_t fd, const uint8_t *src, uint32_t len);
uint32_t sched_fd_pipe_read_ready(int32_t fd);
uint32_t sched_fd_pipe_write_ready(int32_t fd);
int sched_fd_regular_advance(int32_t fd, uint32_t delta, uint32_t *new_offset);
int sched_fd_regular_commit_write(int32_t fd, uint32_t written, uint32_t new_size, uint32_t *new_offset);

void sched_task_set_kind(uint32_t kind);
void sched_task_set_exec_state(uint32_t exec_state);
uint32_t sched_task_get_kind(void);
uint32_t sched_task_get_exec_state(void);
int sched_signal_action(uint32_t sig, const sched_sigaction32_t *act, sched_sigaction32_t *oldact);
int sched_signal_mask(uint32_t how, uint32_t set, uint32_t *oldset);
int sched_signal_kill(int32_t pid, uint32_t sig);

void sched_waitq_init(sched_waitq_t *q);
void sched_waitq_sleep(sched_waitq_t *q, uint32_t timeout_ticks);
void sched_waitq_wake_one(sched_waitq_t *q);
void sched_waitq_wake_all(sched_waitq_t *q);
void sched_block_until_runnable(void);
void sched_pump_once(void);

void sched_run(void);
void schedule_tick(void);
unsigned int sched_ticks(void);

#endif
