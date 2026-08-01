# lamp-vm

A small 32-bit VM with SMP support, MMIO devices, interrupts, and a custom ISA.

## Current Status

- VM boots and runs custom binaries (`--bin`).
- SMP core bring-up works (`STARTAP`, `CPUID`, `IPI`).
- PCIe root complex (ECAM, BAR sizing/relocation, capability list, MSI onto INTC) is
  implemented; see `docs/pci.md`. The guest enumerates bus 0 and drives the
  Ethernet, XRGB8888 display, and 48 kHz PCM endpoints through assigned BARs,
  IOMMU DMA where applicable, and MSI.
- Built-in selftests pass in current tree:
  - `startap_cpuid`
  - `ipi`
  - `mmu_percpu_root`
  - `iommu_paged_translation`
  - `pcie_enumeration`
  - ISA/device conformance checks for relative control flow, zero-flag branches,
    `CALLR`, atomics, divide-by-zero interrupts, 16-bit sign extension, indexed
    read/write widths, extended relative conditions, `INTI`, PS/2, and framebuffer
    acceleration, PCI GPU damage/page-flip, PCI audio DMA completion, runtime
    statistics ABI, and virtual CPU pacing
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

Networking bring-up and BusyBox `wget` notes are in `docs/networking.md`.
Firmware/PCI graphics takeover is described in `docs/graphics.md`; the shared
DMA-ring and SDL3 PCI audio path is described in `docs/audio.md`.
The classic, translated-fetch, and decoded direct-threaded engines, plus the
native JIT roadmap, are documented in
`docs/execution-engine.md`.

## Build

### Requirements

- CMake >= 3.20
- C11 compiler
- SDL3
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

The executable supports subcommands for the common workflows:

```bash
./build/vm run bios/boot.bin --cores 1
./build/vm test
./build/vm help
```

Arguments:
- `run [program.bin]`: boot a guest program image (default: `boot.bin`)
- `test`: run built-in SMP tests and exit
- `help`: show usage and examples
- `--cores <n>` / `--smp <n>`: CPU worker thread count in `[1, 64]` (default: `1`)
- `--cpu-mhz <n>`: per-vCPU execution cap in `[1, 10000]` MHz (default: `100`)
- `--engine <name>`: select `classic`, `cached`, or `threaded`
- `--console` / `--serial-window`: show guest serial in a dedicated `VM Serial`
  SDL window (enabled by default). After early boot, the kernel turns `VM Display`
  into a small graphical desktop with composited windows; shell input, echo,
  logs, and TTY output remain in `VM Serial`.
- `--no-serial-window`: keep the framebuffer SDL window, but route guest serial
  output to the host terminal
- `--headless`: legacy headless serial stdin mode
- `--net <mode>`: ethernet backend: `null`, `nat`, or `udp:<bind-port>:<peer-port>`

Legacy flag-style invocation is still accepted:

```bash
./build/vm --bin bios/boot.bin --smp 1
./build/vm --selftest
```

The serial window accepts normal text input, Ctrl-letter control characters,
arrow/home/end/delete escape sequences, and clipboard paste with Ctrl/Cmd+V.
Once the graphical screen is active, VM Display's PS/2 keyboard path is reserved
for a future GUI event API and does not feed the shell. Its PS/2 mouse drives the
guest-controlled PCI cursor plane on a high-priority input path; click VM
Display to capture the host pointer and use Control+Command+G on macOS (or
Ctrl+Alt+G elsewhere) to release it. Early-boot diagnostics still use the
framebuffer, and a kernel panic restores framebuffer text output.
Closing only `VM Serial` leaves the VM running and falls back to serial output
on the host terminal; closing `VM Display` stops the VM.

Normal graphical runs do not print host lifecycle logs. Set `LAMP_VM_LOG=1` to
show startup, device-registration, and VNC messages; errors remain visible.

The serial renderer uses SDL3 and the bundled monospace glyph set on both Linux
and macOS, so it does not depend on a desktop font package or a platform-native
text API. The terminal model, ANSI handling, input routing, theme, and
high-DPI/resizable UI follow the same rendering path across platforms.
Its single metrics line separates the configured CPU MHz from measured aggregate
interpreter MIPS and also shows guest RAM, host resident memory, core count, and
uptime; the rest of the window is the terminal grid without decorative chrome.

Run selftests:

```bash
./build/vm test
```

## Virtual CPU Clock Model

- The configured MHz is a virtual oscillator frequency, not a benchmark of the
  host interpreter.
- Clock model v1 charges one virtual cycle for every retired guest instruction.
  Each vCPU is paced against a monotonic host deadline with a roughly 250 us
  budget interval; a host scheduling stall does not create a long catch-up burst.
- The invariant virtual cycle counter continues with monotonic time. If the host
  cannot sustain the configured rate, retired instructions and measured MIPS lag
  behind that counter, representing vCPU stalls.
- This is deterministic instruction-cost accounting, not a cycle-accurate model
  of a physical pipeline, cache, or branch predictor.

SYSINFO layout v3 exposes the nominal frequency, invariant cycles, retired
instruction count, aggregate execution rate, uptime, and host RSS. The kernel
publishes these through `/proc/cpuinfo` and `/proc/lampvm`; scheduler and memory
summaries are available in `/proc/stat`, `/proc/uptime`, `/proc/loadavg`, and
`/proc/meminfo`.

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
| SYSINFO MMIO | `0x0074C000` | `0x0074C08F` | 144 B | firmware and runtime VM metadata |
| INTC MMIO | `0x0074D000` | `0x0074D503` | 1284 B | interrupt controller |
| IOMMU MMIO | `0x0074E000` | `0x0074E0FF` | 256 B | IOMMU capability/control |
| MMU MMIO | `0x0074F000` | `0x0074F0FF` | 256 B | paging control/fault registers |
| Ether MMIO | `0x00750000` | `0x0075001B` | 28 B | NIC registers |
| PCIe ECAM | `0x00900000` | `0x009FFFFF` | 1 MiB | PCI Express configuration space, see `docs/pci.md` |

Note: the native C stack used by BIOS and early kernel execution is rooted at
`0x00800000` and grows downward; it is not a fixed-size MMIO region so it does
not appear in this table, but any new fixed physical window must still avoid
it. See the "Why `0x00900000`?" section in `docs/pci.md` before adding new
fixed addresses here.

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
- `docs/isa.md`
- `docs/bios.md`
- `docs/kernel.md`
- `docs/posix.md`
- `docs/user-abi.md`
- `docs/bios-build.md`

## Kernel Developing

It's time to develop a kernel for this platform.

Recommended start order:

1. Bring up single-core kernel first (`--smp 1`): boot, trap/interrupt entry, timer tick, syscall ABI.
2. Add scheduler and memory management in single-core mode.
3. Enable SMP bring-up (`STARTAP`), then add spinlocks and per-core data.
4. Use atomic ISA ops (`LDAR/STLR/CAS/XADD`) for lock primitives.

Practical note:
- If you want fastest iteration, keep kernel bring-up on `--smp 1` until trap path and basic drivers are stable, then turn on SMP.
