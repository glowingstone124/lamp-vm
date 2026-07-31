# User ABI Contract (v0)

This document defines the temporary userspace ABI used by the current kernel launcher.

## Syscall calling convention

- Trap vector: `0x80` (`IRQ_SYSCALL`)
- Invoke: `INT`/`INTI` with syscall vector in interrupt argument
- Input registers on trap entry:
  - `r0 = nr`
  - `r1..r6 = arg0..arg5`
  - `r8 = optional syscall mailbox pointer`
- Return transport:
  - syscall return is published in the caller-provided mailbox when `r8` points at an aligned valid 48-byte range whose first word is `SYC0`
  - legacy fallback mailbox remains at `0x002FE000`
  - return value: `*(u32*)(mailbox + 0x24)`
  - errno: `*(u32*)(mailbox + 0x28)` (valid when return is `-1`)

Reason: current VM interrupt model restores caller registers on `IRET`, so register return ABI is not yet available.

## Syscall mailbox layout

The mailbox is a 48-byte (`12 * u32`) little-endian record. Userspace may pass
an alternate mailbox in `r8`; the kernel accepts it when the address is aligned,
inside guest memory, and the first word contains `SYC0`.

| Offset | Field | Meaning |
|---|---|---|
| `0x00` | `magic` | `0x30435953` (`SYC0`) |
| `0x04` | `version` | `1` |
| `0x08` | `last_nr` | syscall number just dispatched |
| `0x0C` | `arg0` | syscall argument snapshot |
| `0x10` | `arg1` | syscall argument snapshot |
| `0x14` | `arg2` | syscall argument snapshot |
| `0x18` | `arg3` | syscall argument snapshot |
| `0x1C` | `arg4` | syscall argument snapshot |
| `0x20` | `arg5` | syscall argument snapshot |
| `0x24` | `ret` | signed 32-bit return value |
| `0x28` | `errno` | Linux-compatible errno when `ret == -1` |
| `0x2C` | `tick` | scheduler tick snapshot |

## Syscall numbers

Current IDs (must match kernel `include/kernel/syscall.h`):

- `0 getpid`
- `1 yield`
- `2 sleep_ticks`
- `3 exit`
- `4 waitpid`
- `5 nanosleep`
- `6 read`
- `7 write`
- `8 poll`
- `9 select`
- `10 tty_getmode`
- `11 tty_setmode`
- `12 clock_gettime`
- `13 gettimeofday`
- `14 clock_getres`
- `15 clock_settime`
- `16 close`
- `17 dup`
- `18 dup2`
- `19 fcntl`
- `20 open`
- `21 socket`
- `22 connect`
- `23 bind`
- `24 listen`
- `25 accept`
- `26 send`
- `27 recv`
- `28 execve`
- `29 vfork`
- `30 lseek`
- `31 getppid`
- `32 stat`
- `33 fstat`
- `34 getdents`
- `35 access`
- `36 chdir`
- `37 getcwd`
- `38 pipe`
- `39 ioctl`
- `40 sigaction`
- `41 sigprocmask`
- `42 kill`
- `43 umask`
- `44 rename`
- `45 unlink`
- `46 mkdir`
- `47 rmdir`
- `48 link`
- `49 symlink`
- `50 readlink`
- `51 sigreturn` (libc restorer use only)

Current `vfork` note:

- returns `0` in child and `child_pid` in parent
- parent is blocked until child `execve` succeeds or child exits

## Common constants

Wait options:

- `WNOHANG = 0x00000001`
- `WUNTRACED = 0x00000002` is reserved in the ABI header but not currently accepted by `waitpid`

Poll events:

- `POLLIN = 0x0001`
- `POLLOUT = 0x0004`
- `POLLERR = 0x0008`
- `POLLNVAL = 0x0020`

Clock IDs:

- `CLOCK_REALTIME = 0`
- `CLOCK_MONOTONIC = 1`
- `CLOCK_BOOTTIME = 7`

Open flags:

- `O_RDONLY = 0x00000000`
- `O_WRONLY = 0x00000001`
- `O_RDWR = 0x00000002`
- `O_CREAT = 0x00000040`
- `O_EXCL = 0x00000080`
- `O_NOCTTY = 0x00000100`
- `O_TRUNC = 0x00000200`
- `O_APPEND = 0x00000400`
- `O_NONBLOCK = 0x00000800`
- `O_CLOEXEC = 0x00080000`

`fcntl` commands:

- `F_DUPFD = 0`
- `F_GETFD = 1`
- `F_SETFD = 2`
- `F_GETFL = 3`
- `F_SETFL = 4`
- `F_DUPFD_CLOEXEC = 5`
- `FD_CLOEXEC = 0x00000001`

Seek constants:

- `SEEK_SET = 0`
- `SEEK_CUR = 1`
- `SEEK_END = 2`

File type bits:

- `S_IFMT = 0xF000`
- `S_IFSOCK = 0xC000`
- `S_IFLNK = 0xA000`
- `S_IFREG = 0x8000`
- `S_IFDIR = 0x4000`
- `S_IFCHR = 0x2000`
- `S_IFIFO = 0x1000`

Directory entry types:

- `DT_UNKNOWN = 0`
- `DT_REG = 8`
- `DT_DIR = 4`
- `DT_CHR = 2`
- `DT_LNK = 10`
- `DT_SOCK = 12`

