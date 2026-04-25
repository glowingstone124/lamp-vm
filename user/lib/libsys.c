#include <lamp/libsys.h>

int32_t libsys_errno;

int32_t libsys_call6(uint32_t nr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5) {
    volatile uint32_t abi[LAMP_SYSCALL_ABI_WORDS];
    uint32_t abi_addr = (uint32_t)(uintptr_t)&abi[0];
    uint32_t ret;
    abi[LAMP_SYSCALL_ABI_OFF_MAGIC / 4u] = LAMP_SYSCALL_ABI_MAGIC;
    abi[LAMP_SYSCALL_ABI_OFF_VERSION / 4u] = LAMP_SYSCALL_ABI_VERSION;
    __asm__ volatile(
        "mov r0, %0\n"
        "mov r1, %1\n"
        "mov r2, %2\n"
        "mov r3, %3\n"
        "mov r4, %4\n"
        "mov r5, %5\n"
        "mov r6, %6\n"
        "mov r7, %7\n"
        "mov r8, %8\n"
        "int r7\n"
        :
        : "r"(nr), "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(LAMP_IRQ_SYSCALL),
          "r"(abi_addr)
        : "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "memory");

    ret = abi[LAMP_SYSCALL_ABI_OFF_RET / 4u];
    if ((int32_t)ret == -1) {
        libsys_errno = (int32_t)abi[LAMP_SYSCALL_ABI_OFF_ERRNO / 4u];
    } else {
        libsys_errno = 0;
    }
    return (int32_t)ret;
}

int32_t libsys_getpid(void) {
    return libsys_call6(LAMP_SYS_GETPID, 0u, 0u, 0u, 0u, 0u, 0u);
}

int32_t libsys_waitpid(int32_t pid, int32_t *status, uint32_t options) {
    return libsys_call6(LAMP_SYS_WAITPID,
                        (uint32_t)pid,
                        (uint32_t)(uintptr_t)status,
                        options,
                        0u,
                        0u,
                        0u);
}

int32_t libsys_write(int32_t fd, const void *buf, uint32_t len) {
    return libsys_call6(LAMP_SYS_WRITE, (uint32_t)fd, (uint32_t)(uintptr_t)buf, len, 0u, 0u, 0u);
}

int32_t libsys_execve(const char *path, const char *const argv[], const char *const envp[]) {
    return libsys_call6(LAMP_SYS_EXECVE,
                        (uint32_t)(uintptr_t)path,
                        (uint32_t)(uintptr_t)argv,
                        (uint32_t)(uintptr_t)envp,
                        0u,
                        0u,
                        0u);
}

int32_t libsys_vfork(void) {
    return libsys_call6(LAMP_SYS_VFORK, 0u, 0u, 0u, 0u, 0u, 0u);
}

int32_t libsys_exit(int32_t code) {
    return libsys_call6(LAMP_SYS_EXIT, (uint32_t)code, 0u, 0u, 0u, 0u, 0u);
}
