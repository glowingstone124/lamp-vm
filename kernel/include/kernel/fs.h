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
    FS_ERR_NOSYS = -38
};

enum {
    FS_BACKEND_NONE = 0u,
    FS_BACKEND_EXT4 = 1u
};

void fs_init(void);
int fs_open(const char *path, uint32_t flags);
int fs_read(int32_t fd, uint8_t *dst, uint32_t len);
int fs_write(int32_t fd, const uint8_t *src, uint32_t len);

#endif
