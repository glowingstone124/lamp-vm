# LampVM ISA (Polaris) Specification

Polaris V1.0

Released at 2026/03/07

## 1. General

LampVM is a **register-based virtual machine** with **64-bit fixed-width instructions**
and **32-bit general-purpose registers**, designed for system-level and educational use.

Polaris is the name for LampVM's ISA.

This document contains information of Polaris.

- Instruction width: 64 bits
- Register count: 32 (`r0` – `r31`)
- Register width: 32 bits
- Address space: 32 bits, byte-addressed
- Byte order: Little Endian
- Execution model: SMP-capable (BSP/AP) with interrupt support
---

## 2. Instruction Format

All instructions use a unified 64-bit format:

```
63 56 55 48 47 40 39 32 31 0
+-------------+------------+------------+------------+-------------+
| opcode | rd | rs1 | rs2 | imm |
+-------------+------------+------------+------------+-------------+
```

### Instruction Fields

| Field  | Width  | Description                |
|--------|--------|----------------------------|
| opcode | 8 bit  | Operation code             |
| rd     | 8 bit  | Destination register       |
| rs1    | 8 bit  | Source register 1          |
| rs2    | 8 bit  | Source register 2          |
| imm    | 32 bit | Immediate field (instruction-defined interpretation) |

### Unused Field Rules (ABI-stable)

- **Encoding rule**: assemblers/encoders **must** write `0` to fields unused by an opcode.
- **Execution rule**: for all currently-defined opcodes, unused fields are **ignored** and must not change architectural behavior.
- **Tooling rule**: validators/disassemblers may warn on non-zero unused fields, but execution compatibility follows the execution rule above.
- **Extension rule**: future ISA revisions must not repurpose currently-unused fields of existing opcodes.

---

### Opcode Assignment Table (ABI-stable)

The opcode-to-mnemonic mapping below is part of the ISA ABI and must remain stable.

| Opcode | Mnemonic |
|--------|----------|
| `0x01` | `ADD` |
| `0x02` | `SUB` |
| `0x03` | `MUL` |
| `0x04` | `DIV` |
| `0x05` | `HALT` |
| `0x06` | `JMP` |
| `0x07` | `JZ` |
| `0x08` | `PUSH` |
| `0x09` | `POP` |
| `0x0A` | `CALL` |
| `0x0B` | `RET` |
| `0x0C` | `LOAD` |
| `0x0D` | `LOAD32` |
| `0x0E` | `LOADX32` |
| `0x0F` | `STORE` |
| `0x10` | `STORE32` |
| `0x11` | `STOREX32` |
| `0x12` | `CMP` |
| `0x13` | `CMPI` |
| `0x14` | `MOV` |
| `0x15` | `MOVI` |
| `0x16` | `MEMSET` |
| `0x17` | `MEMCPY` |
| `0x18` | `IN` |
| `0x19` | `OUT` |
| `0x1A` | `INT` |
| `0x1B` | `IRET` |
| `0x1C` | `MOD` |
| `0x1D` | `AND` |
| `0x1E` | `OR` |
| `0x1F` | `XOR` |
| `0x20` | `NOT` |
| `0x21` | `SHL` |
| `0x22` | `SHR` |
| `0x23` | `SAR` |
| `0x24` | `JNZ` |
| `0x25` | `JG` |
| `0x26` | `JGE` |
| `0x27` | `JL` |
| `0x28` | `JLE` |
| `0x29` | `JC` |
| `0x2A` | `JNC` |
| `0x2B` | `FADD` |
| `0x2C` | `FSUB` |
| `0x2D` | `FMUL` |
| `0x2E` | `FDIV` |
| `0x2F` | `FNEG` |
| `0x30` | `FABS` |
| `0x31` | `FSQRT` |
| `0x32` | `FCMP` |
| `0x33` | `ITOF` |
| `0x34` | `FTOI` |
| `0x35` | `FLOAD32` |
| `0x36` | `FSTORE32` |
| `0x37` | `INC` |
| `0x38` | `ADDI` |
| `0x39` | `SUBI` |
| `0x3A` | `ANDI` |
| `0x3B` | `ORI` |
| `0x3C` | `XORI` |
| `0x3D` | `SHLI` |
| `0x3E` | `SHRI` |
| `0x3F` | `CAS` |
| `0x40` | `XADD` |
| `0x41` | `XCHG` |
| `0x42` | `LDAR` |
| `0x43` | `STLR` |
| `0x44` | `FENCE` |
| `0x45` | `PAUSE` |
| `0x46` | `STARTAP` |
| `0x47` | `IPI` |
| `0x48` | `CPUID` |
| `0x49` | `CALLR` |
| `0x4A` | `RJMP` |
| `0x4B` | `RCALL` |
| `0x4C` | `RJZ` |
| `0x4D` | `RJNZ` |
| `0x4E` | `ROL` |
| `0x4F` | `ROR` |
| `0x50` | `ROLI` |
| `0x51` | `RORI` |
| `0x52` | `LOAD16` |
| `0x53` | `STORE16` |
| `0x54` | `LOADS8` |
| `0x55` | `LOADS16` |
| `0x56` | `RJG` |
| `0x57` | `RJGE` |
| `0x58` | `RJL` |
| `0x59` | `RJLE` |
| `0x5A` | `RJC` |
| `0x5B` | `RJNC` |
| `0x5C` | `INTI` |
| `0x5D` | `LOADX` |
| `0x5E` | `LOADX16` |
| `0x5F` | `STOREX` |
| `0x60` | `STOREX16` |

