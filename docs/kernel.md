# Kernel Scaffold

> This project may split into a separate repository in the future.

This document describes the current kernel scaffold and bring-up flow, aligned with `bios.md`.

POSIX-facing syscall/fd/tty semantics are documented in `docs/posix.md`.
BusyBox shell porting roadmap is documented in `docs/porting.md`.
Current userspace ABI contract is documented in `docs/user-abi.md`.
Networking/NAT bring-up notes are documented in `docs/networking.md`.

## Goal

Current kernel target in this repository:

1. stable BIOS handoff and trap/syscall ownership
2. SMP-aware scheduler with blocking wait queues
3. POSIX-like syscall/fd/tty/time behavior
4. MMIO interrupt-controller based IRQ control path
5. block I/O + ext4-backed regular-file read/write baseline
6. IOMMU MMIO capability discovery for DMA remap/isolation bring-up
7. MMU paging baseline for Unix-like process/runtime evolution

## Layout

- `include/kernel/platform.h`: platform constants (interrupt numbers, memory constants)
- `include/kernel/types.h`: private freestanding C11 integer/pointer type aliases
- `include/kernel/kernel.h`: kernel boot sequence API
- `include/kernel/console.h`: console ring-buffer and IO API
- `include/kernel/init_task.h`: built-in init task bootstrap
- `include/kernel/printk.h`: console print + leveled kernel log API
- `include/kernel/trap.h`: trap/IRQ entry and dispatch API
- `include/kernel/irq.h`: IRQ handlers API
- `include/kernel/sched.h`: scheduler API
- `include/kernel/syscall.h`: syscall numbers and dispatcher API
- `include/kernel/smp.h`: BSP/AP interfaces
- `include/kernel/vm_info.h`: BootInfo metadata from BIOS handoff
- `include/kernel/spinlock.h`: lock primitive API
- `include/kernel/blk.h`: synchronous block I/O API over disk MMIO
- `include/kernel/iommu.h`: IOMMU init and DMA address translation API
- `include/kernel/dma_ring.h`: reusable descriptor/completion ring producer API
- `include/kernel/gpu.h`: PCI display takeover, completion IRQ, and cursor-plane API
- `include/kernel/graphics.h`: graphical framebuffer ownership and state API
- `include/kernel/wm.h`: kernel window and PS/2 pointer API
- `include/kernel/audio.h`: fixed-format PCI PCM DMA API
- `include/kernel/pci.h`: PCI enumeration results for bound device drivers
- `include/kernel/mmu.h`: kernel paging setup API
- `include/kernel/user_exec.h`: temporary user ELF load/spawn API
- `include/kernel/fs.h`: VFS-like dispatcher API
- `include/kernel/fs_proc.h`: generated read-only procfs backend API
- `include/kernel/fs_ext4.h`: ext4 backend API
- `src/entry.c`: `kernel_entry` and top-level init sequence
- `src/console.c`: console core (`rx` ring buffer, wait queue, read/write path, no implicit rx echo)
- `src/init_task.c`: kernel init task (`init$` command loop and runtime controls)
- `src/trap.c`: trap dispatch + INTC MMIO irq control
- `src/irq.c`: IRQ handlers, including PS/2 keyboard and three-byte mouse decode
- `src/sched.c`: scheduler core (context switch, stack pool, per-CPU runqueue)
- `src/sched_task.c`: task lifecycle and wait queue operations
- `src/sched_fd.c`: per-task fd table and regular-file metadata handling
- `src/sched_internal.h`: private scheduler types, globals, and internal helpers shared by the split implementation
- `src/syscall.c`: syscall dispatcher and ABI mailbox publish
- `src/smp.c`: BSP/AP bootstrap and AP bring-up
- `src/vm_info.c`: BootInfo decode/log helper
- `src/spinlock.c`: CAS/LDAR/STLR spinlock implementation
- `src/blk.c`: blocking disk read/write wrapper
- `src/iommu.c`: IOMMU capability probe and shared DMA IOVA setup
- `src/dma_ring.c`: reusable DMA submission and completion ownership logic
- `src/pci.c`: bus-0 enumeration, BAR allocation, and MSI programming
- `src/gpu.c`: PCI framebuffer validation and firmware-console takeover
- `src/graphics.c`: graphical ownership shim that starts the window manager
- `src/wm.c`: double-buffered kernel compositor, window model, and cursor policy
- `src/audio.c`: 48 kHz S16 stereo DMA-ring playback driver
- `src/mmu.c`: early paging init and identity map bring-up
- `src/user_exec.c`: user ELF loader (`ext4`) + initial user stack builder + temporary launcher task entry
- `src/fs.c`: fs dispatch (`/dev/*` + procfs + ext4)
- `src/fs_proc.c`: generated runtime, scheduler, and memory status files
- `src/fs_ext4.c`: ext4 mount/lookup/read/write backend

