#ifndef LAMP_KERNEL_FS_H
#define LAMP_KERNEL_FS_H

#include "types.h"

enum {
    FS_ERR_NOENT = -2,
    FS_ERR_IO = -5,
    FS_ERR_BADF = -9,
    FS_ERR_BUSY = -16,
    FS_ERR_NOTDIR = -20,
    FS_ERR_ISDIR = -21,
    FS_ERR_INVAL = -22,
    FS_ERR_NOSPC = -28,
    FS_ERR_ROFS = -30,
    FS_ERR_NAMETOOLONG = -36,
    FS_ERR_NOSYS = -38,
    FS_ERR_LOOP = -40
};

enum {
    FS_BACKEND_NONE = 0u,
    FS_BACKEND_EXT4 = 1u
};

typedef struct fs_stat {
    uint32_t st_dev;
    uint32_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t st_rdev;
    uint32_t st_size;
    uint32_t st_blksize;
    uint32_t st_blocks;
} fs_stat_t;

typedef struct fs_dirent {
    uint32_t d_ino;
    uint32_t d_off;
    uint32_t d_reclen;
    uint32_t d_type;
    char d_name[256];
} fs_dirent_t;

void fs_init(void);
int fs_open(const char *path, uint32_t flags);
int fs_stat(const char *path, fs_stat_t *st);
int fs_fstat(int32_t fd, fs_stat_t *st);
int fs_getdents(int32_t fd, fs_dirent_t *dst, uint32_t len);
int fs_readlink(const char *path, uint8_t *dst, uint32_t len);
int fs_read(int32_t fd, uint8_t *dst, uint32_t len);
int fs_write(int32_t fd, const uint8_t *src, uint32_t len);

#endif