---

## 3. Register Convention

- `r0` – `r30`: General-purpose registers
- `r31`: **Interrupt number parameter register**

All registers may be freely used by regular instructions and ISRs.

When entering an interrupt service routine (ISR), the VM **automatically saves and restores
all registers**.

---

## 4. FLAGS Register

FLAGS is a 32-bit integer register containing arithmetic and comparison status.

### Defined Flags

| Flag | Name          | Meaning                            |
|------|---------------|------------------------------------|
| ZF   | Zero Flag     | **Result = 0 / Compare Equal → 1** |
| SF   | Sign Flag     | Signed result is negative → 1      |
| CF   | Carry Flag    | Unsigned carry / borrow            |
| OF   | Overflow Flag | Signed overflow                    |

### Core Rule (Hard ABI Rule)

> **ZF = 1 means “result is zero” or “comparison is equal”.**

This rule is fixed and applies to all current and future instructions.

### ZF Update Semantics

- Arithmetic / logic instructions:  
  Result = 0 → `ZF = 1`
- `CMP` / `CMPI`:  
  Operands equal → `ZF = 1`

---

## 5. FLAGS Update Rules

### Instructions that update **all flags** (ZF, SF, CF, OF)

- `ADD`
- `SUB`
- `CMP`
- `CMPI`
- `INC`
- `ADDI`
- `SUBI`
- `XADD`

### Instructions that update **ZF and SF only**, and **clear CF and OF**

- `MUL`
- `DIV` (when divisor ≠ 0)
- `MOD` (when divisor ≠ 0)
- `FADD`
- `FSUB`
- `FMUL`
- `FDIV`
- `FNEG`
- `FABS`
- `FSQRT`
- `ITOF`
- `FLOAD32`
- `AND`
- `OR`
- `XOR`
- `NOT`
- `SHL`
- `SHR`
- `SAR`
- `ROL`
- `ROR`
- `ANDI`
- `ORI`
- `XORI`
- `SHLI`
- `SHRI`
- `ROLI`
- `RORI`
- `MOV`
- `MOVI`
- `LOAD`
- `LOAD16`
- `LOAD32`
- `LOADS8`
- `LOADS16`
- `LOADX`
- `LOADX16`
- `LOADX32`
- `POP`
- `FTOI` (when input is finite and in-range)
- `XCHG`
- `LDAR`

### Instructions that do **not guarantee FLAGS state**

- `STORE`
- `STORE16`
- `STOREX`
- `STOREX16`
- `STORE32`
- `STOREX32`
- `FSTORE32`
- `CALL`
- `RCALL`
- `RET`
- `INT`
- `INTI`
- `IRET`
- `HALT`
- `MEMSET`
- `MEMCPY`
- `IN`
- `OUT`
- `STLR`
- `FENCE`
- `PAUSE`

Programs must not rely on FLAGS after these instructions.

---

## 6. Arithmetic and Logic Instructions

