#include "sched_internal.h"
#include "../include/kernel/syscall.h"

enum {
    SCHED_PIPE_MAX = 16u,
    SCHED_PIPE_CAP = 256u
};

typedef struct sched_pipe {
    uint32_t used;
    uint32_t read_refs;
    uint32_t write_refs;
    uint32_t head;
    uint32_t len;
    uint8_t buf[SCHED_PIPE_CAP];
} sched_pipe_t;

static spinlock_t g_pipe_lock;
static volatile uint32_t g_pipe_inited;
static sched_pipe_t g_pipes[SCHED_PIPE_MAX];

static void sched_pipe_init_once(void) {
    if (g_pipe_inited != 0u) {
        return;
    }
    spinlock_init(&g_pipe_lock);
    for (uint32_t i = 0u; i < SCHED_PIPE_MAX; i++) {
        g_pipes[i].used = 0u;
        g_pipes[i].read_refs = 0u;
        g_pipes[i].write_refs = 0u;
        g_pipes[i].head = 0u;
        g_pipes[i].len = 0u;
    }
    g_pipe_inited = 1u;
}

static int sched_pipe_alloc_locked(void) {
    for (uint32_t i = 0u; i < SCHED_PIPE_MAX; i++) {
        if (g_pipes[i].used == 0u) {
            g_pipes[i].used = 1u;
            g_pipes[i].read_refs = 0u;
            g_pipes[i].write_refs = 0u;
            g_pipes[i].head = 0u;
            g_pipes[i].len = 0u;
            return (int)i;
        }
    }
    return -1;
}

static void sched_pipe_endpoint_inc(uint32_t type, uint32_t pipe_id) {
    if (type != SCHED_OFILE_TYPE_PIPE_READ && type != SCHED_OFILE_TYPE_PIPE_WRITE) {
        return;
    }
    sched_pipe_init_once();
    spinlock_lock(&g_pipe_lock);
    if (pipe_id < SCHED_PIPE_MAX && g_pipes[pipe_id].used != 0u) {
        if (type == SCHED_OFILE_TYPE_PIPE_READ) {
            g_pipes[pipe_id].read_refs++;
        } else {
            g_pipes[pipe_id].write_refs++;
        }
    }
    spinlock_unlock(&g_pipe_lock);
}

static void sched_pipe_endpoint_dec(uint32_t type, uint32_t pipe_id) {
    if (type != SCHED_OFILE_TYPE_PIPE_READ && type != SCHED_OFILE_TYPE_PIPE_WRITE) {
        return;
    }
    sched_pipe_init_once();
    spinlock_lock(&g_pipe_lock);
    if (pipe_id < SCHED_PIPE_MAX && g_pipes[pipe_id].used != 0u) {
        if (type == SCHED_OFILE_TYPE_PIPE_READ) {
            if (g_pipes[pipe_id].read_refs != 0u) {
                g_pipes[pipe_id].read_refs--;
            }
        } else if (g_pipes[pipe_id].write_refs != 0u) {
            g_pipes[pipe_id].write_refs--;
        }
        if (g_pipes[pipe_id].read_refs == 0u && g_pipes[pipe_id].write_refs == 0u) {
            g_pipes[pipe_id].used = 0u;
            g_pipes[pipe_id].head = 0u;
            g_pipes[pipe_id].len = 0u;
        }
    }
    spinlock_unlock(&g_pipe_lock);
}

static void sched_ofile_reset(sched_ofile_t *of) {
    if (!of) {
        return;
    }
    of->used = 0u;
    of->refs = 0u;
    of->type = SCHED_OFILE_TYPE_NONE;
    of->status_flags = 0u;
    of->fs_backend = 0u;
    of->file_id = 0u;
    of->file_size = 0u;
    of->file_offset = 0u;
    of->file_is_dir = 0u;
}

static void sched_fdent_reset(sched_fdent_t *fdent) {
    if (!fdent) {
        return;
    }
    fdent->used = 0u;
    fdent->ofile_idx = SCHED_FD_OFILE_INVALID;
    fdent->fd_flags = 0u;
}

void sched_fd_table_clear(sched_task_slot_t *slot) {
    uint32_t i;
    if (!slot) {
        return;
    }
    for (i = 0u; i < SCHED_MAX_FDS; i++) {
        sched_ofile_reset(&slot->ofiles[i]);
        sched_fdent_reset(&slot->fdtab[i]);
    }
}

