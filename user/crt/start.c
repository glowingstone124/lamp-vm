#include <lamp/libsys.h>

extern int main(int argc, char **argv, char **envp);

int
start_c(uint32_t sp);
__attribute__((noreturn)) void start_exit_trap(int code);

__asm__(
    ".text\n"
    ".globl _start\n"
    "_start:\n"
    "  mov r0, r30\n"
    "  rcall start_c\n"
    "  rcall start_exit_trap\n"
    "1:\n"
    "  jmp 1b\n"
);

int start_c(uint32_t sp) {
    int argc;
    char **argv;
    char **envp;
    int code;

    argc = *(volatile int32_t *)(uintptr_t)sp;
    argv = (char **)(uintptr_t)(sp + 4u);
    envp = argv + (uint32_t)argc + 1u;

    code = main(argc, argv, envp);
    (void)libsys_exit(code);
    return code;
}

__attribute__((noreturn)) void start_exit_trap(int code) {
    for (;;) {
        (void)libsys_exit(code);
    }
}
