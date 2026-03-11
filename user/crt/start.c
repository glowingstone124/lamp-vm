#include <lamp/libsys.h>

extern int main(int argc, char **argv, char **envp);

int _start(void) {
    uint32_t sp = 0u;
    int argc;
    char **argv;
    char **envp;
    int code;

    __asm__ volatile("mov %0, r30\n" : "=r"(sp));
    argc = *(volatile int32_t *)(uintptr_t)sp;
    argv = (char **)(uintptr_t)(sp + 4u);
    envp = argv + (uint32_t)argc + 1u;

    code = main(argc, argv, envp);
    (void)libsys_exit(code);
    return code;
}