## Boot Sequence

`kernel_entry` currently performs:

1. firmware console, MMU, and IOMMU init
2. trap/IRQ + syscall init
3. PCI enumeration and display/audio/Ethernet driver binding
4. graphical framebuffer takeover; terminal traffic remains on serial
5. SMP BSP init
6. scheduler and block/fs init, then init task spawn
7. AP startup and scheduler run loop

## Scheduler Status

Current `sched` supports full per-task context switching for POSIX-style blocking and SMP:

- fixed task table (`SCHED_MAX_TASKS`)
- per-CPU runqueue (`run_cpu` ownership)
- local pick + global balancing via steal path
- task states: `RUNNABLE/RUNNING/SLEEPING/BLOCKED/ZOMBIE`
- blocking primitives: `sched_sleep_ticks()` and wait-queue wakeup
- fd table per task, including regular-file metadata (`backend/inode/size/offset`)

Current notes:

- task switch saves/restores task-local runtime context (`r31` stack pointer + VM call/data stack state)
- blocking syscalls (`waitpid/poll/select` paths) can park the current task and resume at the original call site
- scheduler critical paths are protected with spinlocks

## Virtual Clock and procfs

SYSINFO layout v3 advertises `RUNTIME_STATS` and exposes the configured virtual
CPU frequency separately from measured interpreter throughput. Clock model v1
charges one cycle per retired guest instruction, caps each vCPU with a monotonic
deadline budget, and uses an invariant wall-time-derived cycle counter. It is a
stable virtual clock contract, not a model of physical pipeline/cache latency.

The read-only procfs backend currently provides:

- `/proc/cpuinfo`: per-core identity, nominal MHz, timer frequency, and cycle model
- `/proc/meminfo`: guest RAM and conservative kernel/task-stack availability estimates
- `/proc/uptime`: monotonic VM uptime and scheduler idle time
- `/proc/stat`: scheduler user/system/idle ticks and task counts
- `/proc/loadavg`: current runnable tasks per online CPU (not yet historical EMA)
- `/proc/version`: kernel/procfs version string
- `/proc/lampvm`: raw frequency, cycles, retired instructions, measured IPS,
  uptime, guest estimates, host RSS, and task count
- `/proc/<pid>/stat`: a live task snapshot compatible with BusyBox process
  scanners; each numeric task directory contains `stat`

`/proc/meminfo` is intentionally labeled as an estimate: the kernel does not yet
have a page allocator that can report exact free/active/cache page accounting.
Host RSS is observable separately in `/proc/lampvm` and in the Serial SDL metrics
line.

## Syscall ABI (Current)

- interrupt vector: `IRQ_SYSCALL = 0x80`
- input registers at trap entry: `r0=nr`, `r1..r6=arg0..arg5`, optional `r8=mailbox`
- initial syscalls: `getpid`, `yield`, `sleep_ticks`, `exit`, `waitpid`, `nanosleep`, `read`, `write`, `close`, `dup`, `dup2`, `fcntl`, `open`, `poll`, `select`, `tty_getmode`, `tty_setmode`, `clock_getres`, `clock_gettime`, `clock_settime`, `gettimeofday`, `socket/connect/bind/listen/accept/send/recv`
- return publishing: caller-provided mailbox, with fixed `SYSCALL_ABI_ADDR (0x002FE000)` fallback

Note:

- VM currently restores caller registers on `IRET`, so direct register return is not yet available.
- The dispatcher writes `ret/errno` and the last call snapshot into the selected syscall mailbox.

## Interrupt Controller (Current)

- interrupt control is unified through INTC MMIO registers (`PENDING/ENABLE/PRIORITY/EOI`)
- kernel APIs: `irq_enable`, `irq_disable`, `irq_set_priority`, `irq_eoi`
- `trap_init()` resets and reprograms IRQ routing/priorities after BIOS handoff
- disk completion IRQ wakes block waiters via `blk_irq_complete()`

## IOMMU (Current VM v2)

- VM exposes IOMMU MMIO at `0x0074E000` and advertises `IOMMU_MMIO` in BootInfo/SYSINFO features.
- Disk, Ethernet, and PCI audio DMA paths go through the VM-side IOMMU translation API.
- Default compatibility behavior:
  - global IOMMU disabled => DMA address is identity-mapped (legacy behavior)
  - enabled but device entry disabled => identity-mapped
  - enabled + device entry enabled => `iova_base/iova_size/pa_base` window translation is enforced
