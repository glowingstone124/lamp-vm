# Execution engines

Lamp VM keeps the original interpreter as its reference backend and exposes
four runtime-selectable engines. This document describes their source layout,
the decoded direct-threading baseline, and the experimental native JIT.

## Current engines and source layout

- `classic` in `src/engines/classic.c` performs the complete per-instruction
  executable-memory fetch path and then invokes the shared instruction
  semantics.
- `cached` in `src/engines/cached.c` caches translated host addresses while
  validating instruction bytes, MMU epochs, and MMIO epochs.
- `threaded` in `src/engines/threaded.c` builds conservative 16-operation
  decoded blocks and executes them with computed goto on Clang/GCC, with a
  portable switch fallback. Unsupported instructions use `cached`.
- `jit` in `src/engines/jit/jit.c` validates and caches compiled blocks. The
  current ARM64 Tier-0 backend emits native code for common integer, memory,
  flags, and direct-control-flow operations; unsupported instructions resume
  through `cached` at the same guest PC.
- `src/engines/interpreter_core.c` contains the single authoritative
  implementation of complete ISA semantics used by `classic`, `cached`, and
  threaded fallback.
- `src/engines/engine.c` is the only runtime selector. `src/vm.c` owns VM
  lifecycle, vCPU scheduling, pacing, and device polling, but no engine
  implementation.

JIT target selection lives in `src/engines/jit/codegen.c`. Its platform-neutral
contract is implemented by separate `arm64/` and `x86_64/` codegen and memory
directories, without mixing native emission into the VM loop. The x86-64
target is currently an explicit unavailable stub, so `--engine jit` safely
uses `cached` there until lowering is implemented.

## Current baseline

The Release build uses `-O3`, frame-pointer omission, and LTO. The shared
interpreter uses a dense opcode switch that Clang lowers to a jump table. LTO
can inline the small engine wrappers and shared helpers across their separate
translation units.

Consequently, replacing the switch with computed goto while retaining one
fetch/decode per instruction is unlikely to be a large improvement by itself.
The more valuable optimization is to cache a short decoded translation block
and execute several decoded operations before returning to the scheduler.

Use `vm benchmark` for repeatable single-core measurements. It separates flat
ALU/control-flow, MMU fetch, RAM load/store, and RAM+MMU workloads. Real guest
boot sampling remains mandatory because an idle loop, MMIO-heavy workload, and
the synthetic loops have very different profiles.

### Interpreting full-system MIPS

`vm benchmark` deliberately sets the virtual clock limit to zero and runs a
small loop made entirely from hot engine operations. A normal BIOS launch is
different in three important ways:

- the default `--cpu-mhz 100` option caps each vCPU at 100 million guest
  instructions per second;
- firmware, the kernel, filesystems, syscalls, interrupts, and device MMIO use
  complex operations such as calls, returns, stack traffic, atomics, and
  indirect control flow that still leave the Tier-0 JIT through `cached`;
- the debugger MIPS field is the recent *retired instruction rate*, not a
  benchmark score. It falls while the guest is idle or blocked for an event.

The guest kernel build therefore defaults to `-O2`; its `user_exec.c`
translation unit temporarily remains at `-O0` because its optimized
Lamp-target build currently fails the `/bin/sh` ELF-loader path. Device
completion must also be event-driven: the disk worker publishes status under
the disk mutex and raises its interrupt directly, so faster engines cannot
outrun a periodic host poll or observe an interrupt before the matching
completion status.

## Shared decoded-block contract

The threaded and JIT engines consume the same immutable decoded-operation
representation. Their block caches are keyed and guarded by:

- virtual start PC;
- a per-core MMU epoch;
- the translated physical start address;
- an MMIO-layout epoch;
- the raw instruction bytes, revalidated before every block entry.

Blocks stop after 16 instructions, at a page boundary, at stores and supported
control flow, or before an instruction that requires the complete interpreter.
Loads may remain inside a block; their RAM fast path retains a guarded exit for
MMIO, faults, and page-boundary cases. Each decoded operation stores the opcode,
register indices, immediate, and guest PC. Calls, returns, interrupts, faults,
HALT, and uncommon operations currently use shared-interpreter fallback.

This removes repeated instruction translation and decoding without changing
the architectural state representation. The existing interpreter remains the
reference implementation and fallback.

## Direct-threaded engine

The useful form here is a direct-threaded *decoded-block* executor, not a label
table wrapped around the existing one-instruction function.

- Clang/GCC builds may use labels-as-values so each handler has its own indirect
  branch site.
- A portable switch-based decoded-block executor must remain available; the
  project intentionally builds as strict C11 and supports both macOS and Linux.
- The executor batches adjacent decoded blocks into a bounded 256-instruction
  quantum. Stop and interrupt conditions are checked every eight operations
  inside a block and between blocks; pacing remains owned by the VM scheduler.
