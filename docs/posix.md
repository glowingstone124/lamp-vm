# POSIX Compatibility Notes

This document tracks the POSIX-like behavior currently implemented by the VM kernel.
It is focused on observable ABI and runtime semantics, not internal design.

## Scope

Implemented now:

- syscall ABI mailbox return path
- process/task basics: `getpid`, `getppid`, `yield`, `sleep_ticks`, `exit`, `waitpid`, `chdir`, `getcwd`
- basic time syscalls: `nanosleep`, `clock_getres`, `clock_gettime`, `clock_settime`, `gettimeofday`
- fd syscalls: `read`, `write`, `close`, `dup`, `dup2`, `fcntl`, `pipe`, `ioctl`, `lseek`, `stat`, `fstat`, `getdents`, `access`
- readiness syscalls: `poll`, `select`
- tty mode syscalls: `tty_getmode`, `tty_setmode`, `ioctl(TCGETS/TCSETS/TIOCGWINSZ)`
- filesystem/network surface:
  - `open` for `/dev/null`, `/dev/zero`, `/dev/tty`
  - `open/read/write` for ext4 regular files (absolute path)
  - `socket/connect/bind/listen/accept/send/recv` with stable stub errno behavior

Not implemented yet:

- ext4 create/unlink/rename and directory mutation
- sparse write and deep extent-tree growth (`depth > 0`) in write path
- real socket transport/protocol stack
- userspace signal handler delivery/trampolines
- process groups/sessions and job-control foreground groups

## Syscall ABI

- interrupt vector: `IRQ_SYSCALL = 0x80`
- input registers at trap entry: `r0=nr`, `r1..r6=arg0..arg5`, optional `r8=mailbox`
- return path: caller-provided syscall mailbox, with fixed `SYSCALL_ABI_ADDR (0x002FE000)` fallback
- mailbox carries: `ret`, `errno`, last syscall nr/args, current tick snapshot

Note:

- VM currently restores caller registers on `IRET`, so register return is not used yet.

## FD Model

Current fd types:

- stdio: `stdin(0)`, `stdout(1)`, `stderr(2)`
- special dev fds from `open`: `/dev/null`, `/dev/zero`, `/dev/tty`
- regular file fds from ext4 `open(path, ...)`
- pipe read/write endpoint fds from `pipe()`
- socket fds from `socket()`

`fcntl` support:

- `F_GETFD`, `F_SETFD` (`FD_CLOEXEC`)
- `F_GETFL`, `F_SETFL` (`O_NONBLOCK` only for status toggling)

`lseek` support:

- regular ext4 file fds support `SEEK_SET`, `SEEK_CUR`, `SEEK_END`
- duplicated fds share one open-file offset
- stdio/tty/socket/pipe fds return `-1/ESPIPE`

`pipe` support:

- `pipe(int pipefd[2])` returns read end then write end
- fixed in-kernel ring buffer with short read/write behavior
- empty nonblocking reads and full nonblocking writes return `-1/EAGAIN`
- closing all writers makes reads return EOF; closing all readers makes writes return `-1/EPIPE`
- `poll/select` observe readable data/EOF and writable capacity

`ioctl` support:

- tty fds (`stdin/stdout/stderr` and `/dev/tty`) support `TCGETS`, `TCSETS`, `TCSETSW`, `TCSETSF`, and `TIOCGWINSZ`
- `TCGETS/TCSETS*` expose the current termios ABI subset: `ISIG`, `ICANON`, `ECHO`, and 32 control-char slots
- `TIOCGWINSZ` returns the current fixed console geometry of 30 rows by 80 columns
- closed fds return `-1/EBADF`; non-tty fds return `-1/ENOTTY`

`stat/fstat` support:

- path stat covers ext4 regular files/directories and `/dev/null`, `/dev/zero`, `/dev/tty`
- fd stat covers regular files, stdio/tty/dev fds, and socket fds
- returned fields are the current 32-bit ABI subset: dev, ino, mode, nlink, uid, gid, rdev, size, blksize, blocks

`getdents` support:

- ext4 directory fds opened read-only can be enumerated
- each returned entry uses the fixed 32-bit `lamp_dirent` ABI record
- ordinary `read()` on a directory fd still returns `-1/EISDIR`

`access` support:

- `F_OK`, `R_OK`, `W_OK`, `X_OK` are validated against `stat` mode bits
- unsupported mode bits return `-1/EINVAL`
- missing paths preserve filesystem errno such as `-1/ENOENT`

cwd/path support:

- each task tracks an absolute cwd, inherited by `sched_spawn` and `vfork`
- `chdir` accepts absolute or relative paths and requires the target to be a directory
- `getcwd` returns the current absolute path or `-1/ERANGE` when the user buffer is too small
- path syscalls resolve relative names against cwd before reaching VFS: `open`, `stat`, `access`, `execve`, `chdir`

