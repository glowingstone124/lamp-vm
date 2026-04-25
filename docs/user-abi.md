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

Current `vfork` note:

- returns `0` in child and `child_pid` in parent
- parent is blocked until child `execve` succeeds or child exits

## Errno mapping

Kernel currently uses Linux-compatible errno numbers for implemented paths:

- `2 ENOENT`
- `4 EINTR`
- `5 EIO`
- `9 EBADF`
- `10 ECHILD`
- `11 EAGAIN`
- `12 ENOMEM`
- `14 EFAULT`
- `16 EBUSY`
- `20 ENOTDIR`
- `21 EISDIR`
- `22 EINVAL`
- `24 EMFILE`
- `28 ENOSPC`
- `30 EROFS`
- `38 ENOSYS`
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