### ADD rd, rs1, rs2

``` 
rd = rs1 + rs2
```

Updates ZF, SF, CF, OF.

---

### SUB rd, rs1, rs2

``` 
rd = rs1 - rs2
```

Updates ZF, SF, CF, OF.

---

### MUL rd, rs1, rs2

```
rd = rs1 * rs2
```

Updates ZF, SF.  
Clears CF, OF.

---

### DIV rd, rs1, rs2

``` 
rd = rs1 / rs2
```

- If `rs2 == 0`: triggers `INT_DIVIDE_BY_ZERO`
- Otherwise, updates ZF, SF and clears CF, OF

---

### MOD rd, rs1, rs2

``` 
rd = rs1 % rs2
```

- If `rs2 == 0`: triggers `INT_DIVIDE_BY_ZERO`

---

### AND / OR / XOR / NOT

Logical instructions update ZF and SF, and clear CF and OF.

---

### SHL / SHR / SAR rd, rs1, rs2

```
sh = rs2 & 31
SHL: rd = (uint32)rs1 << sh
SHR: rd = (uint32)rs1 >> sh
SAR: rd = (int32)rs1 >> sh
```

- Update ZF and SF
- Clear CF and OF

---

### ROL / ROR rd, rs1, rs2

```
sh = rs2 & 31
ROL: rd = ((uint32)rs1 << sh) | ((uint32)rs1 >> (32 - sh))
ROR: rd = ((uint32)rs1 >> sh) | ((uint32)rs1 << (32 - sh))
```

- If `sh == 0`, result is unchanged.
- Update ZF and SF
- Clear CF and OF

---

### CAS rd, rs1, rs2, imm

Atomic compare-and-swap on 32-bit memory word:

```
addr = rs1 + imm
old  = MEM32[addr]
if (old == rd_before) MEM32[addr] = rs2
rd = old
```

- Atomic RMW
- Full fence semantics (sequentially consistent)
- Flags: success => `ZF=1`, failure => `ZF=0` (other flags cleared)

---

### XADD rd, rs1, rs2, imm

Atomic fetch-add on 32-bit memory word:

```
addr = rs1 + imm
old  = MEM32[addr]
MEM32[addr] = old + rs2
rd = old
```

- Atomic RMW
- Full fence semantics (sequentially consistent)
- Flags updated as integer ADD of `(old + rs2)`

---

### XCHG rd, rs1, rs2, imm

Atomic exchange on 32-bit memory word:

```
addr = rs1 + imm
old  = MEM32[addr]
MEM32[addr] = rs2
rd = old
```

- Atomic RMW
- Full fence semantics (sequentially consistent)

---

### LDAR rd, rs1, imm

Acquire-load from 32-bit memory word:

```
addr = rs1 + imm
rd = MEM32[addr]
```

- Load with acquire semantics

---

### STLR rd, rs1, imm

Release-store to 32-bit memory word:

```
addr = rs1 + imm
MEM32[addr] = rd
```

- Store with release semantics

---

### FENCE

Full memory fence.

- Sequentially consistent fence semantics

---

### PAUSE

Hint instruction for spin-wait loops.

- May yield host thread execution

---

### INC rd

```
rd = rd + 1
```

Updates ZF, SF, CF, OF (same as `ADD`).

---

### ADDI / SUBI rd, rs1, imm

```
ADDI: rd = rs1 + imm
SUBI: rd = rs1 - imm
```

- Same FLAGS semantics as `ADD` / `SUB`

---

### ANDI / ORI / XORI rd, rs1, imm

Bitwise immediate operations.

- Update ZF and SF
- Clear CF and OF

---

### SHLI / SHRI / ROLI / RORI rd, rs1, imm

```
sh = imm & 31
SHLI: rd = (uint32)rs1 << sh
SHRI: rd = (uint32)rs1 >> sh
ROLI: rd = ((uint32)rs1 << sh) | ((uint32)rs1 >> (32 - sh))
RORI: rd = ((uint32)rs1 >> sh) | ((uint32)rs1 << (32 - sh))
```

- If `sh == 0`, result is unchanged.
- Update ZF and SF
- Clear CF and OF

---

## 7. Floating-Point (F32) Instructions

