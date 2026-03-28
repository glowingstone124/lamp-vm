#include <lamp/libsys.h>

static uint32_t ustrlen(const char *s) {
    uint32_t n = 0u;
    if (!s) {
        return 0u;
    }
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            static const char sp[] = " ";
            (void)libsys_write(1, sp, 1u);
        }
        (void)libsys_write(1, argv[i], ustrlen(argv[i]));
    }
    (void)libsys_write(1, "\n", 1u);
    return 0;
}