- Device entries can also enable `DEV_CTRL_PAGED`, which switches translation to a 2-level 4KB IOVA page table.
- Kernel DMA programs paged IOMMU mode for the `0x01000000 + pa` IOVA window on disk, Ethernet, and audio device entries.
- Paged IOMMU PTEs now carry explicit DMA permission bits:
  - `IOMMU_PTE_P`: mapping is present
  - `IOMMU_PTE_R`: device may read guest memory through this mapping
  - `IOMMU_PTE_W`: device may write guest memory through this mapping
- Disk, Ethernet, and audio pass access intent into translation (`READ` for device reads
  from memory, `WRITE` for device writes into memory), and permission failures
  are reported with `IOMMU_FAULT_REASON_PERM`.
- Paged IOMMU translations currently require the requested DMA span to resolve to contiguous physical memory because the disk backend consumes one physical address per request.
- fault registers record last rejected translation (`dev/iova/len/reason`).

## MMU Paging (Current v2)

- VM exposes MMU MMIO at `0x0074F000` and advertises `MMU_PAGING` in BootInfo/SYSINFO features.
- MMU control/root/fault registers are now modeled per-CPU (SMP-safe address-space control surface).
- Kernel programs a 2-level 4KB paging structure and enables MMU on BSP and AP cores.
- Page-table updates are serialized and issue the MMU global TLB-epoch flush,
  so permission tightening (notably the ELF loader's temporary W+X to final
  RX/R transition) becomes visible to every vCPU before execution continues.
- Current mapping policy is identity map for kernel physical window (`0..KERNEL_MEM_SIZE + FB_SIZE`).
- Initial page permissions are hardened to Unix-like baseline:
  - `.text`: `RX`
  - `.rodata`: `R`
  - everything else (data/bss/stacks/heap/mmio aperture): `RW`
- Page-walk faults are latched into MMU fault registers for diagnostics.

## Logging (Current)

- unified log prefix format: `[LVL][tag] message`
- levels: `ERR`, `WRN`, `INF`, `DBG`
- default level is controlled by `KERNEL_LOG_LEVEL_DEFAULT` in `include/kernel/platform.h`
- panic path keeps forced error-level output and clears console with panic colors

## Console Behavior (Current)

- firmware and early-kernel output is mirrored to serial and framebuffer
- after graphical takeover, logs, stdout/stderr, input, and TTY echo use serial
  while the framebuffer remains a kernel-owned graphical desktop
- Kotlin VGA-window keyboard input enters through the PS/2 debugger API and
  does not feed the serial shell
- PS/2 mouse packets are coalesced in the priority-`0xD0` mouse IRQ and move the
  PCI GPU cursor plane without desktop recomposition; left click raises windows
- the Kotlin VGA window captures relative pointer motion after clicking it;
  Control+Command+G on macOS or Ctrl+Alt+G elsewhere releases it
- panic handling restores framebuffer text output
- default tty local mode: `ECHO|ICANON|ISIG`
- RX path normalizes `\r` to `\n`
- RX path handles backspace/delete (`0x08`/`0x7F`) by deleting the latest unread non-newline byte
- when `ISIG` is enabled, `^C` clears current input fragment and terminates line
- RX path counts complete lines and exposes dropped-byte stats for diagnostics
- input bytes are queued through tty line discipline (echo is controlled by tty mode bits)
- serial IRQ handler drains all pending RX bytes in one interrupt

## Read/Write Semantics (Current)

- `read(fd=0)` and `write(fd=1|2)` support larger user buffers via internal chunk loops
- `read` preserves short-read behavior:
  - blocks only for the first chunk when in blocking mode
  - once partial data is copied, subsequent chunks are polled nonblocking
- nonblocking mode is now driven by fd status flags (`fcntl(F_SETFL, O_NONBLOCK)`)
- nonblocking `read` returns `-1/EAGAIN` when no data is available

## Poll/Select/TTY (Current)