All floating-point instructions interpret register contents as **IEEE 754 binary32** (32-bit float).
Values are stored in integer registers **bitwise**, with no conversion unless explicitly stated.

### FADD / FSUB / FMUL / FDIV rd, rs1, rs2

```
rd = (float)rs1 ⊕ (float)rs2
```

Where ⊕ is `+`, `-`, `*`, or `/`.

- Updates ZF and SF based on the float result (ZF = 1 if result is +0.0 or -0.0, SF = 1 if result < 0)
- Clears CF and OF
- Division by 0 follows IEEE 754 behavior (no interrupt)

---

### FNEG / FABS / FSQRT rd, rs1

```
FNEG:  rd = -rs1
FABS:  rd = abs(rs1)
FSQRT: rd = sqrt(rs1)
```

- Updates ZF and SF based on the float result
- Clears CF and OF

---

### FCMP rd, rs1

```
compare (float)rd vs (float)rs1
```

Flags are set as:

- If either operand is NaN: **OF = 1**, all other flags cleared
- Else if equal: **ZF = 1**
- Else if rd < rs1: **SF = 1**
- Else if rd > rs1: **CF = 1**

---

### ITOF rd, rs1

```
rd = (float)(int32)rs1
```

- Updates ZF and SF based on the float result
- Clears CF and OF

---

### FTOI rd, rs1

```
rd = (int32)(float)rs1
```

- If input is NaN or outside int32 range: `rd = 0`, **OF = 1**, ZF/SF/CF cleared
- Otherwise: updates ZF and SF based on the integer result; clears CF and OF

---

### FLOAD32 rd, [rs1 + imm]

- Reads 32-bit value and **reinterprets bits** as float32
- Updates ZF and SF based on the float value; clears CF and OF

---

### FSTORE32 [rs1 + imm], rd

- Stores lower 32 bits of `rd` as raw float32 bits
- Does not modify FLAGS

---

## 8. Comparison Instructions

### CMP rd, rs1

``` 
tmp = rd - rs1
```

- No register is written
- ZF = 1 if operands are equal
- SF, CF, OF are set according to subtraction result

---

### CMPI rd, imm

``` 
tmp = rd - imm
```

Same semantics as `CMP`.

---

## 9. Control Flow Instructions

Absolute control-flow instructions use **absolute VM addresses** (`imm`).

### JMP imm

Unconditional jump.

---

### Conditional Jumps

| Instruction | Condition            |
|-------------|----------------------|
| JZ          | ZF == 1              |
| JNZ         | ZF == 0              |
| JC          | CF == 1              |
| JNC         | CF == 0              |
| JG          | ZF == 0 and SF == OF |
| JGE         | SF == OF             |
| JL          | SF != OF             |
| JLE         | ZF == 1 or SF != OF  |

Signed comparison semantics are used.

---

Relative control-flow instructions use signed PC-relative displacement:

```
target = current_instruction_address + imm
```

`current_instruction_address` is the address of the executing branch/call itself
(not the next instruction).

### RJMP imm

Unconditional relative jump.

### RCALL imm

Relative call:

- pushes return address (next instruction) to call stack
- jumps to `current_instruction_address + imm`

### CALLR rd

Register-indirect call:

- pushes return address (next instruction) to call stack
- jumps to absolute VM address in `rd`
- `rs1`, `rs2`, `imm` are unused (ignored by execution)

### Relative Conditional Jumps

Relative conditional jumps:

- `RJZ`: take branch when `ZF == 1`
- `RJNZ`: take branch when `ZF == 0`
- `RJC`: take branch when `CF == 1`
- `RJNC`: take branch when `CF == 0`
- `RJG`: take branch when `ZF == 0 and SF == OF`
- `RJGE`: take branch when `SF == OF`
- `RJL`: take branch when `SF != OF`
- `RJLE`: take branch when `ZF == 1 or SF != OF`

---

## 10. Memory Access

### LOAD rd, [rs1 + imm]

- Reads 8-bit value
- Zero-extends to 32 bit

---

### LOAD16 rd, [rs1 + imm]

- Reads 16-bit value
- Zero-extends to 32 bit
- Address must be 2-byte aligned

---

### LOADS8 rd, [rs1 + imm]

- Reads 8-bit value
- Sign-extends to 32 bit

