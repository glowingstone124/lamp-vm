# BusyBox `sh` Porting Roadmap

Last updated: 2026-05-03

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
- terminal compatibility ioctl subset (`TCGETS/TCSETS/TIOCGWINSZ`) for `isatty`/termios probing

Major remaining pieces for a fuller BusyBox `sh` environment:

- no process groups, sessions, or foreground tty job control yet
- signals do not yet interrupt blocking syscalls and `SA_RESTART` is not active

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

Current implementation snapshot:

- syscall IDs wired: `execve=28`, `vfork=29`
- kernel path:
  - `execve` replaces current user image and applies `FD_CLOEXEC`
  - `vfork` child returns `0`, parent blocks until child `execve` succeeds or child exits
- init shell smoke command: `uvfork [count]` (loads `/bin/vfork_exec`)
- userspace M1 smoke binaries:
  - `/bin/echo`
  - `/bin/vfork_exec`
- disk install helpers:
  - `user/install_user_to_disk.sh` for single binary
  - `user/install_m1_to_disk.sh` for one-shot M1 set install

## M2 - Pipe and Redirection Foundations

Deliverables:

- `pipe()` syscall with blocking/nonblocking behavior
- ensure `dup2 + close + fcntl(O_NONBLOCK)` semantics match shell expectations

Kernel work:

- pipe object type in fd table with read/write ends
- wakeup correctness under SMP for producer/consumer contention

Exit criteria:

- userland test passes: `echo hi | cat`, multi-stage pipe chains, EOF propagation

Current implementation snapshot:

- syscall ID wired: `pipe=38`
- pipe fds use shared in-kernel ring buffers with read/write endpoints
- fd inheritance works through the existing `vfork` fd-table clone path, with pipe endpoint refcounts adjusted during clone/dup/close
- EOF and `EPIPE` are implemented for closed peer endpoints
- nonblocking empty reads/full writes return `EAGAIN`
- `poll/select` readiness covers readable data/EOF and writable capacity
- `fdtest` covers same-task pipe read/write, `poll`, `fstat`, nonblocking read, EOF, and `EPIPE`
- userspace smoke binaries are built:
  - `/bin/cat`
  - `/bin/pipe_exec` for `vfork + pipe + dup2 + execve("/bin/echo") + execve("/bin/cat")`
- init shell command `upipe [count]` runs `/bin/pipe_exec`

Validation:

- install user ELFs into `disk.img` with `PATH=/opt/homebrew/opt/e2fsprogs/sbin:$PATH bash user/install_m1_to_disk.sh`
- smoke via init commands: `uvfork`, `upipe`, `fdtest`

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

Current implementation snapshot:

- syscall IDs wired: `lseek=30`, `stat=32`, `fstat=33`, `getdents=34`, `access=35`, `chdir=36`, `getcwd=37`
- cwd is tracked per task and inherited by `sched_spawn`/`vfork`
- relative paths are resolved for `open/stat/access/execve/chdir`
- ext4 directory fds can be opened read-only and enumerated through `getdents`
- `fdtest` covers cwd changes, `getcwd` buffer sizing, relative `open/stat/access`, metadata, directory enumeration, and `lseek`
- userspace M3 smoke binaries are built:
  - `/bin/pwd`
  - `/bin/ls`
  - `/bin/cat` also accepts path arguments for ext4 regular-file reads
- init shell smoke commands:
  - `upwd`
  - `uls [path]`

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

Current implementation snapshot:

- syscall ID wired: `ioctl=39`
- tty fds support `TCGETS`, `TCSETS`, `TCSETSW`, `TCSETSF`, and `TIOCGWINSZ`
- termios local flags map to existing console `ISIG/ICANON/ECHO`
- non-tty fds return `ENOTTY`; closed fds return `EBADF`
- `fdtest` covers termios get/set, fixed winsize query, and errno paths
- syscall IDs wired: `sigaction=40`, `sigprocmask=41`, `kill=42`
- signal dispositions and masks are tracked per task; `vfork` children inherit dispositions/mask
- `kill(pid, sig)` implements existence checks, ignored signals, and default terminate behavior for concrete task pids
- caught handlers are delivered through a versioned user signal frame; libc's
  restorer invokes `sigreturn` to restore the interrupted context
- blocked signals remain pending and are delivered after unmasking
- blocking syscalls are not interrupted yet, and process groups/job control
  remain future work
- `fdtest` covers ignored self-signal, invalid uncatchable actions, mask block/unblock, missing pid, and child termination
- syscall IDs wired for the next POSIX filesystem surface: `umask=43`, `rename=44`, `unlink=45`, `mkdir=46`, `rmdir=47`, `link=48`, `symlink=49`, `readlink=50`
- `umask` is tracked per task and inherited by `sched_spawn`/`vfork`; it is applied to newly created directories
- ext4 supports creating directories, removing empty directories, and renaming regular files, including replacing a non-directory destination; `mkdir`, `rmdir`, and common `mv` workflows now work from BusyBox
- directory renames, hard links, and creating symlinks remain unsupported

## M5 - BusyBox Integration and Rootfs Layout

Deliverables:

- cross-build BusyBox static binary for this target
- rootfs layout on ext4:
  - `/bin/busybox`
  - `/bin/sh -> /bin/busybox`
  - `/etc/profile` and simple init script
- kernel userspace launcher runs `/bin/sh`

Current implementation snapshot:

- ext4 path lookup follows symlinks, including fast symlinks such as `/bin/sh -> /bin/busybox`
- `readlink` returns ext4 symlink targets without a trailing NUL
- install helper: `user/install_busybox_to_disk.sh --input <busybox-elf>`
- init shell command `ush` launches `/bin/sh`
- BusyBox `ps`, `free`, and `uptime` are enabled; `ps` reads live
  `/proc/<pid>/stat` task snapshots

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
- `sigaction`, `sigprocmask`, `kill` minimum syscall surface
- `ioctl(TCGETS/TCSETS/TIOCGWINSZ)`

Should-have next (coreutils expansion):

- `rename`, `unlink`, `mkdir`, `rmdir`
- `link`, `symlink`, `readlink`
- `umask`, `isatty/ioctl` polish beyond the current terminal subset

Later:

- full `fork` semantics (COW and page-fault driven)
- process groups/sessions/`tcsetpgrp` job control
- sockets/network stack
