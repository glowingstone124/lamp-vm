#ifndef LAMP_KERNEL_FS_EXT4_H
#define LAMP_KERNEL_FS_EXT4_H

#include "fs.h"
#include "types.h"

void fs_ext4_init(void);
int fs_ext4_open(const char *path, uint32_t flags);
int fs_ext4_stat(const char *path, fs_stat_t *st);
int fs_ext4_getdents_fd(int32_t fd, fs_dirent_t *dst, uint32_t len);
int fs_ext4_readlink(const char *path, uint8_t *dst, uint32_t len);
int fs_ext4_read_fd(int32_t fd, uint8_t *dst, uint32_t len);
int fs_ext4_write_fd(int32_t fd, const uint8_t *src, uint32_t len);

#endif
