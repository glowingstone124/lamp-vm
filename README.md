# lamp-vm

## Compile

Use Cmake VERSION > 4.1.

## Features

VM will panic if a bad instruction was executed, and a debug message will be print.

## Roadmap

Implements a standard VGA-like screen.

Implements a virtual harddisk

Isolate assembly instructions into separate files.
## Instructions

### Instructions Mapping

A single instruction's length is 64bit.

| Type(8)    | rd(8)       | rs1(8)           | rs2(8)  | imm(32)             |
|------------|-------------|------------------|---------|---------------------|
| Calc       | destination | source1          | source2 | none                |
| LOAD       | target      | address register | /       | Immediate Value     |
| STORE      | destination | address register | /       | Immediate Value     |
| JMP / JZ   | /           | /                | /       | Jump Instruction Id |
| PUSH / POP | register    | /                | /       | /                   |
| CMP        | register    | register         | /       | Immediate Value     |
| MOV        | register    | register         | /       | /                   |
| MOVI       | register    | /                | /       | Immediate Value     |
| MEMSET     | Start Addr  | Value            | /       | Length              |
| MEMCPY     | destination | source           | /       | Length              |
| IN         | receiver    | address          | /       | /                   |
| OUT        | value       | address          | /       | /                   |

### Instructions Usage:

Calc: ALL math calculations such as ADD, SUB, MUL.

LOAD: Load a value in a memory address into other register. Address is `address register(rs1)'s value + imm`.

STORE: Store a value to a memory address. Address is `address register(rs1)'s value + imm`.

JMP: Jump to a command.

JZ: Jump to a command if flags(ZFLAGS) is 0.

PUSH: Push a register into stack.

POP: Pop a value out of the stack.

CMP: Compare two values and set ZFLAGS. If imm is 0, compare rd and rs1, or compare rd and imm. If they are equal, ZFLAG
will be 0, otherwise it will be 1.

MOV/MOVI: Move rs1's value or a immediate value to rd.

STORE/LOAD v.s. STORE_IND/LOAD_IND: The former uses a constant value which is defined at the beginning of this cycle,
unlike the latter uses the value in register dynamically.**

MEMSET: Fill a memory segment with the value in rs1; the segment starts at the address in rd and has a length of imm.

MEMCPY: Copy a segment of memory to another destination.

IN: Use rd register to receive a data from IO address rs1.

OUT: Send rd register's data to IO address rs1.
