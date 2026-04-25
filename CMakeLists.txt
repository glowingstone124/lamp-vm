cmake_minimum_required(VERSION 3.20)
project(vm C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ------------------------------------------------
# Do not support MSVC
# ------------------------------------------------
if(MSVC)
    message(FATAL_ERROR "MSVC is not supported. Please use Clang or GCC.")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug
        RelWithDebInfo
        Release
        MinSizeRel
)

option(VM_DEBUG "Enable VM debug features" OFF)
option(VM_ASAN "Enable AddressSanitizer" OFF)
option(VM_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(VM_LTO "Enable LTO/IPO in Release builds" ON)
option(VM_PGO_GENERATE "Build with PGO profile generation" OFF)
option(VM_PGO_USE "Build with PGO profile use" OFF)

if(VM_PGO_GENERATE AND VM_PGO_USE)
    message(FATAL_ERROR "VM_PGO_GENERATE and VM_PGO_USE cannot both be ON")
endif()

set(SOURCES
        src/vm.c
        src/debug.c
        src/io_devices/frame/frame.c
        src/io.c
        src/loadbin.c
        src/panic.c
        src/io_devices/disk/disk.c
        src/interrupt.c
        src/io_devices/frame/terminalin.c
        src/memory.c
        src/io_devices/vga_display/display.c
        src/mmio.c
        src/io_devices/vga_display/vga_mmio_register.c
        src/io_devices/time/time_mmio_register.c
        src/io_devices/intc/intc_mmio_register.c
        src/io_devices/iommu/iommu_mmio_register.c
        src/io_devices/mmu/mmu_mmio_register.c
        src/io_devices/sysinfo/sysinfo_mmio_register.c
        src/io_devices/time/timer.c
        src/float.c
        src/selftest.c
        src/vnc.c
        src/vnc.h
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

find_package(Threads REQUIRED)
target_link_libraries(vm PRIVATE Threads::Threads)

if(UNIX AND NOT APPLE)
    target_link_libraries(vm PRIVATE m)
    # Ensure POSIX time APIs are declared before any system headers on Linux.
    target_compile_definitions(vm PRIVATE _POSIX_C_SOURCE=200809L)
endif()

target_compile_options(vm PRIVATE
        -fno-omit-frame-pointer
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wstrict-prototypes
        -Wmissing-prototypes
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(vm PRIVATE DEBUG_BUILD)
    target_compile_options(vm PRIVATE -g3 -Og)
elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    target_compile_definitions(vm PRIVATE RELEASE_BUILD)
    target_compile_options(vm PRIVATE -g -O2)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_definitions(vm PRIVATE RELEASE_BUILD)
    target_compile_options(vm PRIVATE -O2)
elseif(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    target_compile_definitions(vm PRIVATE RELEASE_BUILD)
    target_compile_options(vm PRIVATE -Os)
else()
    target_compile_definitions(vm PRIVATE RELEASE_BUILD)
    target_compile_options(vm PRIVATE -g -O2)
endif()

if(VM_DEBUG)
    target_compile_definitions(vm PRIVATE VM_DEBUG VM_MEMCHECK VM_INSTR_STATS)
endif()

if(VM_ASAN)
    target_compile_options(vm PRIVATE -fsanitize=address)
    target_link_options(vm PRIVATE -fsanitize=address)
endif()

if(VM_UBSAN)
    target_compile_options(vm PRIVATE -fsanitize=undefined)
    target_link_options(vm PRIVATE -fsanitize=undefined)
endif()

if(VM_PGO_GENERATE)
    target_compile_options(vm PRIVATE -fprofile-generate)
    target_link_options(vm PRIVATE -fprofile-generate)
endif()

if(VM_PGO_USE)
    target_compile_options(vm PRIVATE -fprofile-use -fprofile-correction)
    target_link_options(vm PRIVATE -fprofile-use)
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND VM_LTO)
    if(ipo_supported)
        set_property(TARGET vm PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(WARNING "IPO/LTO not supported: ${ipo_error}")
    endif()
endif()