- `poll` ABI: `arg0=pollfd*`, `arg1=nfds`, `arg2=timeout_ms`
- `select` ABI: `arg0=nfds`, `arg1=read_mask*`, `arg2=write_mask*`, `arg3=except_mask*`, `arg4=timeout_ms`
- fd model (current): stdio + `/dev/*` + ext4 regular files + TCP socket fds
- `tty_getmode(fd)` and `tty_setmode(fd, lflag)` expose tty local mode bits
- `close/dup/dup2` and `fcntl(F_GETFD/F_SETFD/F_GETFL/F_SETFL)` are wired to per-task fd tables
- `open` maps `/dev/null`, `/dev/zero`, `/dev/tty` and absolute ext4 paths
- network syscalls support the current IPv4 client path; server-side socket behavior is still limited
- `fdtest` command in init shell runs fd regression checks (`dup`, `fcntl`, `read/poll/select`)
- `fdtest` uses `waitpid(child_pid, WNOHANG)` for post-reap `ECHILD` assertion to avoid ambient-child interference
- `poll` follows POSIX rule for ignored entries: `pollfd.fd < 0` yields `revents=0` and does not count as ready
- `waitpid(pid=0, ...)` is accepted and currently mapped to "any child" (process groups are not modeled yet)
- blocking `waitpid/poll/select` now park the current task and return only when ready/child-exit/timeout
- `clock_getres(clock_id, res*)` returns implementation resolution (`1ns`), `res==NULL` is allowed
- `clock_gettime(clock_id, ts*)` supports `CLOCK_REALTIME(0)`, `CLOCK_MONOTONIC(1)`, and boot time (`2`/`7`)
- `clock_settime(clock_id, ts*)` currently allows `CLOCK_REALTIME(0)` only; `CLOCK_MONOTONIC(1)` returns `EINVAL`
- `gettimeofday(tv*, tz*)` returns realtime and writes zeroed legacy timezone data when `tz` is non-null

## Filesystem and Block I/O (Current)

- `blk` layer provides synchronous sector read/write over disk MMIO command interface
- `fs_open` dispatch:
  - `/dev/*` -> scheduler special fds
  - `/proc/*` -> generated read-only procfs files
  - absolute non-`/dev` path -> ext4 backend
- ext4 backend currently supports:
  - superblock + group descriptor + inode table reads
  - directory traversal for absolute paths
  - regular-file `open/read`
  - regular-file creation with `O_CREAT`, including `O_CREAT|O_EXCL` returning `EEXIST` for existing paths
  - minimal `unlink` for regular files
  - regular-file `write` for:
    - existing extent overwrite
    - EOF append with block bitmap allocation + inode size update
- current ext4 write limitations:
  - `O_TRUNC` is supported by inode-size update (blocks are currently kept allocated)
  - `unlink` clears the directory entry/inode metadata but does not reclaim blocks/inodes yet
  - sparse/non-EOF hole write is not supported (`ENOSYS`)
  - deep extent tree growth (`depth > 0`) is not supported (`ENOSYS`)

## Contract With BIOS

BIOS behavior is fixed by `bios.md`:

- BIOS loads ELF PT_LOAD segments and jumps to `e_entry`.
- BIOS publishes BootInfo at fixed address before handoff.
- Kernel must reinitialize IVT/trap policy on entry.
- Kernel must not assume register state beyond control transfer.
- Kernel headers avoid libc integer headers and use `kernel/types.h`.
- Kernel build-time `KERNEL_MEM_SIZE` should match BIOS/VM memory-size contract for direct-mapped assumptions (`FB_BASE`, pointer-range checks).
- Kernel validates BootInfo `mem_bytes` at boot and warns on mismatch.

## Build To Load Pipeline

This is the practical end-to-end flow used by the current BIOS:

1. Build `kernel.elf`
2. Write `kernel.elf` into `disk.img` at `LBA 1`
3. Build BIOS `boot.bin`
4. Run the VM with BIOS as `--bin`

### 1) Build kernel ELF

Current recommended flags for backend bring-up:

- `-O0` (some files may still hit ISel issues at higher optimization)
- `-ffreestanding -fno-builtin -fno-stack-protector`

- This project needs a custom LLVM toolchain.
```bash
mkdir -p build-kernel

for f in kernel/src/*.c; do
  $LAMP_CLANG --target=lamp-unknown-unknown \
    -ffreestanding -fno-builtin -fno-stack-protector -O0 \
    -Ikernel/include -c "$f" -o "build-kernel/$(basename "$f" .c).o"
done

$LAMP_LD -T kernel/linker.ld -e kernel_entry build-kernel/*.o -o build-kernel/kernel.elf
```

### 2) Write kernel ELF to `disk.img` at LBA 1

BIOS reads kernel from `LBA 1` (offset `512` bytes).

```bash
test -f disk.img || truncate -s 512M disk.img
dd if=build-kernel/kernel.elf of=disk.img bs=512 seek=1 conv=notrunc
```

### 3) Build BIOS boot image

```bash
$LAMP_CLANG --target=lamp-unknown-unknown -c bios/bios.c -o bios/bios.o
$LAMP_LD -T bios/boot_flat.ld bios/bios.o -o bios/boot.bin
```

### 4) Boot VM

```bash
./build/lampvm --bin bios/boot.bin --smp 1
```