- Complex operations call the existing CPU-aware memory, interrupt, and device
  helpers first. They can be specialized only after profiling proves it useful.

Keep the computed-goto variant only if an A/B test against the decoded switch
executor shows a repeatable gain. The current raw switch is already a compiler
generated jump table.

## Native JIT scratch

The JIT is intentionally opt-in with `--engine jit` and keeps the other three
engines intact. Its two-tier shape is:

1. Tier 0 emits ALU, flags, and direct control flow, while retaining C slow
   exits for memory, MMIO, atomics, faults, and uncommon instructions.
2. Target-specific memory entry points specialize identity-mapped RAM and MMU
   TLB hits, then fall back to the complete CPU-aware helpers on a guard miss.

The first ARM64 scratch backend is now executable on Apple Silicon and is also
structured for Linux arm64. It directly lowers `MOV`, `MOVI`, integer
add/subtract/multiply, immediate arithmetic, compare, byte/word loads and
stores, ZF branches, fences, pause, and CPUID. Arithmetic and logic flags are
derived directly from ARM64 NZCV without a C call. Backward direct branches to
an instruction in the same compiled block become native branches with a
256-instruction budget, so tight guest loops do not repeatedly return through
the C dispatcher. Loads may continue within a compiled block; stores remain
block exits so code writes and device effects retain a precise boundary.

Compiled code uses a per-vCPU reusable arena with 1,024 four-way
set-associative slots. Eviction overwrites a slot in place instead of performing
one `mmap`/`munmap` pair per basic block. This matters little to a one-block
microbenchmark but avoids severe compile/evict thrashing across the kernel and
BusyBox working set.

`codegen.h` is the target-independent dispatch contract. Concrete guest-memory
entry points live in `arm64/memory_arm64.c` and
`x86_64/memory_x86_64.c`. The ARM64 implementation handles identity-mapped RAM
and valid MMU-TLB hits directly, after checking the MMIO-page bitmap and
framebuffer range; all misses use the existing CPU-aware memory helpers. The
abstract emitter never embeds a permanent RAM or MMIO host pointer. Pulling
LLVM into the runtime is not required for this tier.

Code memory must obey W^X:

- macOS arm64 uses `MAP_JIT` and `pthread_jit_write_protect_np`;
- Linux emits into writable pages and changes them to RX with `mprotect`;
- instruction caches are synchronized after emission;
- pages are never intentionally left writable and executable together.

Architectural `last_ip` and `ip` are committed before every lowered operation.
Compiled entries execute no more than the native-loop budget plus one block,
then return to the VM scheduler; indirect control flow, interrupts, HALT,
atomics, and uncommon operations currently use interpreter fallback.

## Invalidation and SMP correctness

Before enabling native JIT code, raw-byte validation should be upgraded to
write-generation invalidation:

- RAM writes must increment a physical-page generation while a block engine is
  active. This includes CPU stores, atomics, disk/network/audio DMA, and debug
  writes.
- A block must record the generation of every code page it spans and validate it
  before entry.
- MMU root/control changes and explicit TLB invalidation must increment a per-core
  MMU epoch and invalidate that core's block lookup entries.
- Generation and epoch publication must be atomic so another vCPU cannot continue
  running stale code after a shared executable page changes.
- Dynamic PCI BAR relocation is never embedded as a permanent host pointer;
  MMIO paths retain guarded helper exits.

The guest kernel already separates kernel text as RX and data as RW. Its ELF
loader temporarily makes a segment writable while copying and then restores
the final ELF permissions. Before enabling a JIT, those page-table changes must
also issue an explicit guest TLB/code invalidation; relying on identity mapping
alone is not sufficient.

## Implementation status and gates

1. Complete: retain `classic` and the Release benchmark as correctness and
   performance baselines.
2. Complete: translated fetch caching, MMU epochs, decoded blocks, and portable
   switch execution.
3. Complete: computed-goto decoded execution behind `--engine threaded`.
4. Complete scratch: ARM64 Tier-0 machine-code emitter behind explicit
   `--engine jit`, W^X code memory, per-vCPU compiled block caches, raw-byte and
   epoch invalidation, platform memory seams, and cached fallback.
5. Complete optimization pass: bounded multi-block quanta, native ARM64
   backedges, inline NZCV flag lowering, guarded ARM64 TLB/plain-RAM paths, and
   a reusable four-way per-vCPU code arena.
6. Next: add physical page-generation invalidation before permitting larger
   memory-spanning blocks, and use real-guest profiles to choose the next ISA
   operations or trace shape to lower.
7. Required for every new backend: run all selftests and differential
   instruction/state tests, then boot the full kernel on macOS and Linux.

No engine becomes the default until it passes the interpreter's conformance
suite and shows a stable real-guest improvement, not just a dispatch-only
microbenchmark.
