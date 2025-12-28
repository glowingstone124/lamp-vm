cmake_minimum_required(VERSION 4.1)
project(vm C)

set(CMAKE_C_STANDARD 11)

add_executable(vm
        register_vm.c
        vm.h
        stack.h
        fetch.h
        instruction.h
        frame.c
        io.h
        io.c)
