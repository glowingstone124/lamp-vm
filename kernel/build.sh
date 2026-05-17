#!/usr/bin/env bash
set -e
: "${LAMP_CLANG:?Error: LAMP_CLANG is not defined}"
: "${LAMP_LD:?Error: LAMP_LD is not defined}"

if ! command -v "$LAMP_CLANG" >/dev/null 2>&1; then
  echo "Error: LAMP_CLANG executable not found: $LAMP_CLANG"
  exit 1
fi

if ! command -v "$LAMP_LD" >/dev/null 2>&1; then
  echo "Error: LAMP_LD executable not found: $LAMP_LD"
  exit 1
fi

mkdir -p build-kernel

for f in kernel/src/*.c; do
  "$LAMP_CLANG" --target=lamp-unknown-unknown \
    -ffreestanding -fno-builtin -fno-stack-protector -fomit-frame-pointer \
    -fno-optimize-sibling-calls -O0 \
    -Ikernel/include -c "$f" \
    -o "build-kernel/$(basename "$f" .c).o"
done

"$LAMP_LD" -T kernel/linker.ld -e kernel_entry \
  build-kernel/*.o -o build-kernel/kernel.elf

test -f disk.img || truncate -s 512M disk.img

dd if=build-kernel/kernel.elf of=disk.img \
   bs=512 seek=1 conv=notrunc
