# POSIX Compatibility Notes

This document tracks the POSIX-like behavior currently implemented by the VM kernel.
It is focused on observable ABI and runtime semantics, not internal design.

## Scope

Implemented now:

- syscall ABI mailbox return path
- process/task basics: `getpid`, `yield`, `sleep_ticks`, `exit`, `waitpid`
- basic time syscalls: `nanosleep`, `clock_getres`, `clock_gettime`, `clock_settime`, `gettimeofday`
- fd syscalls: `read`, `write`, `close`, `dup`, `dup2`, `fcntl`
- readiness syscalls: `poll`, `select`
- tty mode syscalls: `tty_getmode`, `tty_setmode`
- filesystem/network surface:
  - `open` for `/dev/null`, `/dev/zero`, `/dev/tty`
  - `open/read/write` for ext4 regular files (absolute path)
  - `socket/connect/bind/listen/accept/send/recv` with stable stub errno behavior

Not implemented yet:

- ext4 create/truncate/unlink/rename and directory mutation
- sparse write and deep extent-tree growth (`depth > 0`) in write path
- real socket transport/protocol stack
- process groups/sessions/signals beyond tty line discipline behavior

## Syscall ABI

- interrupt vector: `IRQ_SYSCALL = 0x80`
- input registers at trap entry: `r0=nr`, `r1..r6=arg0..arg5`
- return path: fixed syscall mailbox at `SYSCALL_ABI_ADDR (0x002FE000)`
- mailbox carries: `ret`, `errno`, last syscall nr/args, current tick snapshot

Note:

- VM currently restores caller registers on `IRET`, so register return is not used yet.

## FD Model

Current fd types:

- stdio: `stdin(0)`, `stdout(1)`, `stderr(2)`
- special dev fds from `open`: `/dev/null`, `/dev/zero`, `/dev/tty`
- regular file fds from ext4 `open(path, ...)`
- socket fds from `socket()`

`fcntl` support:

- `F_GETFD`, `F_SETFD` (`FD_CLOEXEC`)
- `F_GETFL`, `F_SETFL` (`O_NONBLOCK` only for status toggling)

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
- opening directory as file: `-1/EISDIR`
- `O_CREAT`/`O_TRUNC`: `-1/EROFS` (not implemented yet)
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

Coverage currently includes:

- `dup/close/fcntl/read/poll/select` baseline
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
