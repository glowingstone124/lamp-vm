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

int32_t libsys_getppid(void) {
    return libsys_call6(LAMP_SYS_GETPPID, 0u, 0u, 0u, 0u, 0u, 0u);
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

int32_t libsys_read(int32_t fd, void *buf, uint32_t len) {
    return libsys_call6(LAMP_SYS_READ, (uint32_t)fd, (uint32_t)(uintptr_t)buf, len, 0u, 0u, 0u);
}

int32_t libsys_write(int32_t fd, const void *buf, uint32_t len) {
    return libsys_call6(LAMP_SYS_WRITE, (uint32_t)fd, (uint32_t)(uintptr_t)buf, len, 0u, 0u, 0u);
}

int32_t libsys_close(int32_t fd) {
    return libsys_call6(LAMP_SYS_CLOSE, (uint32_t)fd, 0u, 0u, 0u, 0u, 0u);
}

int32_t libsys_dup2(int32_t oldfd, int32_t newfd) {
    return libsys_call6(LAMP_SYS_DUP2, (uint32_t)oldfd, (uint32_t)newfd, 0u, 0u, 0u, 0u);
}

int32_t libsys_lseek(int32_t fd, int32_t offset, uint32_t whence) {
    return libsys_call6(LAMP_SYS_LSEEK, (uint32_t)fd, (uint32_t)offset, whence, 0u, 0u, 0u);
}

int32_t libsys_stat(const char *path, lamp_stat_t *st) {
    return libsys_call6(LAMP_SYS_STAT, (uint32_t)(uintptr_t)path, (uint32_t)(uintptr_t)st, 0u, 0u, 0u, 0u);
}

int32_t libsys_fstat(int32_t fd, lamp_stat_t *st) {
    return libsys_call6(LAMP_SYS_FSTAT, (uint32_t)fd, (uint32_t)(uintptr_t)st, 0u, 0u, 0u, 0u);
}

int32_t libsys_getdents(int32_t fd, lamp_dirent_t *dirp, uint32_t len) {
    return libsys_call6(LAMP_SYS_GETDENTS, (uint32_t)fd, (uint32_t)(uintptr_t)dirp, len, 0u, 0u, 0u);
}

int32_t libsys_access(const char *path, uint32_t mode) {
    return libsys_call6(LAMP_SYS_ACCESS, (uint32_t)(uintptr_t)path, mode, 0u, 0u, 0u, 0u);
}

int32_t libsys_chdir(const char *path) {
    return libsys_call6(LAMP_SYS_CHDIR, (uint32_t)(uintptr_t)path, 0u, 0u, 0u, 0u, 0u);
}

int32_t libsys_getcwd(char *buf, uint32_t size) {
    return libsys_call6(LAMP_SYS_GETCWD, (uint32_t)(uintptr_t)buf, size, 0u, 0u, 0u, 0u);
}

int32_t libsys_pipe(int32_t pipefd[2]) {
    return libsys_call6(LAMP_SYS_PIPE, (uint32_t)(uintptr_t)pipefd, 0u, 0u, 0u, 0u, 0u);
}

int32_t libsys_ioctl(int32_t fd, uint32_t request, void *arg) {
    return libsys_call6(LAMP_SYS_IOCTL, (uint32_t)fd, request, (uint32_t)(uintptr_t)arg, 0u, 0u, 0u);
}

int32_t libsys_sigaction(uint32_t sig, const lamp_sigaction_t *act, lamp_sigaction_t *oldact) {
    return libsys_call6(LAMP_SYS_SIGACTION,
                        sig,
                        (uint32_t)(uintptr_t)act,
                        (uint32_t)(uintptr_t)oldact,
                        0u,
                        0u,
                        0u);
}

int32_t libsys_sigprocmask(uint32_t how, const uint32_t *set, uint32_t *oldset) {
    return libsys_call6(LAMP_SYS_SIGPROCMASK,
                        how,
                        (uint32_t)(uintptr_t)set,
                        (uint32_t)(uintptr_t)oldset,
                        0u,
                        0u,
                        0u);
}

int32_t libsys_kill(int32_t pid, uint32_t sig) {
    return libsys_call6(LAMP_SYS_KILL, (uint32_t)pid, sig, 0u, 0u, 0u, 0u);
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