---

### LOADS16 rd, [rs1 + imm]

- Reads 16-bit value
- Sign-extends to 32 bit
- Address must be 2-byte aligned

---

### LOAD32 rd, [rs1 + imm]

- Reads 32-bit value
- Address must be 4-byte aligned (misaligned access is undefined; current VM panics)

---

### LOADX rd, [rs1 + rs2 + imm]

Indexed 8-bit load (zero-extended to 32 bit).

---

### LOADX16 rd, [rs1 + rs2 + imm]

Indexed 16-bit load (zero-extended to 32 bit).

- Address must be 2-byte aligned

---

### LOADX32 rd, [rs1 + rs2 + imm]

Indexed 32-bit load.

---

### STORE / STORE16 / STORE32 / STOREX / STOREX16 / STOREX32

Write operations do not modify FLAGS.

`STORE16` requires 2-byte aligned address.
`STOREX16` requires 2-byte aligned address.

---

## 11. Stack Model

LampVM has **three independent stacks**:

| Stack      | Purpose                |
|------------|------------------------|
| Call Stack | CALL / RET             |
| Data Stack | PUSH / POP             |
| ISR Stack  | Interrupt context save |

All three stacks are implemented in memory-mapped stack regions (`CALL_STACK_BASE`, `DATA_STACK_BASE`, `ISR_STACK_BASE`).

---

## 12. Interrupt Model

### Interrupt Vector Table (IVT)

- Address: `IVT_BASE + int_no * 8`
- Maximum vectors: 256

---

### Reserved Interrupt Numbers (Platform ABI)

The following interrupt numbers are currently reserved by the LampVM platform environment:

| `int_no` | Name | Meaning |
|----------|------|---------|
| `0x00` | `INT_KEYBOARD` | Keyboard input interrupt |
| `0x01` | `INT_DIVIDE_BY_ZERO` | Raised by `DIV`/`MOD` when divisor is zero |
| `0x02` | `INT_DISK_COMPLETE` | Disk device completion interrupt |
| `0x03` | `INT_SERIAL` | Serial RX/TX interrupt |
| `0x04` | `INT_TIMER` | Timer interrupt |

Unassigned vectors are available for software/platform use unless reserved by a future platform ABI revision.

---

### ISR ABI (Fixed)

On interrupt entry, VM automatically:

1. Sets `r31 = int_no`
2. Saves context to ISR stack (IP, FLAGS, r0–r31)
3. Jumps to ISR entry
4. Sets `in_interrupt = 1`

ISR must return using `IRET`.

Nested interrupts are **not supported**.

Software interrupts are raised with:

- `INT rd`, where `rd` provides `int_no`
- `INTI imm`, where `imm` provides `int_no` (other fields are ignored)

---

## 13. IO Instructions

### IN rd, [rs1]

Reads from IO port.

---

### OUT [rs1], rd

Writes to IO port.

Out-of-range access causes VM panic.

---

## 14. System Instructions

### HALT

Stops VM execution.

### CPUID rd

Stores current core id into `rd`.

### STARTAP rd, rs1, imm

Starts an AP core:

```
target = rd
entry  = rs1 + imm
```

- Only BSP (`core 0`) can start APs.
- Invalid target core id is ignored.

### IPI rd, rs1

Sends interrupt `rs1` to target core `rd`.

- Targeted, per-core interrupt delivery.
- Invalid target or vector is ignored.

---

## 15. Stack Operations

### POP rd

Pops one value from the data stack into `rd`.

---

### PUSH rd

Pushes the value in `rd` onto the data stack.

---

## 16. Undefined Behavior

The following are undefined and may cause VM panic:

- Undefined opcode
- IRET outside ISR
- Out-of-range memory access
- Invalid IO port
- Misaligned LOAD32/STORE32
- Misaligned LOAD16/STORE16/LOADS16/LOADX16/STOREX16
- Misaligned atomic address (`CAS/XADD/XCHG/LDAR/STLR`)

---

## 17. Compatibility Policy

Once defined in this document, instruction semantics are considered ABI-stable.
Existing instructions must not change behavior.

The current frozen opcode map is defined through `0x60` (`STOREX16`).

Extensions must use new opcodes or mechanisms.
