# Execution engines

Lamp VM keeps the original interpreter as its reference backend and exposes
three runtime-selectable engines. This document describes their source layout,
the measured case for decoded direct threading, and the constraints for a
later native JIT.

## Current engines and source layout

- `classic` in `src/engines/classic.c` performs the complete per-instruction
  executable-memory fetch path and then invokes the shared instruction
  semantics.
- `cached` in `src/engines/cached.c` caches translated host addresses while
  validating instruction bytes, MMU epochs, and MMIO epochs.
- `threaded` in `src/engines/threaded.c` builds conservative 16-operation
  decoded blocks and executes them with computed goto on Clang/GCC, with a
  portable switch fallback. Unsupported instructions use `cached`.
- `src/engines/interpreter_core.c` contains the single authoritative
  implementation of complete ISA semantics used by `classic`, `cached`, and
  threaded fallback.
- `src/engines/engine.c` is the only runtime selector. `src/vm.c` owns VM
  lifecycle, vCPU scheduling, pacing, and device polling, but no engine
  implementation.

This boundary also gives future `jit_arm64.c` and `jit_x86_64.c` backends a
stable dispatch API without mixing native code generation into the VM loop.

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

## Shared decoded-block front end

The threaded engine already consumes an immutable decoded block. A future JIT
will consume the same decoded-operation representation. The current block
cache is keyed and guarded by:

- virtual start PC;
- a per-core MMU epoch;
- the translated physical start address;
- an MMIO-layout epoch;
- the raw instruction bytes, revalidated before every block entry.

Blocks stop after 16 instructions, at a page boundary, at stores and supported
control flow, or before an instruction that requires the complete interpreter.
Each decoded operation stores the opcode, register indices, immediate, and
guest PC. Calls, returns, interrupts, faults, HALT, and MMIO currently use
shared-interpreter fallback.

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
- Stop, interrupt, pacing, and debugger conditions are checked at block exits
  and at a bounded interval inside long straight-line blocks. With a 16-op
  bound, interrupt latency stays below a microsecond at the current execution
  rates.
- Complex operations call the existing CPU-aware memory, interrupt, and device
  helpers first. They can be specialized only after profiling proves it useful.

Keep the computed-goto variant only if an A/B test against the decoded switch
executor shows a repeatable gain. The current raw switch is already a compiler
generated jump table.

## Native JIT

The JIT should lower the same decoded operations in two tiers:

1. Tier 0 emits ALU, flags, and direct control flow, while calling C helpers for
   memory, MMIO, atomics, faults, and uncommon instructions.
2. A later tier may inline the MMU-TLB and plain-RAM fast paths with guarded
   exits to the helpers.

An arm64 emitter covers Apple Silicon and Linux arm64. An x86-64 emitter can be
added independently. Pulling LLVM into the runtime is not required for the
first tier and would add a large build/distribution dependency.

Code memory must obey W^X:

- macOS arm64 uses `MAP_JIT` and the platform JIT write-protection API;
- Linux emits into writable pages and changes them to RX with `mprotect`;
- instruction caches are synchronized after emission;
- pages are never intentionally left writable and executable together.

Every compiled block has explicit exit reasons: fallthrough, branch, indirect
control flow, interrupt/debug request, MMU/code invalidation, fault, and halt.
Architectural PC and flags are committed before an exit that can call C.

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
4. Next: add an arm64 Tier-0 JIT behind an explicit experimental engine.
5. Required for every new backend: run all selftests and differential
   instruction/state tests, then boot the full kernel on macOS and Linux.

No engine becomes the default until it passes the interpreter's conformance
suite and shows a stable real-guest improvement, not just a dispatch-only
microbenchmark.
