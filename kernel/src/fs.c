#include "../include/kernel/blk.h"
#include "../include/kernel/fs.h"
#include "../include/kernel/fs_ext4.h"
#include "../include/kernel/sched.h"
#include "../include/kernel/syscall.h"

static inline uint32_t str_eq(const char *a, const char *b) {
    uint32_t i = 0u;
    if (!a || !b) {
        return 0u;
    }
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return 0u;
        }
        i++;
    }
    return (a[i] == '\0' && b[i] == '\0') ? 1u : 0u;
}

static int fs_open_dev(const char *path, uint32_t flags) {
    uint32_t special_type = 0u;
    uint32_t status_flags = flags & (SYS_O_ACCMODE | SYS_O_NONBLOCK);

    if (str_eq(path, "/dev/null")) {
        special_type = SCHED_FD_SPECIAL_DEV_NULL;
    } else if (str_eq(path, "/dev/zero")) {
        special_type = SCHED_FD_SPECIAL_DEV_ZERO;
    } else if (str_eq(path, "/dev/tty")) {
        special_type = SCHED_FD_SPECIAL_DEV_TTY;
    } else {
        return FS_ERR_NOENT;
    }
    return sched_fd_open_special(special_type, status_flags);
}

void fs_init(void) {
    blk_init();
    fs_ext4_init();
}

int fs_open(const char *path, uint32_t flags) {
    if (!path || path[0] == '\0') {
        return FS_ERR_INVAL;
    }
    if (path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v' && path[4] == '/') {
        return fs_open_dev(path, flags);
    }
    return fs_ext4_open(path, flags);
}

int fs_read(int32_t fd, uint8_t *dst, uint32_t len) {
    uint32_t t = SCHED_FD_TYPE_NONE;
    if (!dst || len == 0u) {
        return 0;
    }
    if (sched_fd_get_type(fd, &t) != SCHED_FD_OK) {
        return FS_ERR_BADF;
    }
    if (t == SCHED_FD_TYPE_REGULAR) {
        return fs_ext4_read_fd(fd, dst, len);
    }
    return FS_ERR_BADF;
}

int fs_write(int32_t fd, const uint8_t *src, uint32_t len) {
    uint32_t t = SCHED_FD_TYPE_NONE;
    if (!src || len == 0u) {
        return 0;
    }
    if (sched_fd_get_type(fd, &t) != SCHED_FD_OK) {
        return FS_ERR_BADF;
    }
    if (t == SCHED_FD_TYPE_REGULAR) {
        return fs_ext4_write_fd(fd, src, len);
    }
    return FS_ERR_BADF;
}
