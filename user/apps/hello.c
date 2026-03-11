#include <lamp/libsys.h>

int main(int argc, char **argv, char **envp) {
    static const char msg[] = "hello from user hello\n";
    (void)argc;
    (void)argv;
    (void)envp;
    (void)libsys_write(1, msg, (uint32_t)(sizeof(msg) - 1u));
    return 0;
}