signal support:

- `sigaction(sig, act, oldact)` records per-task 32-bit dispositions and masks
- `sigprocmask(how, set, oldset)` supports `SIG_BLOCK`, `SIG_UNBLOCK`, and `SIG_SETMASK`; `SIGKILL/SIGSTOP` are never masked
- `kill(pid, sig)` supports concrete task pids/tids, signal `0` existence checks, ignored dispositions, and default terminate behavior
- caught userspace handlers are stored but not delivered yet
- process groups, sessions, and tty foreground-group delivery are not implemented yet

Access mode and readiness:

- access mode checks use `O_RDONLY/O_WRONLY/O_RDWR`
- `poll/select` read wait-on-console applies to `stdin` and `/dev/tty`
- `poll` ignores entries with `fd < 0` (POSIX-compatible behavior)

## read/write Semantics

`read`:

- short-read behavior is preserved
- first chunk may block (when blocking mode is active)
- after partial copy, follow-up chunks are polled nonblocking
- nonblocking empty read returns `-1/EAGAIN`

By fd type:

- `/dev/null` read: returns `0`
- `/dev/zero` read: fills user buffer with zero bytes
- regular file read: offset-based ext4 inode read, EOF returns `0`
- socket read (stub): `-1/ENOTCONN`

`write` by fd type:

- stdio and `/dev/tty`: console output path
- `/dev/null` and `/dev/zero`: accepted, returns requested length
- regular file write: supports in-extent overwrite and EOF append with inode size update
- socket write (stub): `-1/ENOTCONN`

## TTY Line Discipline

Default local flags:

- `ECHO | ICANON | ISIG`

Input normalization and control chars:

- `CR` is normalized to `LF`
- canonical erase supports `BS(0x08)` and `DEL(0x7F)`
- canonical kill supports `VKILL (^U, 0x15)` and clears current editable fragment
- canonical EOF supports `VEOF (^D, 0x04)`
- with `ISIG`, `VINTR (^C, 0x03)` clears current line fragment and terminates line

Canonical read visibility:

- readable when a full line (`LF`) exists, or queued EOF marker exists

## poll/select Behavior

- `poll`:
  - `arg0=pollfd*`, `arg1=nfds`, `arg2=timeout_ms`
  - supports `POLLIN`, `POLLOUT`, `POLLNVAL`
- `select`:
  - `arg0=nfds`, `arg1=read_mask*`, `arg2=write_mask*`, `arg3=except_mask*`, `arg4=timeout_ms`
- blocking `waitpid/poll/select` parks current task and resumes when ready/timeout/wakeup

## open and socket Surface

`open(path, flags)` current mapping:

- `/dev/null`
- `/dev/zero`
- `/dev/tty`
- absolute ext4 path (`/foo/bar`)

ext4 open notes:

- missing path: `-1/ENOENT`
- opening directory read-only: succeeds for `getdents`; ordinary `read` returns `-1/EISDIR`
- opening directory writable/truncated: `-1/EISDIR`
- `O_CREAT`: `-1/EROFS` (not implemented yet)
- `O_TRUNC` on writable open: supported (current implementation keeps extents allocated and sets inode size to 0)
- unsupported ext4 write shape: `-1/ENOSYS`

Socket syscall status:

- `socket(domain, type, protocol)` allocates socket fd for supported domains
- unsupported domain: `-1/EAFNOSUPPORT`
- non-socket fd passed to socket operations: `-1/ENOTSOCK`
- `connect/bind/listen/accept` on socket fd: `-1/EOPNOTSUPP` (stub)
- `send/recv` on unconnected socket fd: `-1/ENOTCONN` (stub)

## Regression

Init shell command:

- `fdtest`
- `upipe` after installing `/bin/cat` and `/bin/pipe_exec`

Coverage currently includes:

- `getpid/getppid` parent relationship
- `chdir/getcwd` and relative `open/stat/access`
- `pipe` read/write, nonblocking empty read, EOF, `EPIPE`, `poll`, and `fstat`
- `dup/close/fcntl/read/poll/select` baseline
- regular-file `lseek` and non-seekable fd `ESPIPE`
- `sigaction/sigprocmask/kill` minimum default/ignored signal paths
- `stat/fstat` file type and size behavior
- ext4 directory `open/getdents` behavior
- `access` existence/mode/error behavior
- `/dev/null`, `/dev/zero`, `/dev/tty` open and I/O behavior
- socket stub errno behavior
- SMP waitq/poll/select stress behavior (`fdtest`)

Typical boot test flow:

```bash
bash ./kernel/build.sh
./build/vm --bin bios/boot.bin --smp 1
```

Then in init shell:

```text
fdtest
poll
tty
```
