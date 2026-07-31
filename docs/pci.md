# PCIe Root Complex and Devices

This document describes the PCIe config-space emulation in `src/io_devices/pcie/`,
the virtual Ethernet, display, and audio endpoints, and the guest kernel
enumerator.

## Scope

Implemented now:

- ECAM (Enhanced Configuration Access Mechanism) MMIO window, bus 0 only
- Standard Type 0 (endpoint) configuration header emulation
- BAR size-probe and relocation, gated by `Command.MEM_ENABLE`
- Capability list: Power Management (minimal), MSI, PCI Express (minimal Endpoint subset)
- MSI delivery onto the existing 256-vector INTC, with legacy INTx fallback
- Host bridge at `00:00.0`
- Lamp Ethernet endpoint at `00:01.0`
- Lamp XRGB8888 dumb-display endpoint at `00:02.0`
- Lamp 48 kHz PCM playback endpoint at `00:03.0`
- Guest bus-0 enumeration, BAR probing/allocation, memory-space and bus-master enable
- Guest MSI setup and driver binding for Ethernet, display, and audio

Not implemented yet:

- Multiple buses / bridges (only bus 0 exists)
- MSI-X
- Real PCIe link training semantics beyond fixed "always trained" fields
- A generic class/vendor driver registry (binding is currently explicit)

## ECAM Layout

```
addr = PCIE_ECAM_BASE + (dev * 8 + func) * 4096 + offset
```

- `PCIE_ECAM_BASE = 0x00900000`, window size `0x100000` (1 MiB)
- `PCI_ECAM_DEV_COUNT = 32`, `PCI_ECAM_FUNC_COUNT = 8`, `PCI_ECAM_FUNC_SIZE = 4096`
- Only bus 0 is populated (`PCI_ECAM_BUS_COUNT = 1`); a multi-bus system would add
  additional 1 MiB windows in the future.
- Reads to an unpopulated `(dev, func)` slot return `0xFFFFFFFF` (the standard
  "no device" response). Writes to an unpopulated slot are silently dropped
  (mirroring a real master-abort, without panicking the VM).

### Why `0x00900000`?

This address was chosen only after cross-checking every fixed physical address
literal in the BIOS and kernel sources, not just the tables in `docs/*.md`. An
earlier attempt placed ECAM at `0x00760000` (right after the existing MMU MMIO
window) because that looked free in `docs/kernel.md`'s memory map. It broke BIOS
boot immediately: BIOS's native C stack is rooted at `0x00800000` and grows
downward (`bios/bios.c` `_start`: `movi r30, 8388608`), and this convention is
**not** documented in the memory-map tables. `0x00760000..0x00800000` is the
stack's active growth region across BIOS and early kernel execution (the kernel
inherits this same stack until its scheduler switches to per-task stacks). Once
ECAM claimed that range, ordinary stack spill/reload traffic was silently
rerouted into PCI config-space semantics (unpopulated-slot reads returning
`0xFFFFFFFF`), corrupting the stack and panicking BIOS on the very first boot.

Known reserved ranges that any *new* fixed MMIO/scratch window must avoid
(cross-check `kernel/include/kernel/platform.h`, `bios/bios.c`,
`kernel/src/sched_task.c`, and `kernel/src/iommu.c` directly -- these are not
fully reflected in documentation tables):

| Range | Purpose |
|---|---|
| `0x00000000`-`~0x00430000` | BIOS/kernel image, low stacks, ELF load buffer (`KERNEL_ELF_BUF=0x00300000`) |
| `~0x00750000`-`0x00800000` | Native C stack active growth region (rooted at `0x00800000`, grows down) |
| `0x01000000`-`0x01200000` | Kernel vfork snapshot scratch (`SCHED_VFORK_SNAPSHOT_BASE`) |
| `0x01000000` | IOMMU DMA IOVA base offset (`IOMMU_DMA_IOVA_BASE`, translation input, not a direct occupant) |
| `0x02000000`-`0x03000000` | Guest userspace process region (`USER_REGION_BASE/SIZE`) |
| `0x03B80000`-`0x04000000` | SMP per-task stack pool (top of RAM) |

`0x00900000` sits just above the native stack's high-water mark (>1 MiB of
margin) and well below the vfork snapshot area (>6 MiB of margin).

## Configuration Header (Type 0)

Standard fields are emulated at their spec-defined offsets: Vendor/Device ID,
Command/Status, Revision/Class Code, Header Type (multi-function bit is
computed from whether any function 1-7 is present in the same device slot),
BAR0-5, Subsystem Vendor/ID, Capabilities Pointer, and Interrupt Line/Pin.
Expansion ROM is not implemented (always reads `0`).

## BARs

- `pci_configure_bar()` declares a BAR's size (must be a power of two), 64-bit
  flag, and prefetchable flag, plus device callbacks (`bar_read32`/
  `bar_write32`/optional `bar_relocated`).
- Sizing uses the standard trick: writing all-1s to a BAR register and reading
  it back yields the size mask, because the low, size-width bits are hardwired
  to 0 and cannot be set by the guest. No separate "sizing mode" bookkeeping is
  needed -- the same mask-on-write logic serves both sizing probes and real
  base assignment.
- The BAR's decoded address window is only active while
  `Command.MEM_ENABLE` is set, matching real hardware behavior where BAR
  address decode is gated by the memory-space-enable bit. Toggling Command
  re-evaluates every BAR's decode state.
