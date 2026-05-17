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

static void fs_stat_fill_dev(fs_stat_t *st, uint32_t rdev) {
    if (!st) {
        return;
    }
    st->st_dev = FS_BACKEND_NONE;
    st->st_ino = rdev;
    st->st_mode = SYS_S_IFCHR | 0666u;
    st->st_nlink = 1u;
    st->st_uid = 0u;
    st->st_gid = 0u;
    st->st_rdev = rdev;
    st->st_size = 0u;
    st->st_blksize = 4096u;
    st->st_blocks = 0u;
}

static int fs_stat_dev(const char *path, fs_stat_t *st) {
    if (!st) {
        return FS_ERR_INVAL;
    }
    if (str_eq(path, "/dev/null")) {
        fs_stat_fill_dev(st, SCHED_FD_TYPE_DEV_NULL);
        return 0;
    }
    if (str_eq(path, "/dev/zero")) {
        fs_stat_fill_dev(st, SCHED_FD_TYPE_DEV_ZERO);
        return 0;
    }
    if (str_eq(path, "/dev/tty")) {
        fs_stat_fill_dev(st, SCHED_FD_TYPE_DEV_TTY);
        return 0;
    }
    return FS_ERR_NOENT;
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

int fs_stat(const char *path, fs_stat_t *st) {
    if (!path || path[0] == '\0' || !st) {
        return FS_ERR_INVAL;
    }
    if (path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v' && path[4] == '/') {
        return fs_stat_dev(path, st);
    }
    return fs_ext4_stat(path, st);
}

int fs_fstat(int32_t fd, fs_stat_t *st) {
    uint32_t t = SCHED_FD_TYPE_NONE;
    if (!st) {
        return FS_ERR_INVAL;
    }
    if (sched_fd_get_type(fd, &t) != SCHED_FD_OK) {
        return FS_ERR_BADF;
    }
    if (t == SCHED_FD_TYPE_REGULAR) {
        uint32_t backend = 0u;
        uint32_t file_id = 0u;
        uint32_t file_size = 0u;
        uint32_t is_dir = 0u;
        if (sched_fd_regular_get(fd, &backend, &file_id, &file_size, 0, &is_dir) != SCHED_FD_OK) {
            return FS_ERR_BADF;
        }
        st->st_dev = backend;
        st->st_ino = file_id;
        st->st_mode = (is_dir ? SYS_S_IFDIR : SYS_S_IFREG) | 0644u;
        st->st_nlink = 1u;
        st->st_uid = 0u;
        st->st_gid = 0u;
        st->st_rdev = 0u;
        st->st_size = file_size;
        st->st_blksize = 4096u;
        st->st_blocks = (file_size + 511u) / 512u;
        return 0;
    }
    if (t == SCHED_FD_TYPE_SOCKET) {
        st->st_dev = FS_BACKEND_NONE;
        st->st_ino = (uint32_t)fd;
        st->st_mode = SYS_S_IFSOCK | 0666u;
        st->st_nlink = 1u;
        st->st_uid = 0u;
        st->st_gid = 0u;
        st->st_rdev = 0u;
        st->st_size = 0u;
        st->st_blksize = 4096u;
        st->st_blocks = 0u;
        return 0;
    }
    if (t == SCHED_FD_TYPE_PIPE_READ || t == SCHED_FD_TYPE_PIPE_WRITE) {
        st->st_dev = FS_BACKEND_NONE;
        st->st_ino = (uint32_t)fd;
        st->st_mode = SYS_S_IFIFO | 0600u;
        st->st_nlink = 1u;
        st->st_uid = 0u;
        st->st_gid = 0u;
        st->st_rdev = 0u;
        st->st_size = 0u;
        st->st_blksize = 4096u;
        st->st_blocks = 0u;
        return 0;
    }
    if (t == SCHED_FD_TYPE_STDIN || t == SCHED_FD_TYPE_STDOUT || t == SCHED_FD_TYPE_STDERR ||
        t == SCHED_FD_TYPE_DEV_NULL || t == SCHED_FD_TYPE_DEV_ZERO || t == SCHED_FD_TYPE_DEV_TTY) {
        fs_stat_fill_dev(st, t);
        return 0;
    }
    return FS_ERR_BADF;
}

int fs_getdents(int32_t fd, fs_dirent_t *dst, uint32_t len) {
    uint32_t t = SCHED_FD_TYPE_NONE;
    uint32_t backend = 0u;
    uint32_t is_dir = 0u;
    if (!dst && len != 0u) {
        return FS_ERR_INVAL;
    }
    if (sched_fd_get_type(fd, &t) != SCHED_FD_OK) {
        return FS_ERR_BADF;
    }
    if (t != SCHED_FD_TYPE_REGULAR) {
        return FS_ERR_NOTDIR;
    }
    if (sched_fd_regular_get(fd, &backend, 0, 0, 0, &is_dir) != SCHED_FD_OK) {
        return FS_ERR_BADF;
    }
    if (is_dir == 0u) {
        return FS_ERR_NOTDIR;
    }
    if (backend == FS_BACKEND_EXT4) {
        return fs_ext4_getdents_fd(fd, dst, len);
    }
    return FS_ERR_BADF;
}

int fs_readlink(const char *path, uint8_t *dst, uint32_t len) {
    if (!path || path[0] == '\0' || !dst || len == 0u) {
        return FS_ERR_INVAL;
    }
    if (path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v' && path[4] == '/') {
        return FS_ERR_INVAL;
    }
    return fs_ext4_readlink(path, dst, len);
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
