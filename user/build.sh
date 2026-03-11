#!/usr/bin/env bash
set -e

: "${LAMP_CLANG:?Error: LAMP_CLANG is not defined}"
: "${LAMP_LD:?Error: LAMP_LD is not defined}"

mkdir -p build-user

"$LAMP_CLANG" --target=lamp-unknown-unknown \
  -ffreestanding -fno-builtin -fno-stack-protector -O0 \
  -Iuser/include -c user/crt/start.c -o build-user/start.o

"$LAMP_CLANG" --target=lamp-unknown-unknown \
  -ffreestanding -fno-builtin -fno-stack-protector -O0 \
  -Iuser/include -c user/lib/libsys.c -o build-user/libsys.o

"$LAMP_CLANG" --target=lamp-unknown-unknown \
  -ffreestanding -fno-builtin -fno-stack-protector -O0 \
  -Iuser/include -c user/apps/hello.c -o build-user/hello.o

"$LAMP_LD" -T user/linker.ld -e _start \
  build-user/start.o build-user/libsys.o build-user/hello.o \
  -o build-user/hello.elf

echo "built: build-user/hello.elf"
