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

static int cat_fd(int32_t fd) {
    char buf[128];
    for (;;) {
        int32_t n = libsys_read(fd, buf, (uint32_t)sizeof(buf));
        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            return 0;
        }
        if (write_all(1, buf, (uint32_t)n) != 0) {
            return -1;
        }
    }
}

int main(int argc, char **argv, char **envp) {
    int failed = 0;
    (void)envp;

    if (argc <= 1) {
        return cat_fd(0) == 0 ? 0 : 1;
    }

    for (int i = 1; i < argc; i++) {
        int32_t fd = libsys_open(argv[i], LAMP_O_RDONLY);
        if (fd < 0) {
            (void)write_all(2, "cat: cannot open ", 17u);
            (void)write_all(2, argv[i], ustrlen(argv[i]));
            (void)write_all(2, "\n", 1u);
            failed = 1;
            continue;
        }
        if (cat_fd(fd) != 0) {
            (void)write_all(2, "cat: read error: ", 17u);
            (void)write_all(2, argv[i], ustrlen(argv[i]));
            (void)write_all(2, "\n", 1u);
            failed = 1;
        }
        (void)libsys_close(fd);
    }

    return failed ? 1 : 0;
}
