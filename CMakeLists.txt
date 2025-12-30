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
        io_devices/disk/disk.h
        interrupt.c
        interrupt.h)
target_compile_options(vm PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(vm PRIVATE
            -O3
            -march=native
            -flto
    )
    target_link_options(vm PRIVATE -flto)
else()
    target_compile_options(vm PRIVATE -g -O0)
endif()