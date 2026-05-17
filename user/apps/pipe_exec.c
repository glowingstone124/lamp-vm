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
    buf[0] = '0';
    buf[1] = 'x';
    for (uint32_t i = 0u; i < 8u; i++) {
        uint32_t shift = (7u - i) * 4u;
        buf[2u + i] = hex[(v >> shift) & 0xFu];
    }
    buf[10] = '\0';
    putstr(buf);
}

static int wait_ok(int32_t pid) {
    int32_t status = 0;
    for (;;) {
        int32_t rc = libsys_waitpid(pid, &status, 0u);
        if (rc == pid) {
            break;
        }
        if (rc < 0) {
            putstr("waitpid failed errno=");
            puthex32((uint32_t)libsys_errno);
            putstr("\n");
            return 0;
        }
    }
    if (((uint32_t)status >> 8u) != 0u) {
        putstr("child exited non-zero status=");
        puthex32((uint32_t)status);
        putstr("\n");
        return 0;
    }
    return 1;
}

int main(int argc, char **argv, char **envp) {
    int32_t pfd[2];
    int32_t cat_pid;
    int32_t pipe_read_fd;
    int32_t pipe_write_fd;
    const char msg[] = {
        'p', 'i', 'p', 'e', '-',
        's', 'm', 'o', 'k', 'e', '-',
        'o', 'k', '\n', '\0'
    };

    (void)argc;
    (void)argv;
    (void)envp;

    if (libsys_pipe(pfd) != 0) {
        putstr("pipe failed errno=");
        puthex32((uint32_t)libsys_errno);
        putstr("\n");
        return 1;
    }
    pipe_read_fd = pfd[0];
    pipe_write_fd = pfd[1];

    cat_pid = libsys_vfork();
    if (cat_pid < 0) {
        putstr("vfork cat failed errno=");
        puthex32((uint32_t)libsys_errno);
        putstr("\n");
        return 3;
    }
    if (cat_pid == 0) {
        (void)libsys_dup2(pipe_read_fd, 0);
        (void)libsys_close(pipe_read_fd);
        (void)libsys_close(pipe_write_fd);
        (void)libsys_execve("/bin/cat", 0, 0);
        (void)libsys_exit(127);
        return 127;
    }

    (void)libsys_close(pipe_read_fd);
    if (libsys_write(pipe_write_fd, msg, ustrlen(msg)) < 0) {
        putstr("pipe write failed errno=");
        puthex32((uint32_t)libsys_errno);
        putstr("\n");
        (void)libsys_close(pipe_write_fd);
        return 4;
    }
    (void)libsys_close(pipe_write_fd);

    if (!wait_ok(cat_pid)) {
        return 5;
    }
    putstr("pipe+vfork+dup2+execve success\n");
    return 0;
}
