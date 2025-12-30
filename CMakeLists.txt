cmake_minimum_required(VERSION 4.1)
project(vm C)

set(CMAKE_C_STANDARD 11)

add_executable(vm
        register_vm.c
        vm.h
        stack.h
        fetch.h
        instruction.h
        io_devices/frame/frame.c
        io.h
        io.c
        loadbin.c
        loadbin.h
        panic.c
        panic.h
        io_devices/disk/disk.c
        io_devices/disk/disk.h)
