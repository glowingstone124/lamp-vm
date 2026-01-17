# lamp-vm

## Compile

The project's Cmake file is currently forced to compile in aarch64 mode since I am using a Macbook, some changes must be
done if you want to compile it by yourself.

## Features

Assembler could be found at (lampvm-assembler)[https://github.com/glowingstone124/lampvm-toolchain]

### Panic

VM will panic if a bad instruction was executed, and a debug message will be print.

### Current Memory Mapping

| Type        | Start Addr                 | End Addr   | Size                   | Usage           |
|-------------|----------------------------|------------|------------------------|-----------------|
| IVT         | 0x000000                   | 0x0007FF   | 2048 B                 | IVT, 8byte each |
| CALL_STACK  | 0x000800                   | 0x0008FF   | 256 B                  | Call Stack      |
| DATA_STACK  | 0x000900                   | 0x0009FF   | 256 B                  | Data Stack      |
| PROGRAM     | 0x000A00                   | FB start   | ~4 MB                  | Program         |
| FrameBuffer | Memory End - 800 * 600 * 4 | Memory End | depends on screen size | FrameBuffer     |

...

### Interrupt Tables(IVT) Mapping

In default, LampVM supports 256 interrupt ids. This vector starts at memory address 0x0, since memory is actually a
segment on heap space.

Keyboard Input now has the highest priority, it's located on 0x00. Edit 0x00 first in your program to configure the
address handling Keyboard Interrupt.

## Roadmap

Write Wiki

Implements a standard VGA-like screen.

Implements a virtual hard disk.

Create a assembler and make a subset of C.

## Instructions

### Instructions Mapping

A single instruction's length is 64bit.

| Type(8)    | rd(8)        | rs1(8)           | rs2(8)  | imm(32)             |
|------------|--------------|------------------|---------|---------------------|
| Calc       | destination  | source1          | source2 | none                |
| LOAD       | target       | address register | /       | Immediate Value     |
| STORE      | destination  | address register | /       | Immediate Value     |
| JMP / JZ   | /            | /                | /       | Jump Instruction Id |
| PUSH / POP | register     | /                | /       | /                   |
| CMP        | register     | register         | /       | Immediate Value     |
| MOV        | register     | register         | /       | /                   |
| MOVI       | register     | /                | /       | Immediate Value     |
| MEMSET     | Start Addr   | Value            | /       | Length              |
| MEMCPY     | destination  | source           | /       | Length              |
| IN         | receiver     | address          | /       | /                   |
| OUT        | value        | address          | /       | /                   |
| INT        | interrupt_id | /                | /       | /                   |
| IRET       | /            | /                | /       | /                   |

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

STORE/LOAD: Use relative addressing to store/load a value into memory.

MEMSET: Fill a memory segment with the value in rs1; the segment starts at the address in rd and has a length of imm.

MEMCPY: Copy a segment of memory to another destination.

IN: Use rd register to receive a data from IO address rs1.

OUT: Send rd register's data to IO address rs1.

INT: Submit a software-cause interrupt

IRET: Return from a interrupt.
