#include <lamp/libsys.h>

static int write_all(int32_t fd, const char *buf, uint32_t len) {
    uint32_t off = 0u;
    while (off < len) {
        int32_t w = libsys_write(fd, &buf[off], len - off);
        if (w <= 0) {
            return -1;
        }
        off += (uint32_t)w;
    }
    return 0;
}

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
    char cwd[128];
    (void)argc;
    (void)argv;
    (void)envp;

    if (libsys_getcwd(cwd, (uint32_t)sizeof(cwd)) < 0) {
        (void)write_all(2, "pwd: getcwd failed\n", 19u);
        return 1;
    }
    if (write_all(1, cwd, ustrlen(cwd)) != 0 || write_all(1, "\n", 1u) != 0) {
        return 1;
    }
    return 0;
}
