# BusyBox `sh` Porting Roadmap

Last updated: 2026-03-07

## Goal

Run an interactive BusyBox shell (`/bin/sh`) on this VM kernel with ext4 rootfs.

Definition of done (v1):

- boot to userland and launch BusyBox `sh` as pid 1 replacement (or init child)
- interactive commands work: `echo`, `cd`, `pwd`, `cat`, `ls`
- shell features work: redirection (`>`, `<`), pipe (`|`), command sequencing (`;`)
- scripts work: `sh /etc/init.d/rcS`
- SMP=1 and SMP=2 both pass shell smoke tests without hangs

## Current Baseline

Already available:

- scheduler + waitq + per-task fd table
- `read/write/open/close/dup/dup2/fcntl/poll/select/waitpid/exit`
- tty line discipline (`ECHO/ICANON/ISIG`) and ext4 file `open/read/write`

Major missing pieces for BusyBox `sh`:

- no user process image lifecycle (`execve`)
- no `fork/vfork` for shell child commands
- no `pipe` syscall
- no cwd/path metadata APIs (`chdir/getcwd/stat/getdents`)
- no signal syscalls (`sigaction/sigprocmask/kill`) for shell control flow
- no libc/runtime target for user programs yet

## Architecture Decisions

1. Target MMU-first userspace and keep process memory isolation as baseline.
2. Use static ELF32 user binaries only (no dynamic linker in v1).
3. Implement `vfork + execve` path first; defer COW-style `fork` until later.
4. Disable BusyBox features that require full job control at first bring-up.
5. Keep kernel threads and user tasks under one scheduler, but track task kind explicitly.

## Milestones

## M0 - User ABI and Runtime Bootstrap

Deliverables:

- define user ABI contract: syscall numbers, calling convention, errno mapping
- add minimal crt (`_start`) and syscall wrapper library (`libsys`)
- run a static user `hello` binary via a temporary kernel launcher

Kernel work:

- add a kernel API to load a user ELF image from ext4 into a user region
- define initial user stack layout (`argc/argv/envp/auxv-lite`)

Exit criteria:

- `hello` can call `write(1,...)` and `_exit(0)` from user mode contract

Current implementation snapshot:

- ABI contract doc: `docs/user-abi.md`
- userspace runtime scaffold:
  - `user/crt/start.c`
  - `user/lib/libsys.c`
  - `user/apps/hello.c`
- temporary kernel launcher command in init shell: `uhello`
- kernel user loader API:
  - `user_exec_load_elf_from_ext4(path, argv, envp, out_img)`
  - `user_exec_spawn_path(path, argv, envp)`

## M1 - Process Lifecycle for Shell Commands

Deliverables:

- `execve(path, argv, envp)`
- `vfork()` semantics on top of MMU-enabled process model
- `_exit()` and existing `waitpid()` integrated with user child lifecycle

Kernel work:

- task metadata extensions: pid/ppid, task kind (kernel vs user), exec state
- parent suspension/resume rules for `vfork`
- fd inheritance with `FD_CLOEXEC` handling during `execve`

Exit criteria:

- user test program can `vfork -> execve("/bin/echo") -> waitpid`

## M2 - Pipe and Redirection Foundations

Deliverables:

- `pipe()` syscall with blocking/nonblocking behavior
- ensure `dup2 + close + fcntl(O_NONBLOCK)` semantics match shell expectations

Kernel work:

- pipe object type in fd table with read/write ends
- wakeup correctness under SMP for producer/consumer contention

Exit criteria:

- userland test passes: `echo hi | cat`, multi-stage pipe chains, EOF propagation

## M3 - Filesystem APIs Needed by Shell/Core Applets

Deliverables:

- `chdir`, `getcwd`
- `stat`, `fstat` (minimal POSIX fields first)
- `getdents`/`getdents64` for `ls`
- `lseek` (at least `SEEK_SET/SEEK_CUR/SEEK_END`)

Kernel work:

- cwd tracking per task
- ext4 dir iteration API exposed through VFS layer
- inode metadata translation to userspace stat struct

Exit criteria:

- BusyBox applets `cd/pwd/ls/cat` work on ext4 tree

## M4 - Signals and TTY Minimum for Interactive `sh`

Deliverables:

- `sigaction`, `sigprocmask`, `kill` (minimum set)
- terminal query/set API compatible enough for BusyBox shell mode

Kernel work:

- per-task signal pending/mask state
- safe delivery points in scheduler/syscall return path
- map existing tty controls to user-visible interface (`ioctl`-style or dedicated syscalls)

Exit criteria:

- `Ctrl-C` interrupts foreground command without killing the shell

## M5 - BusyBox Integration and Rootfs Layout

Deliverables:

- cross-build BusyBox static binary for this target
- rootfs layout on ext4:
  - `/bin/busybox`
  - `/bin/sh -> /bin/busybox`
  - `/etc/profile` and simple init script
- kernel userspace launcher runs `/bin/sh`

Recommended initial BusyBox config:

- keep: `ash` or `hush`, `echo`, `cat`, `ls`, `pwd`, `test`
- disable initially: job control heavy features, networking applets, advanced mount tools

Exit criteria:

- boot reaches BusyBox prompt and accepts interactive commands

## M6 - Hardening and Regression

Deliverables:

- regression suite for syscall + shell behavior (SMP=1/2)
- stress tests for `pipe/poll/select/waitpid/signal` races
- failure-injection for ext4/disk I/O paths while shell is active

Exit criteria:

- repeatable 100+ boot-and-smoke runs with no deadlock/panic

## Syscall Priority Matrix

Must-have for BusyBox `sh` v1:

- `execve`, `vfork`, `_exit`, `waitpid`
- `pipe`, `dup2`, `close`, `fcntl`
- `read`, `write`, `open`
- `chdir`, `getcwd`
- `stat`, `fstat`, `getdents`, `lseek`
- `sigaction`, `sigprocmask`, `kill`

Should-have next (coreutils expansion):

- `rename`, `unlink`, `mkdir`, `rmdir`
- `link`, `symlink`, `readlink`
- `umask`, `access`, `isatty/ioctl` polish

Later:

- full `fork` semantics (COW and page-fault driven)
- process groups/sessions/`tcsetpgrp` job control
- sockets/network stack
