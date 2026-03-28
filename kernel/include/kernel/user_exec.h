#ifndef LAMP_KERNEL_USER_EXEC_H
#define LAMP_KERNEL_USER_EXEC_H

#include "types.h"

typedef struct {
    uint32_t entry;
    uint32_t stack_ptr;
    uint32_t phdr;
    uint32_t phent;
    uint32_t phnum;
} user_image_t;

int user_exec_load_elf_from_ext4(const char *path,
                                 const char *const argv[],
                                 const char *const envp[],
                                 user_image_t *out_img);
int user_exec_spawn_path(const char *path, const char *const argv[], const char *const envp[]);
int user_exec_execve_current(const char *path, const char *const argv[], const char *const envp[]);

#endif
