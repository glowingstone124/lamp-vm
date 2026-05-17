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
  -Iuser/sysroot/include -Iuser/include -c user/lib/libc_compat.c -o build-user/libc_compat.o

"$LAMP_CLANG" --target=lamp-unknown-unknown \
  -ffreestanding -fno-builtin -fno-stack-protector -O0 \
  -Iuser/include -c user/apps/hello.c -o build-user/hello.o

"$LAMP_CLANG" --target=lamp-unknown-unknown \
  -ffreestanding -fno-builtin -fno-stack-protector -O0 \
  -Iuser/include -c user/apps/echo.c -o build-user/echo.o

"$LAMP_CLANG" --target=lamp-unknown-unknown \
  -ffreestanding -fno-builtin -fno-stack-protector -O0 \
  -Iuser/include -c user/apps/vfork_exec.c -o build-user/vfork_exec.o

"$LAMP_CLANG" --target=lamp-unknown-unknown \
  -ffreestanding -fno-builtin -fno-stack-protector -O0 \
  -Iuser/include -c user/apps/cat.c -o build-user/cat.o

"$LAMP_CLANG" --target=lamp-unknown-unknown \
  -ffreestanding -fno-builtin -fno-stack-protector -O0 \
  -Iuser/include -c user/apps/pipe_exec.c -o build-user/pipe_exec.o

"$LAMP_CLANG" --target=lamp-unknown-unknown \
  -ffreestanding -fno-builtin -fno-stack-protector -O0 \
  -Iuser/include -c user/apps/pwd.c -o build-user/pwd.o

"$LAMP_CLANG" --target=lamp-unknown-unknown \
  -ffreestanding -fno-builtin -fno-stack-protector -O0 \
  -Iuser/include -c user/apps/ls.c -o build-user/ls.o

"$LAMP_LD" -T user/linker.ld -e _start \
  build-user/start.o build-user/libsys.o build-user/hello.o \
  -o build-user/hello.elf

"$LAMP_LD" -T user/linker.ld -e _start \
  build-user/start.o build-user/libsys.o build-user/echo.o \
  -o build-user/echo.elf

"$LAMP_LD" -T user/linker.ld -e _start \
  build-user/start.o build-user/libsys.o build-user/vfork_exec.o \
  -o build-user/vfork_exec.elf

"$LAMP_LD" -T user/linker.ld -e _start \
  build-user/start.o build-user/libsys.o build-user/cat.o \
  -o build-user/cat.elf

"$LAMP_LD" -T user/linker.ld -e _start \
  build-user/start.o build-user/libsys.o build-user/pipe_exec.o \
  -o build-user/pipe_exec.elf

"$LAMP_LD" -T user/linker.ld -e _start \
  build-user/start.o build-user/libsys.o build-user/pwd.o \
  -o build-user/pwd.elf

"$LAMP_LD" -T user/linker.ld -e _start \
  build-user/start.o build-user/libsys.o build-user/ls.o \
  -o build-user/ls.elf

echo "built: build-user/hello.elf"
echo "built: build-user/echo.elf"
echo "built: build-user/vfork_exec.elf"
echo "built: build-user/cat.elf"
echo "built: build-user/pipe_exec.elf"
echo "built: build-user/pwd.elf"
echo "built: build-user/ls.elf"
echo "built: build-user/libc_compat.o"
