cmake_minimum_required(VERSION 3.20)
project(vm C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(SOURCES
        vm.c
        debug.c
        io_devices/frame/frame.c
        io.c
        loadbin.c
        panic.c
        io_devices/disk/disk.c
        interrupt.c
        io_devices/frame/terminalin.c
        memory.c
        io_devices/vga_display/display.c
        mmio.c
        io_devices/vga_display/vga_mmio_register.c
        io_devices/time/time_mmio_register.c
        io_devices/sysinfo/sysinfo_mmio_register.c
        io_devices/time/timer.c
        float.c
        selftest.c
)

add_executable(vm ${SOURCES})
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
    # Avoid x86-only intrinsics headers on Apple Silicon.
    target_compile_definitions(vm PRIVATE SDL_DISABLE_IMMINTRIN_H SDL_DISABLE_MMINTRIN_H)
endif()
find_package(SDL2 CONFIG QUIET)

if(SDL2_FOUND)
    if(TARGET SDL2::SDL2)
        target_link_libraries(vm PRIVATE SDL2::SDL2)
    else()
        if(DEFINED SDL2_INCLUDE_DIRS)
            target_include_directories(vm PRIVATE ${SDL2_INCLUDE_DIRS})
        endif()
        if(DEFINED SDL2_LIBRARIES)
            target_link_libraries(vm PRIVATE ${SDL2_LIBRARIES})
        endif()
    endif()
    if(TARGET SDL2::SDL2main)
        target_link_libraries(vm PRIVATE $<$<PLATFORM_ID:Windows>:SDL2::SDL2main>)
    endif()
else()
    find_package(SDL2 QUIET)
    if(SDL2_FOUND)
        target_include_directories(vm PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(vm PRIVATE ${SDL2_LIBRARIES})
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL2 REQUIRED sdl2)
        target_include_directories(vm PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(vm PRIVATE ${SDL2_LIBRARIES})
        target_compile_options(vm PRIVATE ${SDL2_CFLAGS_OTHER})
    endif()
endif()

# ----------------------------
# Threads (pthreads on Linux, etc.)
# ----------------------------
find_package(Threads REQUIRED)
target_link_libraries(vm PRIVATE Threads::Threads)

# ----------------------------
# libm for sqrtf on Linux (safe to link on Unix)
# ----------------------------
if(UNIX AND NOT APPLE)
    target_link_libraries(vm PRIVATE m)
    # Ensure POSIX time APIs are declared before any system headers on Linux.
    target_compile_definitions(vm PRIVATE _POSIX_C_SOURCE=200809L)
endif()

if(MSVC)
    target_compile_options(vm PRIVATE /W4 /WX)
else()
    target_compile_options(vm PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_definitions(vm PRIVATE RELEASE_BUILD)
    if(NOT MSVC)
        target_compile_options(vm PRIVATE -O3 -flto)
        target_link_options(vm PRIVATE -flto)
    endif()
else()
    target_compile_definitions(vm PRIVATE DEBUG_BUILD)
    if(NOT MSVC)
        target_compile_options(vm PRIVATE -g -O0)
    endif()
endif()

option(VM_DEBUG "Enable VM debug features" OFF)
if(VM_DEBUG)
    target_compile_definitions(vm PRIVATE VM_DEBUG VM_MEMCHECK VM_INSTR_STATS)
endif()