- Each BAR gets its own internal MMIO shadow window whose `start`/`end` are
  mutated in place whenever the guest reprograms the BAR base; the underlying
  `MMIO_Device*` pointer registered in `vm->mmio_devices[]` never changes.
  BAR reprogramming should happen during single-core early boot; concurrent
  reprogramming while other cores are actively issuing MMIO accesses is not
  guaranteed race-free (a caveat inherited from the flat `vm->mmio_devices[]`
  scan/cache design already used by every other device in this VM).

## Capability List

Capabilities are stored as raw bytes starting at offset `0x40`, linked via the
standard `(id, next)` byte pair. `pci_add_capability()` allocates space and
patches the previous capability's `next` pointer; `Status.CAP_LIST` is set
automatically once any capability exists.

Convenience constructors:

- `pci_add_pm_capability()`: minimal Power Management capability (PMC version 3,
  no PME support).
- `pci_add_msi_capability()`: 64-bit-capable MSI, single vector, no
  per-vector masking. Message Control's Enable bit (bit 0) is the only
  guest-writable control bit; Address Low/High and Data are fully read/write.
- `pci_add_express_capability()`: minimal Endpoint PCI Express Capability
  Structure (capability header + Device Cap/Control/Status + Link
  Cap/Control/Status, 20 bytes total, no optional Slot/Root registers). Link
  Capabilities/Status always report Gen1 x1 with Data Link Layer Active set,
  since there is no physical link to train.

## MSI Delivery Convention

This VM is not x86, so there is no architectural MSI address/data format to
inherit. The convention used here (documented so guest drivers have a single
source of truth):

- Message Address bits `[11:4]` select the destination core id.
- Message Data bits `[7:0]` select the INTC vector to raise on that core.

`pci_notify_irq()` checks the function's MSI Enable bit; if set, it decodes the
above and calls `trigger_interrupt_target()` directly. If MSI is disabled, it
falls back to the legacy INTx vector registered via `pci_set_irq_pin()` and
sets `Status.INTX`. The Ethernet endpoint clears that status when the guest
acknowledges its pending RX frame.

## Enumerated Functions

A minimal host bridge function is always registered at `00:00.0`
(`LAMP_PCI_VENDOR_ID=0x1DB7`, device `0x0001`, class `0x060000`). The vendor ID
is self-assigned and not PCI-SIG registered -- this VM never talks to real
hardware or unmodified real-world drivers, so it only needs to be internally
consistent.

The virtual Ethernet controller is registered at `00:01.0` with vendor/device
`1DB7:1000` and class `0x020000`. It exposes one 4 KiB, 32-bit, non-prefetchable
memory BAR with these registers:

| BAR0 offset | Register | Meaning |
|---:|---|---|
| `0x00` | `TX_LEN` | Write a nonzero frame length to start TX DMA |
| `0x04` | `TX_LO` | TX buffer IOVA |
| `0x08` | `RX_LEN` | Pending received frame length, or zero |
| `0x0C` | `RX_LO` | RX buffer IOVA; writing also acknowledges the previous frame |
| `0x10` | `STATUS` | Bit 0 link-up, bit 1 RX-ready |
| `0x14` | `MAC_LO` | MAC bytes 0-3 |
| `0x18` | `MAC_HI` | MAC bytes 4-5 |

The endpoint has PM, MSI, and PCI Express capabilities. RX notification uses
MSI after the guest enables it; otherwise it falls back to INTA routed to the
Ethernet interrupt vector. The old fixed `0x00750000` register window remains
available as a compatibility path for older guest images, but the current
kernel uses the assigned PCI BAR.

The dumb-display controller is registered at `00:02.0` with vendor/device
`1DB7:2000` and class `0x030000`. BAR0 is a 4 KiB control aperture and BAR1 is
a 4 MiB prefetchable VRAM aperture. It implements XRGB8888 damage flushes,
shadow page flips, sequence completion, MSI, a fixed hardware cursor plane, and
firmware-framebuffer restore. See `docs/graphics.md` for its register/scanout
semantics.

The PCM controller is registered at `00:03.0` with vendor/device `1DB7:3000`
and class `0x040100`. It exposes one 4 KiB control BAR, a reusable IOMMU-backed
descriptor/completion ring, and MSI completion notification. The fixed v1
format is 48 kHz signed 16-bit stereo. See `docs/audio.md` for the full ABI.

## Guest Enumeration

`kernel/src/pci.c` scans bus 0 during boot when the boot-info PCIE feature bit
is present. It detects multifunction devices, probes memory BAR sizes with the
standard all-ones write, and allocates them from
`0x00A00000..0x00FFFFFF`. A 4 KiB-page first-fit bitmap preserves alignment for
large BARs while allowing later small BARs to reuse alignment gaps. Decode is
disabled while probing; after assignment, the kernel enables memory-space
decode and bus mastering.

For the known Lamp endpoints, the enumerator records all assigned BARs and
walks each capability list to program one MSI targeting core 0:

| Function | Typical assigned BARs | Vector |
|---|---|---|
| Ethernet `00:01.0` | BAR0 `0x00A00000` | `IRQ_ETHER` (6) |
| Display `00:02.0` | BAR0 `0x00A01000`, BAR1 `0x00C00000` | `IRQ_GPU` (7) |
| Audio `00:03.0` | BAR0 `0x00A02000` | `IRQ_AUDIO` (8) |

Addresses are allocator results rather than device ABI constants. If PCIe or
an endpoint is absent, Ethernet retains its legacy fixed-MMIO path and the
display console retains the firmware framebuffer. Audio is simply unavailable.

## Follow-up Work

Multiple buses/bridges, MSI-X, a generic driver binding layer, and an EHCI
(USB 2.0) controller remain follow-up work.
