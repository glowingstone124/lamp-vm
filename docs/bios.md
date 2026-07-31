# LampVM BIOS Specification (v2)

This document defines the BIOS/kernel handoff contract for current development.

## Scope

The BIOS is a minimal stage-0 loader. It only:

1. set early stack
2. install disk-complete ISR
3. read VM metadata from firmware MMIO and publish BootInfo block
4. read kernel ELF from disk
5. load PT_LOAD segments into RAM
6. jump to ELF `e_entry`

No scheduler, no memory manager, no AP startup logic in BIOS.

## Disk Boot Source

- Boot media: VM virtual disk (`disk.img`)
- Kernel image location: `LBA 1`
- Format: ELF32 little-endian, `ET_EXEC`
- Temporary read buffer: `0x00300000`
- Max ELF file size: `512 KiB`

## Interrupt Policy in BIOS

- BIOS uses interrupt vector `INT_DISK_COMPLETE (0x02)` for disk DMA completion.
- BIOS installs only this ISR for its own loading flow.
- BIOS does not initialize full kernel IVT policy; kernel owns IVT after entry.

## Entry State

At BIOS `_start`:

- `r30` (SP) is initialized to `0x00800000` (8 MiB) and grows downward.
- BIOS then calls `bios_main`.
- This native C stack is inherited by the kernel after the `entry()` jump and
  continues to be used (still growing down from `0x00800000`) until the
  scheduler switches to per-task stacks from the SMP stack pool. Any new
  fixed MMIO/scratch physical window must leave headroom below `0x00800000`
  for this stack -- see `docs/pci.md` for a worked example of this constraint.

At kernel entry jump (`e_entry`):

- Control transfer is a direct function jump (`entry()`).
- Registers are not sanitized beyond BIOS execution side effects.
- Kernel must reinitialize the execution environment it depends on.

## BootInfo Handoff

BIOS publishes a fixed BootInfo block at `0x002FF000` before jumping to kernel.

- `magic` = `0x3049424C` (`"LBI0"`)
- `version` = `2`
- `size` = `0x64`
- vendor (16 bytes)
- memory size bytes (`lo/hi`)
- disk size bytes (`lo/hi`)
- SMP core count
- SYSINFO layout version, architecture id, endianness, physical address bits
- page size and timer frequency
- feature bitmap
  - includes `INTC_MMIO`, `IOMMU_MMIO`, and `MMU_PAGING` capability bits when exposed by VM SYSINFO
- framebuffer geometry (`width/height/bpp/stride`)
- boot realtime timestamp (`lo/hi`)

These fields are sourced from SYSINFO MMIO (`0x0074C000`), a read-only firmware metadata region provided by the VM.
Kernel should treat `mem_bytes` in BootInfo as runtime ground truth for memory-size validation.

SYSINFO layout v3 extends the MMIO region with live runtime data. These fields
are not copied into the fixed BootInfo v2 block; firmware advertises them through
the `RUNTIME_STATS` feature bit and the kernel reads them directly:

| Offset | Field |
|---|---|
| `0x5C/0x60` | nominal CPU frequency, Hz (`lo/hi`) |
| `0x64/0x68` | invariant virtual cycles (`lo/hi`) |
| `0x6C/0x70` | retired guest instructions (`lo/hi`) |
| `0x74/0x78` | measured aggregate execution rate, instructions/s (`lo/hi`) |
| `0x7C/0x80` | VM uptime, ns (`lo/hi`) |
| `0x84/0x88` | host process resident memory, bytes (`lo/hi`) |
| `0x8C` | runtime-statistics ABI version (`1`) |

Reading the low word takes a coherent snapshot for the matching high-word read.
The v1 CPU model charges one virtual cycle per retired instruction and paces each
vCPU against a monotonic clock. Nominal MHz is therefore configuration, while
execution rate is observed interpreter throughput; a slow host may report less
than the configured rate.

## Memory Ownership

- BIOS code/data: kernel must treat as disposable after boot.
- ELF temporary buffer (`0x00300000..`) may be reused by kernel after early init.
- Kernel load destination uses ELF `p_paddr` if non-zero, otherwise `p_vaddr`.

## ELF Loader Rules

- Validate:
  - magic: `0x7F 'E' 'L' 'F'`
  - class: 32-bit
  - endianness: little-endian
  - type: `ET_EXEC`
- For each `PT_LOAD`:
  - copy `p_filesz` bytes from file offset to destination
  - zero `[p_filesz, p_memsz)` as BSS tail

## Failure Policy

BIOS is fail-stop:

- If validation/loading fails, execute `HALT`.
- No recovery path, no fallback boot target.

## Kernel Requirements (for v2 BIOS)

Kernel should do these first:

1. reset/initialize IVT with `init_ivt` policy
2. install its own ISR table
3. set up its own stack and runtime sections
4. enable timer/console/driver init

## Out of Scope (v2)

- multiboot-style memory map passing
- boot args / cmdline
- module loading
- secure boot / signature
- AP boot orchestration