`access` modes:

- `F_OK = 0`
- `X_OK = 1`
- `W_OK = 2`
- `R_OK = 4`

TTY/ioctl constants:

- `TCGETS = 0x00005401`
- `TCSETS = 0x00005402`
- `TCSETSW = 0x00005403`
- `TCSETSF = 0x00005404`
- `TIOCGWINSZ = 0x00005413`
- termios local flags: `ISIG = 0x1`, `ICANON = 0x2`, `ECHO = 0x8`
- `NCCS = 32`

Signal constants:

- dispositions: `SIG_DFL = 0`, `SIG_IGN = 1`
- signal numbers: `SIGHUP = 1`, `SIGINT = 2`, `SIGQUIT = 3`, `SIGKILL = 9`, `SIGTERM = 15`, `SIGCHLD = 17`, `SIGSTOP = 19`
- mask operations: `SIG_BLOCK = 0`, `SIG_UNBLOCK = 1`, `SIG_SETMASK = 2`

## ABI structures

All fields are little-endian and naturally packed as 32-bit ABI records unless
noted otherwise.

```c
typedef struct lamp_stat {
    uint32_t st_dev;
    uint32_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t st_rdev;
    uint32_t st_size;
    uint32_t st_blksize;
    uint32_t st_blocks;
} lamp_stat_t;

typedef struct lamp_dirent {
    uint32_t d_ino;
    uint32_t d_off;
    uint32_t d_reclen;
    uint32_t d_type;
    char d_name[256];
} lamp_dirent_t;

typedef struct lamp_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_cc[32];
} lamp_termios_t;

typedef struct lamp_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} lamp_winsize_t;

typedef struct lamp_sigaction {
    uint32_t handler;
    uint32_t flags;
    uint32_t mask;
    uint32_t restorer;
} lamp_sigaction_t;
```

## Signal delivery

For a caught, unmasked signal, the kernel saves the interrupted user context in
a versioned `SIG0` frame below the current user stack pointer. It enters the
handler with `r0=signo`; `r1` contains the signal-frame address as a Lamp ABI
extension. The signal itself and `sa_mask` are blocked while the handler runs.

The kernel also pushes `sa_restorer` onto the VM's separate user call stack.
The libc `sigaction()` wrapper supplies `__lamp_signal_restorer` when callers do
not provide one. Returning normally from the handler reaches that restorer,
which invokes syscall 51 and restores registers, IP, flags, memory stack,
call/data stack state, and the previous signal mask. `sigreturn` rejects frames
that are not the current task's active top signal frame.

Current limitations: no `siginfo_t`, alternate signal stack, process-directed
group delivery, or `SA_RESTART` behavior; caught signals do not yet interrupt a
kernel syscall that is sleeping on a wait queue.

The userspace compatibility headers expose the same layouts under `user/include/lamp/abi.h`.

## Errno mapping

Kernel currently uses Linux-compatible errno numbers for implemented paths:

- `1 EPERM`
- `2 ENOENT`
- `3 ESRCH`
- `4 EINTR`
- `5 EIO`
- `9 EBADF`
- `10 ECHILD`
- `11 EAGAIN`
- `13 EACCES`
- `12 ENOMEM`
- `14 EFAULT`
- `16 EBUSY`
- `17 EEXIST`
- `20 ENOTDIR`
- `21 EISDIR`
- `22 EINVAL`
- `24 EMFILE`
- `25 ENOTTY`
- `28 ENOSPC`
- `29 ESPIPE`
- `30 EROFS`
- `32 EPIPE`
- `34 ERANGE`
- `36 ENAMETOOLONG`
- `38 ENOSYS`
- `40 ELOOP`
- `75 EOVERFLOW`
- `88 ENOTSOCK`
- `95 EOPNOTSUPP`
- `97 EAFNOSUPPORT`
- `107 ENOTCONN`

## Initial userspace stack layout

Kernel user ELF launcher sets initial SP to:

```
SP -> argc (u32)
      argv[0] (u32 ptr)
      ...
      argv[argc-1]
      NULL
      envp[0] (u32 ptr)
      ...
      envp[n-1]
      NULL
      auxv[0].a_type (u32)
      auxv[0].a_val  (u32)
      ...
      AT_NULL
      0
      strings...
```

Current `auxv-lite` keys:

- `AT_PAGESZ`
- `AT_ENTRY`
- `AT_PHDR`
- `AT_PHENT`
- `AT_PHNUM`
- `AT_NULL`

## Syscall surface notes

The detailed POSIX behavior is tracked in `docs/posix.md`. This section is the
short ABI-level index.

- `read/write` operate on stdio, `/dev/*`, pipes, ext4 regular files, and supported sockets.
- `open` accepts `/dev/null`, `/dev/zero`, `/dev/tty`, and ext4 paths resolved against the caller cwd.
- `stat/fstat/getdents/access/chdir/getcwd/lseek` use the 32-bit structures and constants above.
- `pipe` returns read end then write end in `int pipefd[2]`.
- `ioctl` currently supports tty termios and winsize requests listed above.
- `sigaction/sigprocmask/kill` support pending/masked signals and caught
  userspace handler delivery through the libc restorer and `sigreturn`.
- `execve` loads a user ELF from ext4; `argv` and `envp` are copied as NULL-terminated string-vector pointers.
