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

static int st_is_dir(const lamp_stat_t *st) {
    return st && ((st->st_mode & LAMP_S_IFMT) == LAMP_S_IFDIR);
}

static int ls_one(const char *path) {
    lamp_stat_t st;
    int32_t fd;
    lamp_dirent_t dents[4];

    if (libsys_stat(path, &st) < 0) {
        (void)write_all(2, "ls: cannot stat ", 16u);
        (void)write_all(2, path, ustrlen(path));
        (void)write_all(2, "\n", 1u);
        return 1;
    }

    if (!st_is_dir(&st)) {
        (void)write_all(1, path, ustrlen(path));
        (void)write_all(1, "\n", 1u);
        return 0;
    }

    fd = libsys_open(path, LAMP_O_RDONLY);
    if (fd < 0) {
        (void)write_all(2, "ls: cannot open ", 16u);
        (void)write_all(2, path, ustrlen(path));
        (void)write_all(2, "\n", 1u);
        return 1;
    }

    for (;;) {
        int32_t n = libsys_getdents(fd, dents, (uint32_t)sizeof(dents));
        if (n < 0) {
            (void)write_all(2, "ls: getdents failed\n", 20u);
            (void)libsys_close(fd);
            return 1;
        }
        if (n == 0) {
            break;
        }
        uint32_t off = 0u;
        while (off + (uint32_t)sizeof(lamp_dirent_t) <= (uint32_t)n) {
            lamp_dirent_t *dent = (lamp_dirent_t *)((uintptr_t)dents + off);
            if (dent->d_name[0] != '\0') {
                (void)write_all(1, dent->d_name, ustrlen(dent->d_name));
                (void)write_all(1, "\n", 1u);
            }
            if (dent->d_reclen == 0u) {
                break;
            }
            off += dent->d_reclen;
        }
    }

    (void)libsys_close(fd);
    return 0;
}

int main(int argc, char **argv, char **envp) {
    int failed = 0;
    (void)envp;

    if (argc <= 1) {
        return ls_one(".");
    }

    for (int i = 1; i < argc; i++) {
        if (argc > 2) {
            if (i > 1) {
                (void)write_all(1, "\n", 1u);
            }
            (void)write_all(1, argv[i], ustrlen(argv[i]));
            (void)write_all(1, ":\n", 2u);
        }
        if (ls_one(argv[i]) != 0) {
            failed = 1;
        }
    }
    return failed ? 1 : 0;
}
