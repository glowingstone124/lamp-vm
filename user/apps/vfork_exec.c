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

static void putstr(const char *s) {
    (void)libsys_write(1, s, ustrlen(s));
}

static void puthex32(uint32_t v) {
    static const char *hex = "0123456789ABCDEF";
    char buf[11];
    uint32_t i;
    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0u; i < 8u; i++) {
        uint32_t shift = (7u - i) * 4u;
        buf[2u + i] = hex[(v >> shift) & 0xFu];
    }
    buf[10] = '\0';
    putstr(buf);
}

int main(int argc, char **argv, char **envp) {
    int32_t pid;
    int32_t status = 0;
    int32_t rc;
    const char *const echo_argv[] = {"/bin/echo", "vfork-exec-ok", 0};

    (void)argc;
    (void)argv;
    (void)envp;

    pid = libsys_vfork();
    if (pid < 0) {
        putstr("vfork failed\n");
        putstr("errno=");
        puthex32((uint32_t)libsys_errno);
        putstr("\n");
        return 1;
    }
    if (pid == 0) {
        rc = libsys_execve("/bin/echo", echo_argv, 0);
        (void)rc;
        putstr("execve failed in child\n");
        (void)libsys_exit(127);
        return 127;
    }

    for (;;) {
        rc = libsys_waitpid(pid, &status, 0u);
        if (rc == pid) {
            break;
        }
        if (rc < 0) {
            putstr("waitpid failed\n");
            putstr("errno=");
            puthex32((uint32_t)libsys_errno);
            putstr("\n");
            return 2;
        }
    }

    if (((uint32_t)status >> 8u) != 0u) {
        putstr("child exited non-zero\n");
        return 3;
    }
    putstr("vfork+execve+waitpid success\n");
    return 0;
}
