# BIOS Build Guide

This document describes how to build the LampVM BIOS source (`bios/bios.c`).

> This project may split into a separate repository in the future.

## Prerequisites

This project needs a custom LLVM toolchain to compile.

## Artifact A: Legacy Flat `boot.bin` (VM `--bin` format)

Format:

- 24-byte little-endian header (`TEXT_BASE/TEXT_SIZE/DATA_BASE/DATA_SIZE/BSS_BASE/BSS_SIZE`)
- followed by text bytes
- followed by data bytes

### Build

```bash
$LAMP_CLANG --target=lamp-unknown-unknown -c bios/bios.c -o bios/bios.o

# Emit legacy flat boot image.
$LAMP_LD -T bios/boot_flat.ld bios/bios.o -o bios/boot.bin
```

### Run with VM

```bash
./build/lampvm --bin bios/boot.bin --smp 1
```

## Artifact B: ELF (`bios.elf`)

Useful for symbol/section inspection and debugging.

```bash
$LAMP_CLANG --target=lamp-unknown-unknown -c bios/bios.c -o bios/bios.o
$LAMP_LD bios/bios.o -e _start -o bios/bios.elf
```

To emit text assembly only:

```bash
$LAMP_CLANG --target=lamp-unknown-unknown -S bios/bios.c -o bios/bios.s
```

## Boot Contract Reminder

- BIOS itself is loaded by VM from `--bin` file.
- BIOS loads kernel ELF from `disk.img` at `LBA 1` (see `bios/bios.c`, `bios.md`).
