# lamp-vm

A small 32-bit VM with SMP support, MMIO devices, interrupts, and a custom ISA.

## Current Status

- VM boots and runs custom binaries (`--bin`).
- SMP core bring-up works (`STARTAP`, `CPUID`, `IPI`).
- Built-in selftests pass in current tree:
  - `startap_cpuid`
  - `ipi`
- Atomic ISA instructions are implemented with C11 atomic semantics on aligned 32-bit RAM words.

## Toolchain

Assembler/toolchain project:
- https://github.com/glowingstone124/lampvm-toolchain
- https://github.com/glowingstone124/llvm-project (Custom LLVM backend)

## Layout

- `src/`: host-side VM implementation, device model, debugger, loader, and selftests
- `kernel/`: guest kernel sources and build script
- `bios/`: guest BIOS/bootloader sources
- `user/`: guest userspace runtime and sample apps
- `docs/`: architecture and ABI notes

## Build

### Requirements

- CMake >= 3.20
- C11 compiler
- SDL2
- pthreads (via `Threads::Threads`)

### Commands

```bash
cmake -S . -B build
cmake --build build -j
```

Notes:
- On Apple Silicon, CMake is configured to build `arm64`.
- Linux links `libm` and enables `_POSIX_C_SOURCE=200809L`.

## Run

```bash
./build/vm --bin bios/boot.bin --smp 1
```

Arguments:
- `--bin <file>`: program binary path (default: `boot.bin`)
- `--smp <cores>`: CPU worker thread count in `[1, 64]` (default: `1`)
- `--selftest`: run built-in SMP tests and exit
- `--console`: attach the host terminal to the guest serial console while keeping SDL and VNC enabled
- `--serial-stdin`: legacy headless serial stdin mode

Run selftests:

```bash
./build/vm --selftest
```

## SMP Execution Model

- `CPU0` is BSP and starts immediately.
- `CPU1..N-1` are APs and stay parked until `STARTAP`.
- Each core has private architectural state:
  - `regs/ip/flags`
  - call/data/ISR stack pointers and interrupt context
- Cores share RAM/MMIO/IO/device model.

## Memory and Concurrency Semantics

### Non-atomic memory path

- Normal `LOAD/STORE/LOAD32/STORE32/...` go through the VM memory API.
- Shared VM state is serialized by a global VM lock.

### Atomic ISA path

Atomic instructions operate on aligned 32-bit RAM words using C11 atomics:

- `CAS`
- `XADD`
- `XCHG`
- `LDAR` (acquire load)
- `STLR` (release store)
- `FENCE` (SC fence)

Rules:
- Unaligned atomic addresses panic.
- Atomic ops on MMIO addresses panic.
- `atomic_thread_fence` is only used with atomic operations; normal memory is not implicitly upgraded to atomic by fences.

## Stack Layout

- Stacks are stored in VM RAM (not host-side arrays).
- `--smp 1`: legacy fixed bases are used.
- `--smp > 1`: per-core stack regions are allocated near top of RAM.
- VM checks image/stack pool overlap at startup.

## Program Binary Format

Single binary format:

1. 24-byte header (`u32` little-endian x6)
2. Text section (`u64` little-endian instructions)
3. Data section (raw bytes)

Header fields:

1. `TEXT_BASE`
2. `TEXT_SIZE` (bytes)
3. `DATA_BASE`
4. `DATA_SIZE` (bytes)
5. `BSS_BASE`
6. `BSS_SIZE` (bytes)

## Memory Map (Default)

| Region | Start | End | Size | Purpose |
|---|---|---|---|---|
| IVT | `0x000000` | `0x0007FF` | 2048 B | 256 vectors x 8B |
| CALL_STACK | `0x000800` | `0x000FFF` | 2048 B | Call stack |
| DATA_STACK | `0x001000` | `0x0017FF` | 2048 B | Data stack |
| ISR_STACK | `0x001800` | `0x001FFF` | 2048 B | Interrupt stack |
| TIME MMIO | `0x002000` | `0x00201B` | 28 B | time registers |
| PROGRAM | `0x00201C` | `FB_BASE-1` | variable | text/data/bss |
| FrameBuffer | `FB_BASE` | `FB_BASE+FB_SIZE-1` | variable | video buffer |

Additional fixed MMIO regions:

| Region | Start | End | Size | Purpose |
|---|---|---|---|---|
| Legacy FrameBuffer Alias | `0x00620000` | `0x0074BFFF` | 1228800 B | video buffer legacy mapping |
| SYSINFO MMIO | `0x0074C000` | `0x0074C05B` | 92 B | firmware-style VM metadata |

## Debug Build (Optional)

Enable debug features:

```bash
cmake -S . -B build -DVM_DEBUG=ON
cmake --build build -j
```

When enabled:
- instruction statistics
- alignment checks
- interactive step/breakpoint debugger

Runtime debug env vars:
- `VM_DEBUG_STEP=1` or `VM_STEP=1`
- `VM_DEBUG_PAUSE=1`
- `VM_BREAKPOINTS=0x201C,0x2024`

## ISA

See:
- `isa.md`
- `bios.md`
- `kernel.md`
- `docs/posix.md`
- `bios-build.md`

## Kernel Developing

It's time to develop a kernel for this platform.

Recommended start order:

1. Bring up single-core kernel first (`--smp 1`): boot, trap/interrupt entry, timer tick, syscall ABI.
2. Add scheduler and memory management in single-core mode.
3. Enable SMP bring-up (`STARTAP`), then add spinlocks and per-core data.
4. Use atomic ISA ops (`LDAR/STLR/CAS/XADD`) for lock primitives.

Practical note:
- If you want fastest iteration, keep kernel bring-up on `--smp 1` until trap path and basic drivers are stable, then turn on SMP.