void sched_fd_table_init_stdio(sched_task_slot_t *slot) {
    if (!slot || SCHED_MAX_FDS < 3u) {
        return;
    }
    sched_pipe_init_once();
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

int sched_fd_table_clone(sched_task_slot_t *dst, const sched_task_slot_t *src) {
    uint32_t i;
    if (!dst || !src) {
        return SCHED_FD_EINVAL;
    }
    sched_fd_table_clear(dst);
    for (i = 0u; i < SCHED_MAX_FDS; i++) {
        dst->ofiles[i].used = src->ofiles[i].used;
        dst->ofiles[i].refs = src->ofiles[i].refs;
        dst->ofiles[i].type = src->ofiles[i].type;
        dst->ofiles[i].status_flags = src->ofiles[i].status_flags;
        dst->ofiles[i].fs_backend = src->ofiles[i].fs_backend;
        dst->ofiles[i].file_id = src->ofiles[i].file_id;
        dst->ofiles[i].file_size = src->ofiles[i].file_size;
        dst->ofiles[i].file_offset = src->ofiles[i].file_offset;
        dst->ofiles[i].file_is_dir = src->ofiles[i].file_is_dir;

        dst->fdtab[i].used = src->fdtab[i].used;
        dst->fdtab[i].ofile_idx = src->fdtab[i].ofile_idx;
        dst->fdtab[i].fd_flags = src->fdtab[i].fd_flags;
    }
    for (i = 0u; i < SCHED_MAX_FDS; i++) {
        dst->ofiles[i].refs = 0u;
    }
    for (i = 0u; i < SCHED_MAX_FDS; i++) {
        uint32_t of_idx;
        if (!dst->fdtab[i].used) {
            continue;
        }
        of_idx = dst->fdtab[i].ofile_idx;
        if (of_idx >= SCHED_MAX_FDS || !dst->ofiles[of_idx].used) {
            return SCHED_FD_EBADF;
        }
        dst->ofiles[of_idx].refs++;
        sched_pipe_endpoint_inc(dst->ofiles[of_idx].type, dst->ofiles[of_idx].fs_backend);
    }
    return SCHED_FD_OK;
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

int sched_slot_close_fd(sched_task_slot_t *slot, int32_t fd) {
    sched_fdent_t *fdent;
    sched_ofile_t *of;
    if (!slot) {
        return SCHED_FD_EBADF;
    }
    of = sched_slot_ofile_by_fd(slot, fd, &fdent);
    if (!of) {
        return SCHED_FD_EBADF;
    }
    sched_fdent_reset(fdent);
    sched_pipe_endpoint_dec(of->type, of->fs_backend);
    if (of->refs != 0u) {
        of->refs--;
    }
    if (of->refs == 0u) {
        sched_ofile_reset(of);
    }
    return SCHED_FD_OK;
}

void sched_slot_close_all_fds(sched_task_slot_t *slot) {
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

static uint32_t sched_fd_is_readable_type(uint32_t type) {
    return (type == SCHED_OFILE_TYPE_STDIN ||
            type == SCHED_OFILE_TYPE_DEV_ZERO ||
            type == SCHED_OFILE_TYPE_DEV_NULL ||
            type == SCHED_OFILE_TYPE_DEV_TTY ||
            type == SCHED_OFILE_TYPE_REGULAR ||
            type == SCHED_OFILE_TYPE_PIPE_READ ||
            type == SCHED_OFILE_TYPE_SOCKET) ? 1u : 0u;
}

static uint32_t sched_fd_is_writable_type(uint32_t type) {
    return (type == SCHED_OFILE_TYPE_STDOUT ||
            type == SCHED_OFILE_TYPE_STDERR ||
            type == SCHED_OFILE_TYPE_DEV_NULL ||
            type == SCHED_OFILE_TYPE_DEV_ZERO ||
            type == SCHED_OFILE_TYPE_DEV_TTY ||
            type == SCHED_OFILE_TYPE_REGULAR ||
            type == SCHED_OFILE_TYPE_PIPE_WRITE ||
            type == SCHED_OFILE_TYPE_SOCKET) ? 1u : 0u;
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
    return sched_fd_dup_min(oldfd, 0, 0u);
}

int sched_fd_dup_min(int32_t oldfd, int32_t minfd, uint32_t fd_flags) {
    int newfd;
    sched_fdent_t *oldfdent;
    sched_ofile_t *of;
    sched_task_slot_t *slot = sched_current_slot_fd_locked();
    if (!slot) {
        return SCHED_FD_EBADF;
    }
    if (minfd < 0) {
        spinlock_unlock(&slot->fd_lock);
        return SCHED_FD_EINVAL;
    }
    if ((uint32_t)minfd >= SCHED_MAX_FDS) {
        spinlock_unlock(&slot->fd_lock);
        return SCHED_FD_EBADF;
    }
    of = sched_slot_ofile_by_fd(slot, oldfd, &oldfdent);
    if (!of) {
        spinlock_unlock(&slot->fd_lock);
        return SCHED_FD_EBADF;
    }
    newfd = sched_slot_find_free_fd(slot, (uint32_t)minfd);
    if (newfd < 0) {
        spinlock_unlock(&slot->fd_lock);
        return SCHED_FD_EMFILE;
    }
    slot->fdtab[(uint32_t)newfd].used = 1u;
    slot->fdtab[(uint32_t)newfd].ofile_idx = oldfdent->ofile_idx;
    slot->fdtab[(uint32_t)newfd].fd_flags = fd_flags & SCHED_FD_CLOEXEC;
    of->refs++;
    sched_pipe_endpoint_inc(of->type, of->fs_backend);
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
    sched_pipe_endpoint_inc(of->type, of->fs_backend);
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
            can_read = sched_fd_is_readable_type(of->type);
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
            can_write = sched_fd_is_writable_type(of->type);
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

int sched_fd_pipe(uint32_t pipefd_addr) {
    int pipe_id;
    int read_fd;
    int write_fd;
    int read_of;
    int write_of;
    sched_task_slot_t *slot = sched_current_slot_fd_locked();
    if (!slot) {
        return SCHED_FD_EBADF;
    }

    read_fd = sched_slot_find_free_fd(slot, 0u);
    if (read_fd < 0) {
        spinlock_unlock(&slot->fd_lock);
        return SCHED_FD_EMFILE;
    }
    write_fd = sched_slot_find_free_fd(slot, (uint32_t)read_fd + 1u);
    if (write_fd < 0) {
        spinlock_unlock(&slot->fd_lock);
        return SCHED_FD_EMFILE;
    }
    read_of = sched_slot_find_free_ofile(slot);
    if (read_of < 0) {
        spinlock_unlock(&slot->fd_lock);
        return SCHED_FD_EMFILE;
    }
    slot->ofiles[(uint32_t)read_of].used = 1u;
    write_of = sched_slot_find_free_ofile(slot);
    slot->ofiles[(uint32_t)read_of].used = 0u;
    if (write_of < 0) {
        spinlock_unlock(&slot->fd_lock);
        return SCHED_FD_EMFILE;
    }

    sched_pipe_init_once();
    spinlock_lock(&g_pipe_lock);
    pipe_id = sched_pipe_alloc_locked();
    if (pipe_id < 0) {
        spinlock_unlock(&g_pipe_lock);
        spinlock_unlock(&slot->fd_lock);
        return SCHED_FD_EMFILE;
    }
    g_pipes[(uint32_t)pipe_id].read_refs = 1u;
    g_pipes[(uint32_t)pipe_id].write_refs = 1u;
    spinlock_unlock(&g_pipe_lock);

    slot->ofiles[(uint32_t)read_of].used = 1u;
    slot->ofiles[(uint32_t)read_of].refs = 1u;
    slot->ofiles[(uint32_t)read_of].type = SCHED_OFILE_TYPE_PIPE_READ;
    slot->ofiles[(uint32_t)read_of].status_flags = SCHED_FD_O_RDONLY;
    slot->ofiles[(uint32_t)read_of].fs_backend = (uint32_t)pipe_id;
    slot->ofiles[(uint32_t)read_of].file_id = 0u;
    slot->ofiles[(uint32_t)read_of].file_size = 0u;
    slot->ofiles[(uint32_t)read_of].file_offset = 0u;
    slot->ofiles[(uint32_t)read_of].file_is_dir = 0u;

    slot->ofiles[(uint32_t)write_of].used = 1u;
    slot->ofiles[(uint32_t)write_of].refs = 1u;
    slot->ofiles[(uint32_t)write_of].type = SCHED_OFILE_TYPE_PIPE_WRITE;
    slot->ofiles[(uint32_t)write_of].status_flags = SCHED_FD_O_WRONLY;
    slot->ofiles[(uint32_t)write_of].fs_backend = (uint32_t)pipe_id;
    slot->ofiles[(uint32_t)write_of].file_id = 0u;
    slot->ofiles[(uint32_t)write_of].file_size = 0u;
    slot->ofiles[(uint32_t)write_of].file_offset = 0u;
    slot->ofiles[(uint32_t)write_of].file_is_dir = 0u;

    slot->fdtab[(uint32_t)read_fd].used = 1u;
    slot->fdtab[(uint32_t)read_fd].ofile_idx = (uint32_t)read_of;
    slot->fdtab[(uint32_t)read_fd].fd_flags = 0u;
    slot->fdtab[(uint32_t)write_fd].used = 1u;
    slot->fdtab[(uint32_t)write_fd].ofile_idx = (uint32_t)write_of;
    slot->fdtab[(uint32_t)write_fd].fd_flags = 0u;

    *(volatile uint32_t *)(uintptr_t)(pipefd_addr + 0u) = (uint32_t)read_fd;
    *(volatile uint32_t *)(uintptr_t)(pipefd_addr + 4u) = (uint32_t)write_fd;
    spinlock_unlock(&slot->fd_lock);
    return SCHED_FD_OK;
}

int sched_fd_close_cloexec(void) {
    int closed = 0;
    sched_task_slot_t *slot = sched_current_slot_fd_locked();
    if (!slot) {
        return SCHED_FD_EBADF;
    }
    for (uint32_t i = 0u; i < SCHED_MAX_FDS; i++) {
        if (!slot->fdtab[i].used) {
            continue;
        }
        if ((slot->fdtab[i].fd_flags & SCHED_FD_CLOEXEC) == 0u) {
            continue;
        }
        if (sched_slot_close_fd(slot, (int32_t)i) == SCHED_FD_OK) {
            closed++;
        }
    }
    spinlock_unlock(&slot->fd_lock);
    return closed;
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

int sched_fd_lseek(int32_t fd, int32_t offset, uint32_t whence, uint32_t *new_offset) {
    uint32_t base = 0u;
    uint32_t next = 0u;
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
    if (of->type != SCHED_OFILE_TYPE_REGULAR) {
        spinlock_unlock(&slot->fd_lock);
        return SCHED_FD_ESPIPE;
    }

    switch (whence) {
        case SYS_SEEK_SET:
            base = 0u;
            break;
        case SYS_SEEK_CUR:
            base = of->file_offset;
            break;
        case SYS_SEEK_END:
            base = of->file_size;
            break;
        default:
            spinlock_unlock(&slot->fd_lock);
            return SCHED_FD_EINVAL;
    }

    if (offset < 0) {
        uint32_t mag = (uint32_t)(-(offset + 1)) + 1u;
        if (base < mag) {
            spinlock_unlock(&slot->fd_lock);
            return SCHED_FD_EINVAL;
        }
        next = base - mag;
    } else {
        uint32_t pos = (uint32_t)offset;
        if (base > 0xFFFFFFFFu - pos) {
            spinlock_unlock(&slot->fd_lock);
            return SCHED_FD_EINVAL;
        }
        next = base + pos;
    }

    of->file_offset = next;
    if (new_offset) {
        *new_offset = next;
    }
    spinlock_unlock(&slot->fd_lock);
    return SCHED_FD_OK;
}

static int sched_fd_pipe_id_for_fd(int32_t fd, uint32_t want_type, uint32_t *pipe_id_out) {
    sched_ofile_t *of;
    sched_task_slot_t *slot = sched_current_slot_fd_locked();
    if (!slot) {
        return SCHED_FD_EBADF;
    }
    of = sched_slot_ofile_by_fd(slot, fd, 0);
    if (!of || of->type != want_type || of->fs_backend >= SCHED_PIPE_MAX) {
        spinlock_unlock(&slot->fd_lock);
        return SCHED_FD_EBADF;
    }
    if (pipe_id_out) {
        *pipe_id_out = of->fs_backend;
    }
    spinlock_unlock(&slot->fd_lock);
    return SCHED_FD_OK;
}

int sched_fd_pipe_read(int32_t fd, uint8_t *dst, uint32_t len) {
    uint32_t pipe_id = 0u;
    uint32_t copied = 0u;
    int rc;
    if (!dst || len == 0u) {
        return 0;
    }
    rc = sched_fd_pipe_id_for_fd(fd, SCHED_OFILE_TYPE_PIPE_READ, &pipe_id);
    if (rc != SCHED_FD_OK) {
        return rc;
    }
    sched_pipe_init_once();
    spinlock_lock(&g_pipe_lock);
    if (pipe_id >= SCHED_PIPE_MAX || g_pipes[pipe_id].used == 0u) {
        spinlock_unlock(&g_pipe_lock);
        return SCHED_FD_EBADF;
    }
    while (copied < len && g_pipes[pipe_id].len != 0u) {
        dst[copied] = g_pipes[pipe_id].buf[g_pipes[pipe_id].head];
        g_pipes[pipe_id].head = (g_pipes[pipe_id].head + 1u) % SCHED_PIPE_CAP;
        g_pipes[pipe_id].len--;
        copied++;
    }
    if (copied != 0u) {
        spinlock_unlock(&g_pipe_lock);
        return (int)copied;
    }
    if (g_pipes[pipe_id].write_refs == 0u) {
        spinlock_unlock(&g_pipe_lock);
        return 0;
    }
    spinlock_unlock(&g_pipe_lock);
    return SCHED_FD_EAGAIN;
}

int sched_fd_pipe_write(int32_t fd, const uint8_t *src, uint32_t len) {
    uint32_t pipe_id = 0u;
    uint32_t written = 0u;
    int rc;
    if (!src || len == 0u) {
        return 0;
    }
    rc = sched_fd_pipe_id_for_fd(fd, SCHED_OFILE_TYPE_PIPE_WRITE, &pipe_id);
    if (rc != SCHED_FD_OK) {
        return rc;
    }
    sched_pipe_init_once();
    spinlock_lock(&g_pipe_lock);
    if (pipe_id >= SCHED_PIPE_MAX || g_pipes[pipe_id].used == 0u) {
        spinlock_unlock(&g_pipe_lock);
        return SCHED_FD_EBADF;
    }
    if (g_pipes[pipe_id].read_refs == 0u) {
        spinlock_unlock(&g_pipe_lock);
        return SCHED_FD_EPIPE;
    }
    while (written < len && g_pipes[pipe_id].len < SCHED_PIPE_CAP) {
        uint32_t tail = (g_pipes[pipe_id].head + g_pipes[pipe_id].len) % SCHED_PIPE_CAP;
        g_pipes[pipe_id].buf[tail] = src[written];
        g_pipes[pipe_id].len++;
        written++;
    }
    if (written != 0u) {
        spinlock_unlock(&g_pipe_lock);
        return (int)written;
    }
    spinlock_unlock(&g_pipe_lock);
    return SCHED_FD_EAGAIN;
}

uint32_t sched_fd_pipe_read_ready(int32_t fd) {
    uint32_t pipe_id = 0u;
    uint32_t ready = 0u;
    if (sched_fd_pipe_id_for_fd(fd, SCHED_OFILE_TYPE_PIPE_READ, &pipe_id) != SCHED_FD_OK) {
        return 0u;
    }
    sched_pipe_init_once();
    spinlock_lock(&g_pipe_lock);
    if (pipe_id < SCHED_PIPE_MAX && g_pipes[pipe_id].used != 0u) {
        ready = (g_pipes[pipe_id].len != 0u || g_pipes[pipe_id].write_refs == 0u) ? 1u : 0u;
    }
    spinlock_unlock(&g_pipe_lock);
    return ready;
}

uint32_t sched_fd_pipe_write_ready(int32_t fd) {
    uint32_t pipe_id = 0u;
    uint32_t ready = 0u;
    if (sched_fd_pipe_id_for_fd(fd, SCHED_OFILE_TYPE_PIPE_WRITE, &pipe_id) != SCHED_FD_OK) {
        return 0u;
    }
    sched_pipe_init_once();
    spinlock_lock(&g_pipe_lock);
    if (pipe_id < SCHED_PIPE_MAX && g_pipes[pipe_id].used != 0u) {
        ready = (g_pipes[pipe_id].read_refs != 0u && g_pipes[pipe_id].len < SCHED_PIPE_CAP) ? 1u : 0u;
    }
    spinlock_unlock(&g_pipe_lock);
    return ready;
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
